//! NyxstoneTricoreGCC, Rust bindings.
//!
//! Safe Rust wrapper around the C ABI defined in `c_api/nyxstone_c.h`.  The
//! underlying library is an in-process TriCore assembler/disassembler built
//! on the EEESlab `tricore-binutils-gdb` fork.
//!
//! The API shape mirrors that of the sibling project [Nyxstone],
//! a separate codebase built on LLVM-MC, covering the architectures
//! LLVM supports.  This crate is an independent implementation using GNU
//! binutils to cover TriCore (which LLVM-MC has no backend for).  Both
//! projects expose four methods named
//! [`assemble`](NyxstoneTricoreGCC::assemble),
//! [`assemble_to_instructions`](NyxstoneTricoreGCC::assemble_to_instructions),
//! [`disassemble`](NyxstoneTricoreGCC::disassemble), and
//! [`disassemble_to_instructions`](NyxstoneTricoreGCC::disassemble_to_instructions),
//! all taking an absolute `address` (and, for the assembly entry points,
//! a slice of `LabelDefinition`s for external symbols).
//!
//! [Nyxstone]: https://github.com/emproof-com/nyxstone
//!
//! # Threading
//!
//! All GAS globals are process-wide; a process-wide lock serializes every
//! C call.  `NyxstoneTricoreGCC` is both `Send` and `Sync`, but two threads
//! sharing one or two handles will serialize on the global lock, they
//! won't actually run concurrently.
//!
//! # Section restriction (Nyxstone-style)
//!
//! Any directive that would switch the active section to something other
//! than `.text` (e.g. `.data`, `.bss`, `.section .foo`) makes
//! [`assemble`](NyxstoneTricoreGCC::assemble) return an error.  `.text` and
//! `.section .text` / `.section .text.<name>` are accepted as no-ops.
//!
//! # Example
//!
//! ```no_run
//! use nyxstone_tricore_gcc::NyxstoneTricoreGCC;
//!
//! let nx = NyxstoneTricoreGCC::new()?;
//! let bytes = nx.assemble("start:\n nop\n j here\nhere:\n ret\n", 0, &[])?;
//! // `j here` relaxes to its 2-byte short form and targets `here` (disp 0x01).
//! assert_eq!(bytes, [0x00, 0x00, 0x3c, 0x01, 0x00, 0x90]);
//!
//! let insns = nx.disassemble_to_instructions(&bytes, 0x80000000, 0)?;
//! for ins in &insns {
//!     println!("0x{:08x}  {}", ins.address, ins.assembly);
//! }
//! # Ok::<(), nyxstone_tricore_gcc::Error>(())
//! ```

mod ffi;

use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::c_char;
use std::ptr;
use std::sync::Mutex;

// All gas globals are process-wide; concurrent calls into the C ABI from
// different threads would race.  Serialize via a static mutex so the Rust
// surface is safe even when threads try to use the library at the same time.
static GLOBAL_LOCK: Mutex<()> = Mutex::new(());

/// Take the global gas lock, ignoring poisoning: no panic can occur mid-C-call
/// (all Rust-side validation happens before the lock is taken), so a panicking
/// thread cannot leave the guarded gas state corrupted.
fn lock_gas() -> std::sync::MutexGuard<'static, ()> {
    GLOBAL_LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

/// Convenience alias matching the public C++/Rust API contract.
pub type Address = u64;

/// External label definition (input to `assemble` / `assemble_to_instructions`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LabelDefinition {
    /// The label name.
    pub name:    String,
    /// The absolute address of the label.
    pub address: Address,
}

impl LabelDefinition {
    /// Convenience constructor.
    pub fn new(name: impl Into<String>, address: Address) -> Self {
        Self { name: name.into(), address }
    }
}

/// Symbol target of a relocation, with the resolved address copied from
/// the matching [`LabelDefinition`] (0 if the caller did not supply one).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelocationSymbol {
    pub name:    String,
    pub address: Address,
}

/// One relocation entry, the same shape gas/gcc emits with `-r`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RelocationInfo {
    /// Relocation offset (section-relative).
    pub offset: Address,
    /// Relocation addend if available (TriCore uses RELA, so this is always `Some`).
    pub addend: Option<i64>,
    /// Symbol target of this relocation, resolved if possible.
    pub symbol: RelocationSymbol,
    /// ELF R_TRICORE_* relocation type, width-fixed for size-independent querying.
    pub relocation_type: u32,
}

/// Errors produced by [`NyxstoneTricoreGCC`] methods.  Each variant carries a free-form
/// message from the underlying library where available.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// libbfd init failed (typically: target vector for elf32-tricore is
    /// missing, the host wasn't built with TriCore support).
    Init(String),
    /// gas couldn't parse or encode the source (unknown mnemonic, operand
    /// mismatch, immediate out of range, …).
    AssembleFailed(String),
    /// The source contains a directive that would switch the active
    /// section away from `.text` (`.data`, `.bss`, `.section .foo`, …).
    SectionViolation(String),
    /// libopcodes couldn't decode some byte in the buffer.
    DisassembleFailed(String),
    /// Out of memory.
    Alloc,
    /// A C ABI argument was unexpectedly NULL (should not happen in safe
    /// Rust code).
    NullArg,
    /// The library returned a status code we don't recognise.
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

/// Pull the malloc'd error message back into an owned String, freeing the
/// underlying buffer.  Returns "" if the pointer is null.
fn take_err(p: *mut c_char) -> String {
    if p.is_null() { return String::new(); }
    let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
    unsafe { ffi::nyxstone_free_string(p); }
    s
}

fn err_from_status(s: ffi::nyxstone_status_t, msg: String) -> Result<(), Error> {
    match s {
        ffi::NYXSTONE_OK                    => Ok(()),
        ffi::NYXSTONE_ERR_INIT              => Err(Error::Init(msg)),
        ffi::NYXSTONE_ERR_ASSEMBLE_FAILED   => Err(Error::AssembleFailed(msg)),
        ffi::NYXSTONE_ERR_SECTION_VIOLATION => Err(Error::SectionViolation(msg)),
        ffi::NYXSTONE_ERR_DISASM_FAILED     => Err(Error::DisassembleFailed(msg)),
        ffi::NYXSTONE_ERR_ALLOC             => Err(Error::Alloc),
        ffi::NYXSTONE_ERR_NULL_ARG          => Err(Error::NullArg),
        other                          => Err(Error::Unknown(other, msg)),
    }
}

/// One disassembled (or freshly assembled) instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Instruction {
    /// Address of the first byte (= `address + offset_in_buffer`).
    pub address:  u64,
    /// Disassembled text, matches `tricore-elf-objdump -d` output.
    pub assembly: String,
    /// Encoded bytes (2 or 4).
    pub bytes:    Vec<u8>,
}

/// In-process TriCore assembler/disassembler.  Same API shape as the
/// sibling [Nyxstone](https://github.com/emproof-com/nyxstone) project
/// (which is LLVM-MC-based and an independent codebase).
pub struct NyxstoneTricoreGCC {
    inner: *mut ffi::nyxstone_handle_t,
}

unsafe impl Send for NyxstoneTricoreGCC {}
// Every public method takes &self and holds GLOBAL_LOCK while touching gas
// state, so two threads sharing a handle serialize internally.
unsafe impl Sync for NyxstoneTricoreGCC {}

impl NyxstoneTricoreGCC {
    /// Create a new NyxstoneTricoreGCC.  The first call in a process
    /// performs gas's one-time init; subsequent calls reuse it.
    pub fn new() -> Result<Self, Error> {
        let _g = lock_gas();
        let mut err: *mut c_char = ptr::null_mut();
        let p = unsafe { ffi::nyxstone_create(&mut err) };
        if p.is_null() { return Err(Error::Init(take_err(err))); }
        Ok(Self { inner: p })
    }

    /// Build a `Vec<nyxstone_label_def_t>` plus the owned CStrings that back the
    /// `name` pointers.  Keep the CStrings alive until the C call returns.
    ///
    /// Returns an error for label names with interior NUL bytes.  Callers must
    /// invoke this BEFORE taking `GLOBAL_LOCK` so validation failures can never
    /// panic (or bail) while the global gas lock is held.
    fn pack_labels(labels: &[LabelDefinition])
        -> Result<(Vec<ffi::nyxstone_label_def_t>, Vec<CString>), Error>
    {
        let owned: Vec<CString> = labels.iter()
            .map(|l| CString::new(l.name.as_str()).map_err(|_| {
                Error::AssembleFailed(format!(
                    "label name {:?} contains an interior NUL byte", l.name))
            }))
            .collect::<Result<_, _>>()?;
        let raw: Vec<ffi::nyxstone_label_def_t> = owned.iter().zip(labels)
            .map(|(n, l)| ffi::nyxstone_label_def_t {
                name:    n.as_ptr(),
                address: l.address,
            })
            .collect();
        Ok((raw, owned))
    }

    /// Translate assembly to `.text` bytes at the given absolute `address`,
    /// with optional external label definitions.
    pub fn assemble(
        &self,
        source: &str,
        address: u64,
        labels: &[LabelDefinition],
    ) -> Result<Vec<u8>, Error> {
        let (raw, _owned) = Self::pack_labels(labels)?; // validate before locking
        let _g = lock_gas();
        let mut out:  *mut u8 = ptr::null_mut();
        let mut nlen: usize   = 0;
        let mut err:  *mut c_char = ptr::null_mut();
        let s = unsafe {
            ffi::nyxstone_assemble(
                self.inner,
                source.as_ptr() as *const c_char, source.len(),
                address,
                raw.as_ptr(), raw.len(),
                &mut out, &mut nlen,
                &mut err,
            )
        };
        err_from_status(s, take_err(err))?;
        if out.is_null() || nlen == 0 { return Ok(Vec::new()); }
        let v = unsafe { std::slice::from_raw_parts(out, nlen).to_vec() };
        unsafe { ffi::nyxstone_free_bytes(out); }
        Ok(v)
    }

    /// Translate assembly to a vector of [`Instruction`]s (one per encoded
    /// instruction, with disassembly text round-tripped via libopcodes).
    pub fn assemble_to_instructions(
        &self,
        source: &str,
        address: u64,
        labels: &[LabelDefinition],
    ) -> Result<Vec<Instruction>, Error> {
        let (raw, _owned) = Self::pack_labels(labels)?; // validate before locking
        let _g = lock_gas();
        let mut out: *mut ffi::nyxstone_instruction_t = ptr::null_mut();
        let mut n:   usize = 0;
        let mut err: *mut c_char = ptr::null_mut();
        let s = unsafe {
            ffi::nyxstone_assemble_to_instructions(
                self.inner,
                source.as_ptr() as *const c_char, source.len(),
                address,
                raw.as_ptr(), raw.len(),
                &mut out, &mut n,
                &mut err,
            )
        };
        err_from_status(s, take_err(err))?;
        Ok(unpack_instructions(out, n))
    }

    /// Translate bytes to disassembly text.  `count` limits the number of
    /// instructions decoded; pass `0` for "all".
    pub fn disassemble(
        &self,
        bytes: &[u8],
        address: u64,
        count: usize,
    ) -> Result<String, Error> {
        let _g = lock_gas();
        let mut out: *mut c_char = ptr::null_mut();
        let mut err: *mut c_char = ptr::null_mut();
        let s = unsafe {
            ffi::nyxstone_disassemble(
                self.inner,
                bytes.as_ptr(), bytes.len(),
                address,
                count,
                &mut out, &mut err,
            )
        };
        err_from_status(s, take_err(err))?;
        if out.is_null() { return Ok(String::new()); }
        let r = unsafe { CStr::from_ptr(out) }.to_string_lossy().into_owned();
        unsafe { ffi::nyxstone_free_string(out); }
        Ok(r)
    }

    /// Translate bytes to [`Instruction`] records.
    pub fn disassemble_to_instructions(
        &self,
        bytes: &[u8],
        address: u64,
        count: usize,
    ) -> Result<Vec<Instruction>, Error> {
        let _g = lock_gas();
        let mut out: *mut ffi::nyxstone_instruction_t = ptr::null_mut();
        let mut n:   usize = 0;
        let mut err: *mut c_char = ptr::null_mut();
        let s = unsafe {
            ffi::nyxstone_disassemble_to_instructions(
                self.inner,
                bytes.as_ptr(), bytes.len(),
                address,
                count,
                &mut out, &mut n,
                &mut err,
            )
        };
        err_from_status(s, take_err(err))?;
        Ok(unpack_instructions(out, n))
    }
}

impl NyxstoneTricoreGCC {
    /// Like [`assemble`](Self::assemble) but leaves @p labels unresolved and
    /// returns one [`RelocationInfo`] per external reference, the same shape
    /// gas/gcc emits with `-r`.
    ///
    /// Unlike the plain paths, an undefined symbol is NOT an error here --
    /// this is the link-later path.  Each undefined reference stays as zero
    /// placeholder bytes in the stream and is described by a relocation
    /// record; `labels` may be empty and only fills the `symbol.address`
    /// hint field of matching records.
    pub fn assemble_with_relocs(
        &self,
        source: &str,
        address: Address,
        labels: &[LabelDefinition],
    ) -> Result<(Vec<u8>, Vec<RelocationInfo>), Error> {
        let (raw, _owned) = Self::pack_labels(labels)?; // validate before locking
        let _g = lock_gas();
        let mut out_b:  *mut u8 = ptr::null_mut();
        let mut nlen:   usize   = 0;
        let mut out_r:  *mut ffi::nyxstone_reloc_t = ptr::null_mut();
        let mut rlen:   usize   = 0;
        let mut err:    *mut c_char = ptr::null_mut();
        let s = unsafe {
            ffi::nyxstone_assemble_with_relocs(
                self.inner,
                source.as_ptr() as *const c_char, source.len(),
                address,
                raw.as_ptr(), raw.len(),
                &mut out_b, &mut nlen,
                &mut out_r, &mut rlen,
                &mut err,
            )
        };
        err_from_status(s, take_err(err))?;
        let bytes = if out_b.is_null() || nlen == 0 {
            Vec::new()
        } else {
            let v = unsafe { std::slice::from_raw_parts(out_b, nlen).to_vec() };
            unsafe { ffi::nyxstone_free_bytes(out_b); }
            v
        };
        Ok((bytes, unpack_relocations(out_r, rlen)))
    }

    /// Like [`assemble_to_instructions`](Self::assemble_to_instructions)
    /// but with `-r`-style relocation output (see
    /// [`assemble_with_relocs`](Self::assemble_with_relocs) for the
    /// undefined-symbol semantics).  The per-instruction text decodes the
    /// placeholder bytes, so a relocation site prints with displacement 0,
    /// like objdump on an unlinked object; correlate `Instruction.address`
    /// with the relocation offsets to find the sites the linker patches.
    pub fn assemble_to_instructions_with_relocs(
        &self,
        source: &str,
        address: Address,
        labels: &[LabelDefinition],
    ) -> Result<(Vec<Instruction>, Vec<RelocationInfo>), Error> {
        let (raw, _owned) = Self::pack_labels(labels)?; // validate before locking
        let _g = lock_gas();
        let mut out_i:  *mut ffi::nyxstone_instruction_t = ptr::null_mut();
        let mut ilen:   usize   = 0;
        let mut out_r:  *mut ffi::nyxstone_reloc_t = ptr::null_mut();
        let mut rlen:   usize   = 0;
        let mut err:    *mut c_char = ptr::null_mut();
        let s = unsafe {
            ffi::nyxstone_assemble_to_instructions_with_relocs(
                self.inner,
                source.as_ptr() as *const c_char, source.len(),
                address,
                raw.as_ptr(), raw.len(),
                &mut out_i, &mut ilen,
                &mut out_r, &mut rlen,
                &mut err,
            )
        };
        err_from_status(s, take_err(err))?;
        Ok((unpack_instructions(out_i, ilen), unpack_relocations(out_r, rlen)))
    }
}

fn unpack_relocations(out: *mut ffi::nyxstone_reloc_t, n: usize) -> Vec<RelocationInfo> {
    let mut v = Vec::with_capacity(n);
    for i in 0..n {
        let r = unsafe { &*out.add(i) };
        let name = if r.symbol.name.is_null() { String::new() }
                   else { unsafe { CStr::from_ptr(r.symbol.name) }
                              .to_string_lossy()
                              .into_owned() };
        v.push(RelocationInfo {
            offset: r.offset,
            addend: if r.has_addend != 0 { Some(r.addend) } else { None },
            symbol: RelocationSymbol { name, address: r.symbol.address },
            relocation_type: r.relocation_type,
        });
    }
    if !out.is_null() { unsafe { ffi::nyxstone_free_relocations(out, n); } }
    v
}

fn unpack_instructions(out: *mut ffi::nyxstone_instruction_t, n: usize) -> Vec<Instruction> {
    let mut v = Vec::with_capacity(n);
    for i in 0..n {
        let p = unsafe { &*out.add(i) };
        // Match the reloc-unpacking path: a NULL string from the C side maps
        // to an empty string instead of being dereferenced.
        let assembly = if p.assembly.is_null() { String::new() }
                       else { unsafe { CStr::from_ptr(p.assembly as *const c_char) }
                                  .to_string_lossy()
                                  .into_owned() };
        let bytes = if p.bytes.is_null() || p.bytes_len == 0 { Vec::new() }
                    else { unsafe { std::slice::from_raw_parts(p.bytes, p.bytes_len) }.to_vec() };
        v.push(Instruction { address: p.address, assembly, bytes });
    }
    if !out.is_null() { unsafe { ffi::nyxstone_free_instructions(out, n); } }
    v
}

impl Drop for NyxstoneTricoreGCC {
    fn drop(&mut self) {
        let _g = lock_gas();
        unsafe { ffi::nyxstone_destroy(self.inner); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assemble_nop() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
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
    fn label_with_interior_nul_is_error_not_panic() {
        let nx = NyxstoneTricoreGCC::new().unwrap();
        // A NUL inside a label name must surface as Err, not panic (a panic
        // here used to poison GLOBAL_LOCK and abort the process in Drop).
        let labels = [LabelDefinition::new("bad\0name", 0x1000)];
        let err = nx.assemble("j ext\n", 0, &labels).unwrap_err();
        assert!(matches!(err, Error::AssembleFailed(_)), "{err:?}");
        // The global lock must remain usable afterwards.
        assert_eq!(nx.assemble("nop", 0, &[]).unwrap(), vec![0x00, 0x00]);
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
