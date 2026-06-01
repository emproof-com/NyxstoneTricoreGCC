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
//! nyxstone-tricore-gcc-ipc = "0.1"
//! ```
//!
//! User code stays the same, same struct, same methods, same types.
//!
//! # How the IPC mode works
//!
//! On first `NyxstoneTricoreGCC::new()` call the crate fork+execs the
//! `nyxstone-tcd` daemon binary (GPL-3.0+, distributed separately, install
//! via `cargo install nyxstone-tricore-gcc`), then talks to it over a
//! `socketpair(2)` UNIX socket using a small custom binary protocol.
//!
//! The daemon:
//! - is **one per parent process**.  Different parents never share daemons.
//! - is **lazy**, spawned on the first `new()`, not at module init.
//! - has its **lifetime tied to the parent process** via `PR_SET_PDEATHSIG`
//!   on Linux: kernel sends `SIGTERM` to the daemon the moment the parent
//!   exits (including crashes and `kill -9`).
//! - serves **strict FIFO** request/response, no out-of-order completion,
//!   no request IDs.  Multi-threaded callers serialize through a
//!   per-instance `Mutex`.
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
    pub fn assemble_with_relocs(&self, source: &str, address: Address,
                                labels: &[LabelDefinition])
        -> Result<(Vec<u8>, Vec<RelocationInfo>), Error>
    {
        self.conn.assemble_with_relocs(source, address, labels)
    }

    /// Same as [`assemble_to_instructions`] but with `-r`-style relocation
    /// output.
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
    fn assemble_labels() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        let b = nx.assemble("start:\n nop\n j here\nhere:\n ret\n", 0, &[]).unwrap();
        assert_eq!(b, vec![0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x90]);
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
        assert_eq!(r.offset, 0x2);
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
        assert_eq!(relocs[0].offset, 0x2);
    }
}
