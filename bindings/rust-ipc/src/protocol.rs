//! Wire protocol between `nyxstone-tricore-gcc-ipc` (client) and `nyxstone-tcd`
//! (daemon).
//!
//! Strict FIFO request/response over a single SOCK_STREAM UNIX socket.
//! All integers little-endian.  No serde, no schema language, just hand-rolled
//! pack/unpack so both sides have zero runtime dependencies beyond `libc`.
//!
//! Frame layout (8-byte header + variable body):
//!
//! ```text
//!   +--------+--------+--------+--------+--------+--------+--------+--------+
//!   |              body_len (u32 LE)    | opcode | flags  |    reserved     |
//!   +--------+--------+--------+--------+--------+--------+--------+--------+
//!   |                          body (body_len bytes)                        |
//!   +-----------------------------------------------------------------------+
//! ```
//!
//! For responses, `opcode` is replaced by `status` (0 = ok, else error code).
//! Both sides validate `PROTOCOL_VERSION` once during the connection handshake.

use std::io::{self, Read, Write};

/// Bumped whenever the wire format changes incompatibly.
pub const PROTOCOL_VERSION: u32 = 1;

/// Hard cap on a frame body (and on any single length-prefixed string or
/// byte buffer inside one): 64 MiB.
///
/// Write side: rejected with `InvalidInput` before any byte hits the wire,
/// so an oversized payload can never desync the stream (`len as u32` used to
/// silently truncate the prefix while `write_all` still wrote everything).
/// Read side: a length prefix above the cap is rejected with `InvalidData`
/// *before* allocating, so a corrupt prefix can't trigger a 4 GiB allocation.
pub const MAX_FRAME: u32 = 64 * 1024 * 1024;

/// Magic bytes the daemon writes on accept, and the client verifies.
pub const HANDSHAKE_MAGIC: [u8; 4] = *b"NYXT";

// ---- opcodes (request) --------------------------------------------------

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Opcode {
    Assemble                         = 0x01,
    AssembleToInstructions           = 0x02,
    Disassemble                      = 0x03,
    DisassembleToInstructions        = 0x04,
    AssembleWithRelocs               = 0x05,
    AssembleToInstructionsWithRelocs = 0x06,
}

// ---- status codes (response) --------------------------------------------

pub mod status {
    pub const OK:                 u8 = 0;
    pub const ERR_INIT:           u8 = 1;
    pub const ERR_NULL_ARG:       u8 = 2;
    pub const ERR_ASSEMBLE:       u8 = 3;
    pub const ERR_SECTION:        u8 = 4;
    pub const ERR_DISASM:         u8 = 5;
    pub const ERR_ALLOC:          u8 = 6;
    pub const ERR_PROTOCOL:       u8 = 7;
    pub const ERR_UNKNOWN:        u8 = 0xFF;
}

// ---- low-level read/write helpers ---------------------------------------

#[inline] pub fn write_u8(w: &mut impl Write, v: u8) -> io::Result<()> { w.write_all(&[v]) }
#[inline] pub fn write_u16(w: &mut impl Write, v: u16) -> io::Result<()> { w.write_all(&v.to_le_bytes()) }
#[inline] pub fn write_u32(w: &mut impl Write, v: u32) -> io::Result<()> { w.write_all(&v.to_le_bytes()) }
#[inline] pub fn write_u64(w: &mut impl Write, v: u64) -> io::Result<()> { w.write_all(&v.to_le_bytes()) }
#[inline] pub fn write_i64(w: &mut impl Write, v: i64) -> io::Result<()> { w.write_all(&v.to_le_bytes()) }

#[inline] pub fn write_bytes(w: &mut impl Write, b: &[u8]) -> io::Result<()> {
    let n: u32 = b.len().try_into().ok()
        .filter(|n| *n <= MAX_FRAME)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput,
            format!("payload of {} bytes exceeds MAX_FRAME ({MAX_FRAME} bytes)", b.len())))?;
    write_u32(w, n)?;
    w.write_all(b)
}
#[inline] pub fn write_str(w: &mut impl Write, s: &str) -> io::Result<()> {
    write_bytes(w, s.as_bytes())
}

#[inline] pub fn read_u8(r: &mut impl Read) -> io::Result<u8> {
    let mut b = [0u8; 1]; r.read_exact(&mut b)?; Ok(b[0])
}
#[inline] pub fn read_u32(r: &mut impl Read) -> io::Result<u32> {
    let mut b = [0u8; 4]; r.read_exact(&mut b)?; Ok(u32::from_le_bytes(b))
}
#[inline] pub fn read_u64(r: &mut impl Read) -> io::Result<u64> {
    let mut b = [0u8; 8]; r.read_exact(&mut b)?; Ok(u64::from_le_bytes(b))
}
#[inline] pub fn read_i64(r: &mut impl Read) -> io::Result<i64> {
    let mut b = [0u8; 8]; r.read_exact(&mut b)?; Ok(i64::from_le_bytes(b))
}
#[inline] pub fn read_bytes(r: &mut impl Read) -> io::Result<Vec<u8>> {
    let n = read_u32(r)?;
    if n > MAX_FRAME {
        return Err(io::Error::new(io::ErrorKind::InvalidData,
            format!("length prefix {n} exceeds MAX_FRAME ({MAX_FRAME} bytes)")));
    }
    let mut v = vec![0u8; n as usize]; r.read_exact(&mut v)?; Ok(v)
}
#[inline] pub fn read_str(r: &mut impl Read) -> io::Result<String> {
    let v = read_bytes(r)?;
    String::from_utf8(v).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}

// ---- frame I/O ----------------------------------------------------------

#[derive(Debug)]
pub struct FrameHeader {
    pub body_len: u32,
    /// On request: an `Opcode`.  On response: a `status` byte.
    pub kind:     u8,
    pub flags:    u8,
}

impl FrameHeader {
    pub fn write(&self, w: &mut impl Write) -> io::Result<()> {
        // Validate before any byte hits the wire so an oversized frame can
        // never leave the stream half-written and desynced.
        if self.body_len > MAX_FRAME {
            return Err(io::Error::new(io::ErrorKind::InvalidInput,
                format!("frame body of {} bytes exceeds MAX_FRAME ({MAX_FRAME} bytes)",
                        self.body_len)));
        }
        write_u32(w, self.body_len)?;
        write_u8(w, self.kind)?;
        write_u8(w, self.flags)?;
        write_u8(w, 0)?;            // reserved
        write_u8(w, 0)?;
        Ok(())
    }
    pub fn read(r: &mut impl Read) -> io::Result<FrameHeader> {
        let body_len = read_u32(r)?;
        let kind     = read_u8(r)?;
        let flags    = read_u8(r)?;
        let _r0      = read_u8(r)?;
        let _r1      = read_u8(r)?;
        // Reject before the caller allocates a body buffer: a corrupt prefix
        // must not be able to request a multi-GiB allocation.
        if body_len > MAX_FRAME {
            return Err(io::Error::new(io::ErrorKind::InvalidData,
                format!("frame body length {body_len} exceeds MAX_FRAME ({MAX_FRAME} bytes)")));
        }
        Ok(FrameHeader { body_len, kind, flags })
    }
}

/// Handshake bytes the daemon sends on connect.  Verifies protocol version.
pub fn write_handshake(w: &mut impl Write) -> io::Result<()> {
    w.write_all(&HANDSHAKE_MAGIC)?;
    write_u32(w, PROTOCOL_VERSION)
}

pub fn read_handshake(r: &mut impl Read) -> io::Result<()> {
    let mut magic = [0u8; 4];
    r.read_exact(&mut magic)?;
    if magic != HANDSHAKE_MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData,
            format!("bad handshake magic: {magic:?}")));
    }
    let ver = read_u32(r)?;
    if ver != PROTOCOL_VERSION {
        return Err(io::Error::new(io::ErrorKind::InvalidData,
            format!("protocol version mismatch: daemon {ver}, client {PROTOCOL_VERSION}")));
    }
    Ok(())
}

// ---- typed request / response structs the daemon dispatches on ----------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LabelDef { pub name: String, pub address: u64 }

#[derive(Debug, Clone)]
pub enum Request {
    Assemble                         { src: String, address: u64, labels: Vec<LabelDef>, with_relocs: bool, want_instructions: bool },
    Disassemble                      { bytes: Vec<u8>, address: u64, count: u32, want_instructions: bool },
}

impl Request {
    pub fn write(&self, w: &mut impl Write) -> io::Result<()> {
        let mut body = Vec::with_capacity(256);
        let opcode: u8 = match self {
            Request::Assemble { with_relocs: false, want_instructions: false, .. } => Opcode::Assemble as u8,
            Request::Assemble { with_relocs: false, want_instructions: true,  .. } => Opcode::AssembleToInstructions as u8,
            Request::Assemble { with_relocs: true,  want_instructions: false, .. } => Opcode::AssembleWithRelocs as u8,
            Request::Assemble { with_relocs: true,  want_instructions: true,  .. } => Opcode::AssembleToInstructionsWithRelocs as u8,
            Request::Disassemble { want_instructions: false, .. } => Opcode::Disassemble as u8,
            Request::Disassemble { want_instructions: true,  .. } => Opcode::DisassembleToInstructions as u8,
        };
        match self {
            Request::Assemble { src, address, labels, .. } => {
                write_u64(&mut body, *address)?;
                write_u32(&mut body, labels.len() as u32)?;
                for l in labels {
                    write_str(&mut body, &l.name)?;
                    write_u64(&mut body, l.address)?;
                }
                write_str(&mut body, src)?;
            }
            Request::Disassemble { bytes, address, count, .. } => {
                write_u64(&mut body, *address)?;
                write_u32(&mut body, *count)?;
                write_bytes(&mut body, bytes)?;
            }
        }
        let body_len: u32 = body.len().try_into()
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput,
                format!("request body of {} bytes does not fit a u32 length prefix",
                        body.len())))?;
        // FrameHeader::write enforces MAX_FRAME before anything is sent.
        FrameHeader { body_len, kind: opcode, flags: 0 }.write(w)?;
        w.write_all(&body)
    }

    pub fn read(r: &mut impl Read) -> io::Result<Request> {
        let (h, body) = read_frame(r)?;
        parse_request(h.kind, &body)
    }
}

/// Read one complete frame (header + body) off the stream.  Errors here mean
/// the stream is unusable (EOF, short read, oversized frame); errors from the
/// subsequent [`parse_request`] mean the frame was consumed but malformed,
/// the stream itself is still in sync.
pub fn read_frame(r: &mut impl Read) -> io::Result<(FrameHeader, Vec<u8>)> {
    let h = FrameHeader::read(r)?;   // validates body_len <= MAX_FRAME
    let mut body = vec![0u8; h.body_len as usize];
    r.read_exact(&mut body)?;
    Ok((h, body))
}

/// Parse an already-read request body.  A failure here (unknown opcode,
/// invalid UTF-8, truncated body) is recoverable for the daemon: the frame
/// was fully consumed, so it can reply `ERR_PROTOCOL` and keep serving.
pub fn parse_request(kind: u8, body: &[u8]) -> io::Result<Request> {
    let mut b = body;
    match kind {
        x if x == Opcode::Assemble                         as u8 => parse_assemble(&mut b, false, false),
        x if x == Opcode::AssembleToInstructions           as u8 => parse_assemble(&mut b, false, true),
        x if x == Opcode::AssembleWithRelocs               as u8 => parse_assemble(&mut b, true,  false),
        x if x == Opcode::AssembleToInstructionsWithRelocs as u8 => parse_assemble(&mut b, true,  true),
        x if x == Opcode::Disassemble                      as u8 => parse_disasm(&mut b, false),
        x if x == Opcode::DisassembleToInstructions        as u8 => parse_disasm(&mut b, true),
        other => Err(io::Error::new(io::ErrorKind::InvalidData,
            format!("unknown opcode: {other}"))),
    }
}

fn parse_assemble(r: &mut &[u8], with_relocs: bool, want_instructions: bool) -> io::Result<Request> {
    let address = read_u64(r)?;
    let n = read_u32(r)? as usize;
    let mut labels = Vec::with_capacity(n);
    for _ in 0..n {
        let name = read_str(r)?;
        let address = read_u64(r)?;
        labels.push(LabelDef { name, address });
    }
    let src = read_str(r)?;
    Ok(Request::Assemble { src, address, labels, with_relocs, want_instructions })
}

fn parse_disasm(r: &mut &[u8], want_instructions: bool) -> io::Result<Request> {
    let address = read_u64(r)?;
    let count   = read_u32(r)?;
    let bytes   = read_bytes(r)?;
    Ok(Request::Disassemble { bytes, address, count, want_instructions })
}

// ---- response body builders / parsers -----------------------------------
//
// The daemon assembles a body Vec<u8>, then sends it with a FrameHeader.
// The client uses the matching read_* helper for the operation it issued.

pub fn write_response_header(w: &mut impl Write, status: u8, flags: u8, body_len: u32) -> io::Result<()> {
    FrameHeader { body_len, kind: status, flags }.write(w)
}

/// Write an `Err(...)` response body, string message after the status header.
pub fn write_err_body(buf: &mut Vec<u8>, msg: &str) {
    let _ = write_str(buf, msg);
}

/// Read a status header, then either return Ok((flags, body_reader)) or an Err
/// with the decoded message.
pub fn read_response_status<R: Read>(r: &mut R) -> io::Result<(u8, u8, Vec<u8>)> {
    let (h, body) = read_frame(r)?;  // enforces MAX_FRAME before allocating
    Ok((h.kind, h.flags, body))
}

// ---- payload encoders ---------------------------------------------------

/// Append an instruction record { address, asm, bytes } to `body`.
/// Fails only if a single field exceeds [`MAX_FRAME`].
pub fn write_instruction(body: &mut Vec<u8>, address: u64, asm: &str, bytes: &[u8])
    -> io::Result<()>
{
    write_u64(body, address)?;
    write_str(body, asm)?;
    write_bytes(body, bytes)
}

/// Append a relocation record.  Fails only if a single field exceeds
/// [`MAX_FRAME`].
pub fn write_relocation(
    body: &mut Vec<u8>,
    offset: u64, addend: Option<i64>, reloc_type: u32,
    sym_name: &str, sym_addr: u64,
) -> io::Result<()> {
    write_u64(body, offset)?;
    write_u8(body, if addend.is_some() { 1 } else { 0 })?;
    write_i64(body, addend.unwrap_or(0))?;
    write_u32(body, reloc_type)?;
    write_str(body, sym_name)?;
    write_u64(body, sym_addr)
}

// ---- payload decoders (used by the client) ------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WireInstruction {
    pub address: u64,
    pub assembly: String,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WireRelocation {
    pub offset: u64,
    pub addend: Option<i64>,
    pub reloc_type: u32,
    pub sym_name: String,
    pub sym_addr: u64,
}

pub fn read_instructions(r: &mut &[u8]) -> io::Result<Vec<WireInstruction>> {
    let n = read_u32(r)? as usize;
    let mut v = Vec::with_capacity(n);
    for _ in 0..n {
        let address = read_u64(r)?;
        let assembly = read_str(r)?;
        let bytes = read_bytes(r)?;
        v.push(WireInstruction { address, assembly, bytes });
    }
    Ok(v)
}

pub fn read_relocations(r: &mut &[u8]) -> io::Result<Vec<WireRelocation>> {
    let n = read_u32(r)? as usize;
    let mut v = Vec::with_capacity(n);
    for _ in 0..n {
        let offset = read_u64(r)?;
        let has_addend = read_u8(r)?;
        let addend = read_i64(r)?;
        let reloc_type = read_u32(r)?;
        let sym_name = read_str(r)?;
        let sym_addr = read_u64(r)?;
        v.push(WireRelocation {
            offset,
            addend: if has_addend != 0 { Some(addend) } else { None },
            reloc_type,
            sym_name,
            sym_addr,
        });
    }
    Ok(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_bytes_rejects_oversized_prefix_before_allocating() {
        // A corrupt 4 GiB length prefix must error out, not allocate.
        let mut data = Vec::new();
        write_u32(&mut data, u32::MAX).unwrap();
        let err = read_bytes(&mut data.as_slice()).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    #[test]
    fn frame_header_read_rejects_oversized_body_len() {
        let mut data = Vec::new();
        write_u32(&mut data, MAX_FRAME + 1).unwrap();   // body_len
        data.extend_from_slice(&[0, 0, 0, 0]);          // kind/flags/reserved
        let err = FrameHeader::read(&mut data.as_slice()).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }

    #[test]
    fn write_side_rejects_oversized_payload_before_any_byte_is_written() {
        // write_bytes: oversized buffer → error, nothing written.
        let big = vec![0u8; MAX_FRAME as usize + 1];
        let mut out = Vec::new();
        let err = write_bytes(&mut out, &big).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(out.is_empty(), "no bytes may hit the wire on rejection");

        // FrameHeader: oversized body_len → error, nothing written.
        let mut out = Vec::new();
        let err = FrameHeader { body_len: MAX_FRAME + 1, kind: 0, flags: 0 }
            .write(&mut out).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
        assert!(out.is_empty(), "no bytes may hit the wire on rejection");
    }

    #[test]
    fn parse_request_unknown_opcode_is_recoverable_error() {
        let err = parse_request(0x7F, &[]).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
    }
}
