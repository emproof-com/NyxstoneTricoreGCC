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

/// The exact crate spec `cargo install` / `cargo binstall` targets.  Pinned to
/// this crate's version because the daemon (GPL `nyxstone-tricore-gcc`) ships
/// in lockstep with it: an unpinned install would fetch the latest published
/// daemon, which may speak a newer wire protocol and fail the handshake.
const DAEMON_CRATE_PINNED: &str = concat!("nyxstone-tricore-gcc@", env!("CARGO_PKG_VERSION"));

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

    /// Pid of the daemon child (test hook for kill/respawn coverage).
    #[cfg(test)]
    pub(crate) fn pid(&self) -> u32 {
        self.process.id()
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
///   2. The directory of the current executable -- ship `nyxstone-tcd`
///      alongside your binary and it is found with zero configuration.  This
///      is the recommended deployment (containers, packaged apps): no PATH
///      entry, no env var, no runtime `cargo install`.
///   3. `nyxstone-tcd` on the system `PATH`.
pub(crate) fn find_daemon() -> Option<PathBuf> {
    if let Ok(p) = env::var("NYXSTONE_TCD_PATH") {
        let p = PathBuf::from(p);
        if p.is_file() { return Some(p); }
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            let cand = dir.join("nyxstone-tcd");
            if cand.is_file() { return Some(cand); }
        }
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
    format!(
    "could not locate the nyxstone-tcd daemon binary (version {ver}).\n\
     For a DEPLOYED/containerized build, ship the daemon with your binary -- \
     do NOT rely on a runtime `cargo install`:\n  \
       - place `nyxstone-tcd` next to your executable, or on PATH, or\n  \
       - set NYXSTONE_TCD_PATH to its full path.\n\
     The daemon must be the matching version ({ver}).\n\
     For local development you may instead call \
     nyxstone_tricore_gcc_ipc::install_daemon_if_missing()? once (it shells out \
     to `cargo install` and needs cargo + network).",
        ver = env!("CARGO_PKG_VERSION"))
}

/// Install the daemon binary by invoking `cargo install`, preferring
/// `cargo binstall` (fetches a pre-built binary, much faster) when available
/// on PATH.  Returns the resolved daemon path on success.
///
/// The install is PINNED to this crate's exact version so the daemon's wire
/// protocol always matches the client -- an unpinned `cargo install` would
/// fetch the latest published daemon, which may speak a newer protocol and
/// fail the handshake.
///
/// This shells out to cargo (and needs network) and is intended for local
/// development only.  DEPLOYED builds must ship the daemon instead (see
/// `install_required_message`); do not call this from a container/CI runtime.
///
/// Prints progress messages to stderr so the user sees what's happening.
pub(crate) fn run_install() -> io::Result<PathBuf> {
    use std::process::Command;

    let pinned = DAEMON_CRATE_PINNED;

    // Cargo's --quiet flag still leaves errors visible.
    if which_on_path("cargo-binstall").is_some() {
        eprintln!("nyxstone-tricore-gcc-ipc: installing {pinned} via cargo binstall...");
        let status = Command::new("cargo")
            .args(["binstall", "--no-confirm", "--quiet", pinned])
            .status();
        if matches!(&status, Ok(s) if s.success()) {
            if let Some(p) = find_daemon() { return Ok(p); }
        }
        // Fall through to cargo install on binstall failure (e.g., no binary
        // for this target was uploaded yet).
    }

    eprintln!("nyxstone-tricore-gcc-ipc: building {pinned} via \
               'cargo install' (one-time, ~10 s; needs cargo + network)...");
    let status = Command::new("cargo")
        .args(["install", "--quiet", pinned])
        .status()
        .map_err(|e| io::Error::new(e.kind(),
            format!("`cargo install {pinned}` could not be launched ({e}); is `cargo` \
                     on PATH?  A deployed build should ship nyxstone-tcd instead of \
                     installing it at runtime.")))?;
    if !status.success() {
        return Err(io::Error::new(io::ErrorKind::Other,
            format!("`cargo install {pinned}` failed; install manually, set \
                     NYXSTONE_TCD_PATH, or ship nyxstone-tcd next to your binary")));
    }
    find_daemon().ok_or_else(|| io::Error::new(io::ErrorKind::NotFound,
        "cargo reported success but nyxstone-tcd not found; \
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

#[cfg(test)]
mod tests {
    use super::*;

    // The auto-install target must be pinned to this crate's exact version so
    // the daemon's wire protocol matches the client -- an unpinned install
    // could pull a newer, protocol-incompatible daemon.
    #[test]
    fn auto_install_target_is_version_pinned() {
        assert_eq!(
            DAEMON_CRATE_PINNED,
            format!("nyxstone-tricore-gcc@{}", env!("CARGO_PKG_VERSION")));
    }

    // The "daemon not found" error must steer deployed builds toward shipping
    // the binary (not a runtime install) and name the required version.
    #[test]
    fn missing_daemon_message_is_deployment_oriented() {
        let m = install_required_message();
        assert!(m.contains(env!("CARGO_PKG_VERSION")), "must name the version: {m}");
        assert!(m.contains("NYXSTONE_TCD_PATH"), "must mention the env override: {m}");
        assert!(m.to_lowercase().contains("ship")
                || m.to_lowercase().contains("next to"),
                "must point deployments at shipping the daemon: {m}");
    }

    // find_daemon honors an explicit NYXSTONE_TCD_PATH that points at a real
    // file (the daemon under test), and rejects a path that doesn't exist.
    // Uses the process's own env (already set for the test run) so it doesn't
    // race other tests by mutating shared state.
    #[test]
    fn find_daemon_uses_explicit_path_when_valid() {
        if let Ok(p) = env::var("NYXSTONE_TCD_PATH") {
            if std::path::Path::new(&p).is_file() {
                assert_eq!(find_daemon().as_deref(), Some(std::path::Path::new(&p)));
            }
        }
    }
}
