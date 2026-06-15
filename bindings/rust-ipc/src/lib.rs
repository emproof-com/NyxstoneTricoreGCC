//! NyxstoneTricoreGCC, MIT IPC bindings.
//!
//! Public API is identical, byte-for-byte, to the [GPL `nyxstone-tricore-gcc`
//! crate](https://crates.io/crates/nyxstone-tricore-gcc).  Switching license modes
//! is a one-line `Cargo.toml` change:
//!
//! ```toml
//! # max performance, GPL-3.0+:
//! nyxstone-tricore-gcc     = "0.2"
//!
//! # commercially permissive, ~150 k ops/s via IPC:
//! nyxstone-tricore-gcc-ipc = "0.2"
//! ```
//!
//! User code stays the same, same struct, same methods, same types.
//!
//! # How the IPC mode works
//!
//! Every `NyxstoneTricoreGCC::new()` call fork+execs its own
//! `nyxstone-tcd` daemon binary (GPL-3.0+, distributed separately, install
//! via `cargo install nyxstone-tricore-gcc`), then talks to it over a
//! `socketpair(2)` UNIX socket using a small custom binary protocol.
//!
//! The daemon:
//! - is **one per `NyxstoneTricoreGCC` instance**, owned by that instance.
//!   Instances (and processes) never share daemons.
//! - is **lazy**, spawned on `new()`, not at module init.
//! - has its **lifetime tied to its socket**: when the instance is dropped,
//!   or the parent process dies for any reason (crash, `kill -9`, normal
//!   exit), the OS closes the socket and the daemon exits on read-EOF.
//!   `PR_SET_PDEATHSIG` is deliberately *not* used — it is parent-*thread*-
//!   scoped on Linux and would kill a daemon whose spawning thread exited
//!   while the instance lives on (see `daemon.rs`).
//! - serves **strict FIFO** request/response, no out-of-order completion,
//!   no request IDs.  Multi-threaded callers serialize through a
//!   per-instance `Mutex`.
//!
//! # Timeouts and automatic respawn
//!
//! Every request is bounded by a socket read+write timeout, 30 s by default,
//! overridable via the `NYXSTONE_TCD_TIMEOUT_MS` env var (read once when the
//! instance is created; `0` disables the timeout).  After any transport
//! error — timeout, daemon crash/EOF, connection reset — the stream may be
//! desynced and is never reused: the daemon is torn down and respawned once
//! (with a fresh handshake) and the request is retried exactly once.  If the
//! retry also fails, the error is returned.  Daemon-level error replies
//! (gas diagnostics etc.) are valid responses and are never retried.
//!
//! # Locating the daemon
//!
//! 1. `NYXSTONE_TCD_PATH` env var (explicit path override).
//! 2. `nyxstone-tcd` on the system `PATH`.
//! 3. Fail with a helpful "install nyxstone-tricore-gcc" message otherwise.
//!
//! # Threading
//!
//! `NyxstoneTricoreGCC` is `Send + Sync`.  Shared instances serialize their
//! calls through an internal mutex (same contract as the GPL crate).

#![warn(missing_docs)]

/// Wire protocol shared between this crate (client side) and the
/// `nyxstone-tcd` daemon (server side, in the GPL crate).  Re-exported as
/// `pub` so the daemon binary can `use nyxstone_tricore_gcc_ipc::protocol::*;`
/// without duplicating the pack/unpack code.  End users of the high-level
/// API don't need to touch this module.
#[allow(missing_docs)]
pub mod protocol;
mod connection;
mod daemon;

use std::fmt;
use std::path::PathBuf;

use connection::Connection;

/// Address alias.  Same shape as the GPL crate.
pub type Address = u64;

/// Defines the location of a label by absolute address.  Same fields and
/// semantics as the GPL crate's `LabelDefinition`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LabelDefinition {
    /// Label name.
    pub name:    String,
    /// Absolute address.
    pub address: Address,
}

impl LabelDefinition {
    /// Convenience constructor matching the GPL crate.
    pub fn new(name: impl Into<String>, address: Address) -> Self {
        Self { name: name.into(), address }
    }
}

/// Symbol target of a relocation (gas `-r` style).
#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(missing_docs)]
pub struct RelocationSymbol {
    pub name:    String,
    pub address: Address,
}

/// One relocation entry produced by `assemble_with_relocs` / its instructions
/// variant.  Same shape as the GPL crate.
#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(missing_docs)]
pub struct RelocationInfo {
    pub offset: Address,
    pub addend: Option<i64>,
    pub symbol: RelocationSymbol,
    pub relocation_type: u32,
}

/// One disassembled (or freshly assembled) instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
#[allow(missing_docs)]
pub struct Instruction {
    pub address:  Address,
    pub assembly: String,
    pub bytes:    Vec<u8>,
}

/// Errors produced by [`NyxstoneTricoreGCC`].  Variants match the GPL crate's
/// `Error` enum exactly; IPC-specific failures (daemon spawn, lost socket,
/// protocol mismatch) are surfaced through `Error::Init` so user code that
/// matches on the GPL variants compiles unchanged.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
#[allow(missing_docs)]
pub enum Error {
    Init(String),
    AssembleFailed(String),
    SectionViolation(String),
    DisassembleFailed(String),
    Alloc,
    NullArg,
    Unknown(i32, String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Init(m)              => write!(f, "NyxstoneTricoreGCC init failed: {m}"),
            Error::AssembleFailed(m)    => write!(f, "assemble failed: {m}"),
            Error::SectionViolation(m)  => write!(f, "non-.text section directive: {m}"),
            Error::DisassembleFailed(m) => write!(f, "disassemble failed: {m}"),
            Error::Alloc                => f.write_str("allocation failure"),
            Error::NullArg              => f.write_str("null argument"),
            Error::Unknown(c, m)        => write!(f, "unknown status {c}: {m}"),
        }
    }
}

impl std::error::Error for Error {}

// ---- Daemon-installation helpers (opt-in bootstrap) ---------------------

/// Locate the `nyxstone-tcd` daemon binary without installing.
///
/// Lookup order:
/// 1. `NYXSTONE_TCD_PATH` env var.
/// 2. `nyxstone-tcd` on the system `PATH`.
///
/// Returns `Err(Error::Init(...))` with an actionable message if the binary
/// isn't found.
pub fn daemon_path() -> Result<PathBuf, Error> {
    daemon::find_daemon().ok_or_else(|| Error::Init(daemon::install_required_message()))
}

/// Ensure the `nyxstone-tcd` daemon binary is installed.
///
/// Skip-fast if the daemon is already locatable.  Otherwise:
/// 1. Try `cargo binstall --no-confirm nyxstone-tricore-gcc` (fast, pulls a
///    pre-built binary from the GitHub release if available; ~1 s on a
///    fresh machine).
/// 2. Fall back to `cargo install nyxstone-tricore-gcc` (~5-10 s; compiles the
///    daemon from source using the bundled binutils-tricore prebuilts).
///
/// Intended as a one-time bootstrap call early in your `main()`:
///
/// ```no_run
/// fn main() -> Result<(), Box<dyn std::error::Error>> {
///     nyxstone_tricore_gcc_ipc::install_daemon_if_missing()?;
///     let nx = nyxstone_tricore_gcc_ipc::NyxstoneTricoreGCC::new()?;
///     // ... use nx ...
///     # Ok(())
/// }
/// ```
///
/// Returns the path to the installed daemon binary.  Production deployments
/// where `cargo` is not available should ship the binary themselves and set
/// `NYXSTONE_TCD_PATH` instead.
pub fn install_daemon_if_missing() -> Result<PathBuf, Error> {
    if let Some(p) = daemon::find_daemon() { return Ok(p); }
    daemon::run_install()
        .map_err(|e| Error::Init(format!("auto-install of nyxstone-tcd failed: {e}")))
}

/// Force-reinstall the daemon binary even if one is already on PATH.
///
/// Useful when bumping `nyxstone-tricore-gcc` to a new version that requires a
/// matching daemon update.  Otherwise prefer [`install_daemon_if_missing`].
pub fn install_daemon() -> Result<PathBuf, Error> {
    daemon::run_install()
        .map_err(|e| Error::Init(format!("install of nyxstone-tcd failed: {e}")))
}

/// In-process TriCore assembler/disassembler, IPC-backed.
///
/// API identical to the GPL crate's `NyxstoneTricoreGCC`.  Internally
/// every call round-trips through the `nyxstone-tcd` daemon, which links
/// gas in-process.
pub struct NyxstoneTricoreGCC {
    conn: Connection,
}

impl NyxstoneTricoreGCC {
    /// Lazy-spawn the daemon and connect.  First call in a parent process
    /// fork+execs `nyxstone-tcd`; subsequent calls go to that same child.
    pub fn new() -> Result<Self, Error> {
        Ok(Self { conn: Connection::new()? })
    }

    /// Assemble `source` to `.text` bytes.
    pub fn assemble(&self, source: &str, address: Address, labels: &[LabelDefinition])
        -> Result<Vec<u8>, Error>
    {
        self.conn.assemble(source, address, labels)
    }

    /// Same as [`assemble`](Self::assemble) but returns one [`Instruction`]
    /// per encoded insn (disassembly text round-tripped via libopcodes).
    pub fn assemble_to_instructions(&self, source: &str, address: Address,
                                    labels: &[LabelDefinition])
        -> Result<Vec<Instruction>, Error>
    {
        self.conn.assemble_to_instructions(source, address, labels)
    }

    /// `gas -r` style: returns bytes (with reloc placeholders zeroed) plus
    /// one [`RelocationInfo`] per external label reference.
    ///
    /// Unlike the plain paths, an undefined symbol is NOT an error here --
    /// this is the link-later path.  `labels` may be empty; a matching
    /// entry only fills the `symbol.address` hint field.
    pub fn assemble_with_relocs(&self, source: &str, address: Address,
                                labels: &[LabelDefinition])
        -> Result<(Vec<u8>, Vec<RelocationInfo>), Error>
    {
        self.conn.assemble_with_relocs(source, address, labels)
    }

    /// Same as [`assemble_to_instructions`] but with `-r`-style relocation
    /// output (see [`assemble_with_relocs`](Self::assemble_with_relocs) for
    /// the undefined-symbol semantics).  Relocation sites decode with
    /// displacement 0, like objdump on an unlinked object.
    pub fn assemble_to_instructions_with_relocs(
        &self, source: &str, address: Address, labels: &[LabelDefinition],
    ) -> Result<(Vec<Instruction>, Vec<RelocationInfo>), Error>
    {
        self.conn.assemble_to_instructions_with_relocs(source, address, labels)
    }

    /// Disassemble `bytes` to text starting at absolute `address`.
    /// Decodes at most `count` instructions; pass `0` for "all".
    pub fn disassemble(&self, bytes: &[u8], address: Address, count: usize)
        -> Result<String, Error>
    {
        self.conn.disassemble(bytes, address, count)
    }

    /// Disassemble `bytes` to a list of [`Instruction`] records.
    pub fn disassemble_to_instructions(&self, bytes: &[u8], address: Address, count: usize)
        -> Result<Vec<Instruction>, Error>
    {
        self.conn.disassemble_to_instructions(bytes, address, count)
    }

    /// Test hook: pid of the daemon currently backing this instance.
    #[cfg(test)]
    fn daemon_pid_for_tests(&self) -> u32 {
        self.conn.daemon_pid()
    }
}

// `Connection` is `Send + Sync` via the inner Mutex; the daemon child is
// owned but never accessed concurrently.
unsafe impl Send for NyxstoneTricoreGCC {}
unsafe impl Sync for NyxstoneTricoreGCC {}
#[cfg(test)]
mod tests {
    use crate::*;

    #[test]
    fn assemble_nop() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
    }

    #[test]
    fn daemon_survives_spawning_thread_exit() {
        // Regression for parallel-test failures ("Broken pipe" / "Connection
        // reset by peer"): the daemon must outlive the thread that spawned it.
        // Create the instance on a worker thread, join it so that thread has
        // fully exited, then keep using the instance.  A thread-scoped
        // PR_SET_PDEATHSIG would have had the kernel kill the daemon here.
        let nx = std::thread::spawn(|| NyxstoneTricoreGCC::new().unwrap())
            .join()
            .expect("creation thread panicked");
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
        assert_eq!(nx.assemble("ret", 0, &[]).unwrap().len(), 2);

        // Also drive it concurrently from several short-lived threads to make
        // sure their exit doesn't disturb the shared daemon either.
        std::thread::scope(|s| {
            for _ in 0..8 {
                s.spawn(|| {
                    for _ in 0..16 {
                        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
                    }
                });
            }
        });
    }

    #[test]
    fn assemble_labels() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let b = nx.assemble("start:\n nop\n j here\nhere:\n ret\n", 0, &[]).unwrap();
        // `j here` relaxes to its 2-byte short form and targets `here` (disp 0x01).
        assert_eq!(b, vec![0x00, 0x00, 0x3c, 0x01, 0x00, 0x90]);
    }

    #[test]
    fn relax_local_short_external_long() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        // A same-section target relaxes to the 2-byte short `j` (opcode 0x3c),
        // whether the label sits after the branch (backward)...
        let back = nx.assemble("a:\n nop\n nop\n j a\n", 0, &[]).unwrap();
        assert_eq!(back.len(), 6); // nop nop (4) + short j (2)
        assert_eq!(back[4], 0x3c);
        // ...or before it (forward).
        let fwd = nx.assemble("j t\n nop\n nop\nt:\n ret\n", 0, &[]).unwrap();
        assert_eq!(fwd.len(), 8); // short j (2) + nop nop (4) + ret (2)
        assert_eq!(fwd[0], 0x3c);
        // An external target (resolved via LabelDefinition) stays the 4-byte
        // long `j` (opcode 0x1d) to keep maximum displacement range for the
        // linker. Undefined targets that emit relocations likewise stay long;
        // see `assemble_with_relocs_emits_entry`.
        let labels = [LabelDefinition::new("ext", 0x4000)];
        let ext = nx.assemble("nop\n nop\n j ext\n", 0, &labels).unwrap();
        assert_eq!(ext.len(), 8); // nop nop (4) + long j (4)
        assert_eq!(ext[4], 0x1d);
    }

    #[test]
    fn data_symbol_references() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        // `.word` to a local label encodes its absolute address (base+offset),
        // instead of being silently dropped.  nop(2)+.word(4)+ret(2); here@+6.
        let b = nx.assemble("start:\n nop\n .word here\nhere:\n ret\n", 0x1000, &[]).unwrap();
        assert_eq!(&b[2..6], &[0x06, 0x10, 0x00, 0x00]);
        // A label difference folds to a constant (b - a == 4 bytes).
        let b = nx.assemble("a:\n nop\n nop\nb:\n .word b - a\n", 0x1000, &[]).unwrap();
        assert_eq!(&b[4..8], &[0x04, 0x00, 0x00, 0x00]);
        // Pure-literal data lists still take the fast path unchanged.
        assert_eq!(nx.assemble(".word 0x11223344\n", 0, &[]).unwrap(),
                   vec![0x44, 0x33, 0x22, 0x11]);
        // `.short`/`.byte` to a local label take the low bits of the address.
        let b = nx.assemble("l:\n nop\n .short l\n", 0x1000, &[]).unwrap();
        assert_eq!(&b[2..4], &[0x00, 0x10]);
    }

    #[test]
    fn resolution_reloc_encodings() {
        // Non-linear relocation forms the in-place encoder resolves (faithful
        // to gas md_apply_fix): the B-format 24-bit split (24ABS), the high-
        // half carry-adjusted RLC (HIADJ via `hi:`), and the 18-bit absolute
        // addressing split (18ABS).  A dst_mask heuristic mis-encoded these.
        let nx = NyxstoneTricoreGCC::new().unwrap();
        assert_eq!(nx.assemble("ja S\n", 0, &[LabelDefinition::new("S", 0x10)]).unwrap(),
                   vec![0x9d, 0x00, 0x08, 0x00]);
        assert_eq!(nx.assemble("movh %d0, hi:S\n", 0, &[LabelDefinition::new("S", 0x12345678)]).unwrap(),
                   vec![0x7b, 0x40, 0x23, 0x01]);
        assert_eq!(nx.assemble("ld.w %d0, S\n", 0, &[LabelDefinition::new("S", 0x100)]).unwrap(),
                   vec![0x85, 0x00, 0x00, 0x40]);
        let b = nx.assemble("movh %d0, hi:S\n addi %d0, %d0, lo:S\n", 0,
                            &[LabelDefinition::new("S", 0xabcd)]).unwrap();
        let dis = nx.disassemble(&b, 0, 0).unwrap();
        assert!(dis.contains(",1\n") || dis.contains(",1 "), "movh hiadj: {dis:?}");
        assert!(dis.contains("-21555"), "addi sign-ext lo: {dis:?}");
    }

    #[test]
    fn block_comments_stripped() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        assert_eq!(nx.assemble("nop /* x */\n", 0, &[]).unwrap(), vec![0x00, 0x00]);
        assert_eq!(nx.assemble("nop\n/* a\n b */\n ret\n", 0, &[]).unwrap(),
                   vec![0x00, 0x00, 0x00, 0x90]);
        assert_eq!(nx.assemble(".asciz \"a/*b*/c\"\n", 0, &[]).unwrap(),
                   vec![0x61, 0x2f, 0x2a, 0x62, 0x2a, 0x2f, 0x63, 0x00]);
    }

    #[test]
    fn v16_isa_and_disasm_annotation() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        assert_eq!(nx.assemble("fret\n", 0, &[]).unwrap(), vec![0x00, 0x70]);
        assert_eq!(nx.assemble("cmpswap.w [%a0+]4, %e0\n", 0x10000, &[]).unwrap(),
                   vec![0x49, 0x00, 0xc4, 0x00]);
        assert!(nx.assemble("fcall L\nL: nop\n", 0x10002, &[]).is_ok());
        let b = nx.assemble("movh.a %a0, hi:L\n lea %a0, [%a0]lo:L\n", 0,
                            &[LabelDefinition::new("L", 0x70001234)]).unwrap();
        for ins in nx.disassemble_to_instructions(&b, 0, 0).unwrap() {
            assert!(!ins.assembly.contains('<'), "annotation leaked: {:?}", ins.assembly);
        }
    }

    #[test]
    fn branch_displacements_resolve() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        // Forward branch over several instructions must target the label, not
        // itself: `l` is the last instruction, at base + len - 2 (ret = 2 B).
        let b = nx.assemble("j l\n nop\n nop\n nop\nl: ret\n", 0x1000, &[]).unwrap();
        let want = 0x1000 + b.len() as u64 - 2;
        let dis = nx.disassemble(&b, 0x1000, 0).unwrap();
        let first = dis.lines().next().unwrap();
        assert!(first.contains(&format!("0x{want:x}")), "fwd: {first:?} want 0x{want:x}");
        // Backward branch must target the first instruction (the base address).
        let b = nx.assemble("l: nop\n nop\n nop\n j l\n", 0x1000, &[]).unwrap();
        let dis = nx.disassemble(&b, 0x1000, 0).unwrap();
        let last = dis.lines().last().unwrap();
        assert!(last.contains("0x1000"), "back: {last:?} want 0x1000");
    }

    #[test]
    fn section_violation() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let err = nx.assemble(".data\n .byte 0x42\n", 0, &[]).unwrap_err();
        assert!(matches!(err, Error::AssembleFailed(_) | Error::SectionViolation(_)));
    }

    #[test]
    fn roundtrip_to_instructions() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let bytes = nx.assemble("nop; ret", 0x1000, &[]).unwrap();
        let insns = nx.disassemble_to_instructions(&bytes, 0x1000, 0).unwrap();
        assert_eq!(insns.len(), 2);
        assert_eq!(insns[0].address, 0x1000);
        assert_eq!(insns[1].address, 0x1002);
    }

    #[test]
    fn assemble_to_instructions_address() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let v = nx.assemble_to_instructions("nop\n ret\n", 0x4000, &[]).unwrap();
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].address, 0x4000);
        assert_eq!(v[1].address, 0x4002);
    }

    #[test]
    fn external_label() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let labels = [LabelDefinition::new("ext", 0x1008)];
        let b = nx.assemble("nop\n nop\n nop\n j ext\n", 0x1000, &labels).unwrap();
        // 3 nop (6 bytes) + 4-byte j to absolute symbol = 10 bytes total.
        assert_eq!(b.len(), 10);
    }

    #[test]
    fn disassemble_count() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let bytes = nx.assemble("nop; nop; ret", 0, &[]).unwrap();
        let v = nx.disassemble_to_instructions(&bytes, 0, 2).unwrap();
        assert_eq!(v.len(), 2);
    }

    #[test]
    fn assemble_with_relocs_emits_entry() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let labels = [LabelDefinition::new("ext", 0x2000)];
        let (bytes, relocs) = nx.assemble_with_relocs(
            "nop\n j ext\n", 0x1000, &labels).unwrap();
        // 2 bytes nop + 4 bytes long-form j (displacement zeroed)
        assert_eq!(bytes.len(), 6);
        assert_eq!(&bytes[..], &[0x00, 0x00, 0x1d, 0x00, 0x00, 0x00]);
        assert_eq!(relocs.len(), 1);
        let r = &relocs[0];
        // offset is absolute: base 0x1000 + 2-byte nop.
        assert_eq!(r.offset, 0x1002);
        assert_eq!(r.symbol.name, "ext");
        assert_eq!(r.symbol.address, 0x2000);
        // R_TRICORE_24REL == 3
        assert_eq!(r.relocation_type, 3);
        assert!(r.addend.is_some());
    }

    #[test]
    fn assemble_with_relocs_unlisted_symbol_address_zero() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        // No LabelDefinition for `foobar`, should still yield a reloc, but
        // symbol.address stays 0.
        let (_, relocs) = nx.assemble_with_relocs(
            "j foobar\n", 0, &[]).unwrap();
        assert_eq!(relocs.len(), 1);
        assert_eq!(relocs[0].symbol.name, "foobar");
        assert_eq!(relocs[0].symbol.address, 0);
    }

    #[test]
    fn assemble_to_instructions_with_relocs_carries_both() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let labels = [LabelDefinition::new("ext", 0x9000)];
        let (insns, relocs) = nx.assemble_to_instructions_with_relocs(
            "nop\n j ext\n", 0x1000, &labels).unwrap();
        assert_eq!(insns.len(), 2);
        assert_eq!(insns[0].address, 0x1000);
        assert_eq!(insns[1].address, 0x1002);
        assert_eq!(relocs.len(), 1);
        assert_eq!(relocs[0].offset, 0x1002);          // absolute
        assert_eq!(relocs[0].symbol.address, 0x9000);  // hint from LabelDefinition
    }

    #[test]
    fn reloc_offset_is_absolute_and_address_hint_resolves() {
        // Regression: absolute reloc offset + symbol.address hint must survive
        // the IPC round-trip (the daemon serializes sym_addr in the protocol),
        // including a leading-dot label name.
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let (_, relocs) = nx.assemble_with_relocs(
            "j .lbl\n", 0x10002, &[LabelDefinition::new(".lbl", 0x101010)]).unwrap();
        assert_eq!(relocs.len(), 1);
        assert_eq!(relocs[0].offset, 0x10002);
        assert_eq!(relocs[0].symbol.name, ".lbl");
        assert_eq!(relocs[0].symbol.address, 0x101010);
        assert_eq!(relocs[0].relocation_type, 3);
    }

    #[test]
    fn label_with_interior_nul_is_error_not_panic() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        // Must surface as Err end-to-end; in particular it must not panic
        // (or kill) the daemon, which keeps serving afterwards.
        let labels = [LabelDefinition::new("bad\0name", 0x1000)];
        let err = nx.assemble("j ext\n", 0, &labels).unwrap_err();
        assert!(matches!(err, Error::AssembleFailed(_)), "{err:?}");
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
    }

    #[cfg(target_pointer_width = "64")]
    #[test]
    fn oversized_disassemble_count_is_error() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let bytes = nx.assemble("nop; ret", 0, &[]).unwrap();
        // count == 2^32 used to truncate to 0, inverting the semantics
        // (0 = "decode all").  It must be a hard error instead.
        let err = nx.disassemble(&bytes, 0, 1usize << 32).unwrap_err();
        assert!(matches!(err, Error::DisassembleFailed(_)), "{err:?}");
        // The largest representable count still works.
        assert!(nx.disassemble(&bytes, 0, u32::MAX as usize).is_ok());
    }

    #[test]
    fn daemon_killed_then_next_call_succeeds_via_respawn() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);

        // Hard-kill the daemon out from under the instance.
        let old_pid = nx.daemon_pid_for_tests();
        unsafe { libc::kill(old_pid as libc::pid_t, libc::SIGKILL); }
        // Give the kernel a moment to tear the process down and close its
        // socket end so the next call observes the transport error.
        std::thread::sleep(std::time::Duration::from_millis(200));

        // The next call hits a transport error, respawns the daemon once,
        // and retries the request transparently.
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
        assert_ne!(nx.daemon_pid_for_tests(), old_pid);
        // The fresh connection keeps working.
        assert_eq!(nx.assemble("ret", 0, &[]).unwrap().len(), 2);
    }
}
