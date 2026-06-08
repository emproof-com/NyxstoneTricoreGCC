//! Lazy daemon spawn: one `nyxstone-tcd` child per `NyxstoneTricoreGCC`
//! instance, owned by that instance.  The child receives an inherited
//! socketpair fd (no filesystem socket) and its lifetime is tied to that
//! socket — *not* to the thread that spawned it.
//!
//! Lifecycle:
//!   - `Daemon::spawn()` does `socketpair()` then fork+execs the daemon binary
//!     with `NYXSTONE_TCD_FD={child_fd}` in env.  `FD_CLOEXEC` is cleared on the
//!     child fd from inside the child (`pre_exec`), so the fd survives exec
//!     without ever appearing cleared in the parent's fd table — otherwise a
//!     concurrent thread's fork+exec could inherit and leak it.
//!   - The daemon serves requests until its `read()` returns EOF, then exits.
//!   - `Drop` shuts the socket down → daemon `read()` returns 0 → daemon exits;
//!     we then `wait()` the child to reap it.
//!   - If the whole process dies (crash, kill -9, normal exit) the OS closes
//!     the socket fd → daemon gets EOF and exits, reaped by init.
//!
//! We deliberately do NOT use `PR_SET_PDEATHSIG`: on Linux it fires when the
//! parent *thread* terminates, not the parent process.  A daemon spawned on a
//! short-lived worker thread (parallel test runners, thread pools, or any
//! instance cached past the thread that created it) would then be killed while
//! still in use, surfacing as "Broken pipe" / "Connection reset by peer".  The
//! socket-EOF lifecycle above is correctly process-scoped.

use std::env;
use std::io;
use std::os::unix::io::{AsRawFd, IntoRawFd, RawFd};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

/// A spawned daemon process plus its connected socket.  Dropping this closes
/// the socket and reaps the child.
pub(crate) struct Daemon {
    pub(crate) stream:  UnixStream,
    process: Child,
}

impl Daemon {
    pub(crate) fn spawn() -> io::Result<Daemon> {
        let bin = locate_daemon()?;
        spawn_with(&bin)
    }
}

impl Drop for Daemon {
    fn drop(&mut self) {
        // 1. Half-close write side so the daemon's read() gets EOF.
        let _ = self.stream.shutdown(std::net::Shutdown::Write);
        // 2. Wait briefly for graceful exit; force-kill if it lingers.
        for _ in 0..50 {
            if let Ok(Some(_)) = self.process.try_wait() { return; }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        let _ = self.process.kill();
        let _ = self.process.wait();
    }
}

/// Locate the `nyxstone-tcd` binary without installing.  Returns `None` if
/// it's absent.  Lookup order:
///   1. `NYXSTONE_TCD_PATH` env var (explicit override).
///   2. `nyxstone-tcd` on the system `PATH`.
pub(crate) fn find_daemon() -> Option<PathBuf> {
    if let Ok(p) = env::var("NYXSTONE_TCD_PATH") {
        let p = PathBuf::from(p);
        if p.is_file() { return Some(p); }
    }
    if let Some(path) = env::var_os("PATH") {
        for d in env::split_paths(&path) {
            let cand = d.join("nyxstone-tcd");
            if cand.is_file() { return Some(cand); }
        }
    }
    None
}

/// Human-readable "how to install" string used as the error message when
/// the daemon can't be located.
pub(crate) fn install_required_message() -> String {
    "could not locate nyxstone-tcd binary.\n\
     Install it with one of:\n  \
       cargo install nyxstone-tricore-gcc\n  \
       cargo binstall nyxstone-tricore-gcc             (if cargo-binstall is available)\n  \
       nyxstone_tricore_gcc_ipc::install_daemon_if_missing()?  (programmatic)\n\
     Or set NYXSTONE_TCD_PATH to the binary path.".to_string()
}

/// Install the daemon binary by invoking `cargo install nyxstone-tricore-gcc`,
/// preferring `cargo binstall` (fetches a pre-built binary, much faster) when
/// available on PATH.  Returns the resolved daemon path on success.
///
/// Prints progress messages to stderr so the user sees what's happening.
pub(crate) fn run_install() -> io::Result<PathBuf> {
    use std::process::Command;

    // Cargo's --quiet flag still leaves errors visible.
    let try_binstall = which_on_path("cargo-binstall").is_some();
    if try_binstall {
        eprintln!("nyxstone-tricore-gcc-ipc: installing nyxstone-tcd via cargo binstall...");
        let status = Command::new("cargo")
            .args(["binstall", "--no-confirm", "--quiet", "nyxstone-tricore-gcc"])
            .status();
        if matches!(&status, Ok(s) if s.success()) {
            if let Some(p) = find_daemon() { return Ok(p); }
        }
        // Fall through to cargo install on binstall failure (e.g., no binary
        // for this target was uploaded yet).
    }

    eprintln!("nyxstone-tricore-gcc-ipc: building nyxstone-tcd via \
               'cargo install nyxstone-tricore-gcc' (one-time, ~10 s)...");
    let status = Command::new("cargo")
        .args(["install", "--quiet", "nyxstone-tricore-gcc"])
        .status()
        .map_err(|e| io::Error::new(e.kind(),
            format!("`cargo install nyxstone-tricore-gcc` could not be launched ({e}); \
                     is `cargo` on PATH?")))?;
    if !status.success() {
        return Err(io::Error::new(io::ErrorKind::Other,
            "`cargo install nyxstone-tricore-gcc` failed; \
             install manually or set NYXSTONE_TCD_PATH".to_string()));
    }
    find_daemon().ok_or_else(|| io::Error::new(io::ErrorKind::NotFound,
        "cargo reported success but nyxstone-tcd not found on PATH; \
         check ~/.cargo/bin or set NYXSTONE_TCD_PATH".to_string()))
}

fn which_on_path(name: &str) -> Option<PathBuf> {
    let path = env::var_os("PATH")?;
    env::split_paths(&path)
        .map(|d| d.join(name))
        .find(|p| p.is_file())
}

/// Internal wrapper used by `Daemon::spawn()`, fail with the standard
/// "install required" message when the daemon isn't found.
fn locate_daemon() -> io::Result<PathBuf> {
    find_daemon().ok_or_else(|| io::Error::new(io::ErrorKind::NotFound,
        install_required_message()))
}

fn spawn_with(bin: &Path) -> io::Result<Daemon> {
    // Connected socket pair.  Both ends are FD_CLOEXEC by default; we keep the
    // parent end CLOEXEC and clear the child end's flag inside the child only
    // (see pre_exec below).
    let (parent, child) = UnixStream::pair()?;
    let child_fd: RawFd = child.into_raw_fd();

    let mut cmd = Command::new(bin);
    cmd.env("NYXSTONE_TCD_FD", child_fd.to_string());
    cmd.stdin(Stdio::null());
    cmd.stdout(Stdio::null());
    // Leave stderr inherited so user sees daemon-side panics during dev.

    // After fork, before exec, clear FD_CLOEXEC on the inherited socket so it
    // survives the exec.  Doing this here (child-side) rather than in the
    // parent avoids a race: were the flag cleared in the parent's fd table, a
    // concurrent thread spawning its own daemon could fork in that window and
    // leak this fd into an unrelated child.  Keeping it CLOEXEC in the parent
    // means no other spawn ever inherits it.
    //
    // Note: no PR_SET_PDEATHSIG — it is parent-*thread*-scoped on Linux and
    // would kill a still-in-use daemon when the spawning thread exits.  The
    // daemon instead exits on socket EOF (Drop, or the OS closing the fd when
    // the process dies), which is process-scoped.
    unsafe {
        cmd.pre_exec(move || {
            let flags = libc::fcntl(child_fd, libc::F_GETFD);
            if flags < 0 { return Err(io::Error::last_os_error()); }
            if libc::fcntl(child_fd, libc::F_SETFD, flags & !libc::FD_CLOEXEC) < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }

    let process = cmd.spawn().map_err(|e| {
        // SAFETY: we still own child_fd if spawn failed.
        unsafe { libc::close(child_fd); }
        io::Error::new(e.kind(),
            format!("failed to spawn {}: {}", bin.display(), e))
    })?;

    // Drop the parent's reference to the child fd; the child process has its
    // own duplicate via the inherited file table.
    unsafe { libc::close(child_fd); }

    // The `parent` UnixStream is what we use to talk to the daemon.
    // Default CLOEXEC=on is what we want for further exec()'s in the parent.
    let _ = parent.as_raw_fd();   // anchor lifetime
    Ok(Daemon { stream: parent, process })
}
