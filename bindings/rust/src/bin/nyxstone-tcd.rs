//! `nyxstone-tcd`, GPL-3.0+ daemon that hosts the in-process
//! `NyxstoneTricoreGCC` and serves requests over a UNIX socket inherited from
//! its parent via the `NYXSTONE_TCD_FD` env var.
//!
//! See [bindings/rust-ipc/src/protocol.rs](../../../rust-ipc/src/protocol.rs)
//! for the wire format.  The daemon is single-threaded, gas's globals are
//! process-wide, and serves strict FIFO.
//!
//! Lifecycle:
//!   - Initializes the in-process gas instance (one-time per process).
//!   - Writes the handshake bytes the client validates.
//!   - Loops: read request frame → dispatch → write response frame.
//!     Malformed request bodies (unknown opcode, invalid UTF-8) get an
//!     ERR_PROTOCOL reply and the daemon keeps serving; only frame-level
//!     stream errors (which desync the socket) are fatal.
//!   - Exits cleanly when the client closes its socket end (read EOF), which
//!     also covers the parent process dying (the OS closes the fd).  We do not
//!     use PR_SET_PDEATHSIG: it is parent-thread-scoped on Linux and would kill
//!     the daemon when the spawning thread exits while the client lives on.

use std::env;
use std::io::{BufReader, BufWriter, Read, Write};
use std::os::unix::io::FromRawFd;
use std::os::unix::net::UnixStream;

use nyxstone_tricore_gcc::{LabelDefinition, NyxstoneTricoreGCC};
use nyxstone_tricore_gcc_ipc::protocol::{
    self as p, status, write_response_header,
};

fn main() {
    // Resolve the inherited socket FD.
    let fd: i32 = match env::var("NYXSTONE_TCD_FD") {
        Ok(s) => match s.parse() {
            Ok(n) => n,
            Err(e) => {
                eprintln!("nyxstone-tcd: NYXSTONE_TCD_FD parse: {e}");
                std::process::exit(2);
            }
        },
        Err(_) => {
            eprintln!("nyxstone-tcd: NYXSTONE_TCD_FD env var not set, this binary is \
                       meant to be spawned by the nyxstone-tricore-gcc-ipc client.");
            std::process::exit(2);
        }
    };
    let stream = unsafe { UnixStream::from_raw_fd(fd) };

    // Initialise the gas-backed encoder.  One per process.
    let nyx = match NyxstoneTricoreGCC::new() {
        Ok(n) => n,
        Err(e) => {
            eprintln!("nyxstone-tcd: NyxstoneTricoreGCC init: {e}");
            std::process::exit(3);
        }
    };

    let reader = BufReader::new(stream.try_clone().expect("dup socket"));
    let writer = BufWriter::new(stream);
    if let Err(e) = serve(reader, writer, &nyx) {
        eprintln!("nyxstone-tcd: serve loop exited: {e}");
        std::process::exit(4);
    }
}

fn serve(mut r: impl Read, mut w: impl Write, nyx: &NyxstoneTricoreGCC)
    -> std::io::Result<()>
{
    p::write_handshake(&mut w)?;
    w.flush()?;
    loop {
        // Read one raw frame.  EOF = client closed socket → graceful exit.
        // Other frame-level errors (short read, oversized body_len) leave the
        // stream desynced, so they remain fatal.
        let (header, raw_body) = match p::read_frame(&mut r) {
            Ok(f) => f,
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(e),
        };
        // The frame is fully consumed at this point, so a request-body parse
        // error (unknown opcode, invalid UTF-8, truncated body) must NOT kill
        // the daemon: reply ERR_PROTOCOL and keep serving.
        let (status, flags, body) = match p::parse_request(header.kind, &raw_body) {
            Ok(req) => dispatch(nyx, &req),
            Err(e)  => protocol_err_response(&format!("bad request: {e}")),
        };
        // Never emit a frame the client would reject (or that would truncate
        // the u32 length prefix); downgrade to an in-band protocol error.
        let (status, flags, body) = match u32::try_from(body.len()) {
            Ok(n) if n <= p::MAX_FRAME => (status, flags, body),
            _ => protocol_err_response(&format!(
                "response body of {} bytes exceeds MAX_FRAME ({} bytes)",
                body.len(), p::MAX_FRAME)),
        };
        write_response_header(&mut w, status, flags, body.len() as u32)?;
        w.write_all(&body)?;
        w.flush()?;
    }
}

fn protocol_err_response(msg: &str) -> (u8, u8, Vec<u8>) {
    let mut body = Vec::with_capacity(msg.len() + 4);
    p::write_err_body(&mut body, msg);
    (status::ERR_PROTOCOL, 0, body)
}

fn dispatch(nyx: &NyxstoneTricoreGCC, req: &p::Request) -> (u8, u8, Vec<u8>) {
    match req {
        p::Request::Assemble { src, address, labels, with_relocs: false, want_instructions: false } => {
            match nyx.assemble(src, *address, &to_gpl_labels(labels)) {
                Ok(bytes) => ok_or_protocol(0, {
                    let mut body = Vec::with_capacity(bytes.len() + 8);
                    p::write_bytes(&mut body, &bytes).map(|_| body)
                }),
                Err(e) => err_response(e),
            }
        }
        p::Request::Assemble { src, address, labels, with_relocs: false, want_instructions: true } => {
            match nyx.assemble_to_instructions(src, *address, &to_gpl_labels(labels)) {
                Ok(insns) => ok_or_protocol(0, encode_instructions(&insns)),
                Err(e)    => err_response(e),
            }
        }
        p::Request::Assemble { src, address, labels, with_relocs: true, want_instructions: false } => {
            match nyx.assemble_with_relocs(src, *address, &to_gpl_labels(labels)) {
                Ok((bytes, relocs)) => ok_or_protocol(1, (|| {
                    let mut body = Vec::with_capacity(bytes.len() + relocs.len() * 64 + 16);
                    p::write_bytes(&mut body, &bytes)?;
                    encode_relocs(&mut body, &relocs)?;
                    Ok(body)
                })()),
                Err(e) => err_response(e),
            }
        }
        p::Request::Assemble { src, address, labels, with_relocs: true, want_instructions: true } => {
            match nyx.assemble_to_instructions_with_relocs(src, *address, &to_gpl_labels(labels)) {
                Ok((insns, relocs)) => ok_or_protocol(1, (|| {
                    let mut body = encode_instructions(&insns)?;
                    encode_relocs(&mut body, &relocs)?;
                    Ok(body)
                })()),
                Err(e) => err_response(e),
            }
        }
        p::Request::Disassemble { bytes, address, count, want_instructions: false } => {
            match nyx.disassemble(bytes, *address, *count as usize) {
                Ok(s) => ok_or_protocol(0, {
                    let mut body = Vec::with_capacity(s.len() + 4);
                    p::write_bytes(&mut body, s.as_bytes()).map(|_| body)
                }),
                Err(e) => err_response(e),
            }
        }
        p::Request::Disassemble { bytes, address, count, want_instructions: true } => {
            match nyx.disassemble_to_instructions(bytes, *address, *count as usize) {
                Ok(insns) => ok_or_protocol(0, encode_instructions(&insns)),
                Err(e)    => err_response(e),
            }
        }
    }
}

fn to_gpl_labels(ls: &[p::LabelDef]) -> Vec<LabelDefinition> {
    ls.iter().map(|l| LabelDefinition::new(l.name.clone(), l.address)).collect()
}

/// Wrap a fallibly-encoded response body: encoding only fails when a field
/// exceeds the protocol's MAX_FRAME cap, which becomes an in-band
/// ERR_PROTOCOL reply instead of killing the daemon.
fn ok_or_protocol(flags: u8, body: std::io::Result<Vec<u8>>) -> (u8, u8, Vec<u8>) {
    match body {
        Ok(body) => (status::OK, flags, body),
        Err(e)   => protocol_err_response(&format!("encode response: {e}")),
    }
}

fn encode_instructions(insns: &[nyxstone_tricore_gcc::Instruction])
    -> std::io::Result<Vec<u8>>
{
    let mut body = Vec::with_capacity(insns.len() * 32 + 4);
    p::write_u32(&mut body, insns.len() as u32)?;
    for i in insns {
        p::write_instruction(&mut body, i.address, &i.assembly, &i.bytes)?;
    }
    Ok(body)
}

fn encode_relocs(body: &mut Vec<u8>, relocs: &[nyxstone_tricore_gcc::RelocationInfo])
    -> std::io::Result<()>
{
    p::write_u32(body, relocs.len() as u32)?;
    for r in relocs {
        p::write_relocation(body, r.offset, r.addend, r.relocation_type,
                            &r.symbol.name, r.symbol.address)?;
    }
    Ok(())
}

fn err_response(e: nyxstone_tricore_gcc::Error) -> (u8, u8, Vec<u8>) {
    use nyxstone_tricore_gcc::Error::*;
    let (status, msg) = match e {
        Init(m)              => (status::ERR_INIT,     m),
        AssembleFailed(m)    => (status::ERR_ASSEMBLE, m),
        SectionViolation(m)  => (status::ERR_SECTION,  m),
        DisassembleFailed(m) => (status::ERR_DISASM,   m),
        Alloc                => (status::ERR_ALLOC,    String::new()),
        NullArg              => (status::ERR_NULL_ARG, String::new()),
        Unknown(c, m)        => (status::ERR_UNKNOWN,  format!("({c}) {m}")),
    };
    let mut body = Vec::with_capacity(msg.len() + 4);
    p::write_err_body(&mut body, &msg);
    (status, 0, body)
}
