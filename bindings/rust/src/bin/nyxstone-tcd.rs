//! `nyxstone-tcd`, GPL-3.0+ daemon that hosts the in-process
//! `NyxstoneTricoreGCC` and serves requests over a UNIX socket inherited from
//! its parent via the `NYXSTONE_TCD_FD` env var.
//!
//! See [bindings/rust-ipc/src/protocol.rs](../../../rust-ipc/src/protocol.rs)
//! for the wire format.  The daemon is single-threaded, gas's globals are
//! process-wide, and serves strict FIFO.
//!
//! Lifecycle:
//!   - On exec, optionally installs `PR_SET_PDEATHSIG(SIGTERM)` (env-gated).
//!   - Initializes the in-process gas instance (one-time per process).
//!   - Writes the handshake bytes the client validates.
//!   - Loops: read request frame → dispatch → write response frame.
//!   - Exits cleanly when the client closes its socket end (read EOF).

use std::env;
use std::io::{BufReader, BufWriter, Read, Write};
use std::os::unix::io::FromRawFd;
use std::os::unix::net::UnixStream;

use nyxstone_tricore_gcc::{LabelDefinition, NyxstoneTricoreGCC};
use nyxstone_tricore_gcc_ipc::protocol::{
    self as p, status, write_response_header,
};

fn main() {
    // Optional: kernel-managed lifetime, die when parent does.
    #[cfg(target_os = "linux")]
    if env::var("NYXSTONE_TCD_PDEATH").as_deref() == Ok("1") {
        unsafe {
            libc::prctl(libc::PR_SET_PDEATHSIG,
                        libc::SIGTERM as libc::c_ulong, 0, 0, 0);
        }
    }

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
        // Read one request.  EOF = client closed socket → graceful exit.
        let req = match p::Request::read(&mut r) {
            Ok(req) => req,
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(e),
        };
        let (status, flags, body) = dispatch(nyx, &req);
        write_response_header(&mut w, status, flags, body.len() as u32)?;
        w.write_all(&body)?;
        w.flush()?;
    }
}

fn dispatch(nyx: &NyxstoneTricoreGCC, req: &p::Request) -> (u8, u8, Vec<u8>) {
    match req {
        p::Request::Assemble { src, address, labels, with_relocs: false, want_instructions: false } => {
            match nyx.assemble(src, *address, &to_gpl_labels(labels)) {
                Ok(bytes) => {
                    let mut body = Vec::with_capacity(bytes.len() + 8);
                    let _ = p::write_bytes(&mut body, &bytes);
                    (status::OK, 0, body)
                }
                Err(e) => err_response(e),
            }
        }
        p::Request::Assemble { src, address, labels, with_relocs: false, want_instructions: true } => {
            match nyx.assemble_to_instructions(src, *address, &to_gpl_labels(labels)) {
                Ok(insns) => (status::OK, 0, encode_instructions(&insns)),
                Err(e)    => err_response(e),
            }
        }
        p::Request::Assemble { src, address, labels, with_relocs: true, want_instructions: false } => {
            match nyx.assemble_with_relocs(src, *address, &to_gpl_labels(labels)) {
                Ok((bytes, relocs)) => {
                    let mut body = Vec::with_capacity(bytes.len() + relocs.len() * 64 + 16);
                    let _ = p::write_bytes(&mut body, &bytes);
                    encode_relocs(&mut body, &relocs);
                    (status::OK, 1, body)
                }
                Err(e) => err_response(e),
            }
        }
        p::Request::Assemble { src, address, labels, with_relocs: true, want_instructions: true } => {
            match nyx.assemble_to_instructions_with_relocs(src, *address, &to_gpl_labels(labels)) {
                Ok((insns, relocs)) => {
                    let mut body = encode_instructions(&insns);
                    encode_relocs(&mut body, &relocs);
                    (status::OK, 1, body)
                }
                Err(e) => err_response(e),
            }
        }
        p::Request::Disassemble { bytes, address, count, want_instructions: false } => {
            match nyx.disassemble(bytes, *address, *count as usize) {
                Ok(s) => {
                    let mut body = Vec::with_capacity(s.len() + 4);
                    let _ = p::write_bytes(&mut body, s.as_bytes());
                    (status::OK, 0, body)
                }
                Err(e) => err_response(e),
            }
        }
        p::Request::Disassemble { bytes, address, count, want_instructions: true } => {
            match nyx.disassemble_to_instructions(bytes, *address, *count as usize) {
                Ok(insns) => (status::OK, 0, encode_instructions(&insns)),
                Err(e)    => err_response(e),
            }
        }
    }
}

fn to_gpl_labels(ls: &[p::LabelDef]) -> Vec<LabelDefinition> {
    ls.iter().map(|l| LabelDefinition::new(l.name.clone(), l.address)).collect()
}

fn encode_instructions(insns: &[nyxstone_tricore_gcc::Instruction]) -> Vec<u8> {
    let mut body = Vec::with_capacity(insns.len() * 32 + 4);
    let _ = p::write_u32(&mut body, insns.len() as u32);
    for i in insns {
        p::write_instruction(&mut body, i.address, &i.assembly, &i.bytes);
    }
    body
}

fn encode_relocs(body: &mut Vec<u8>, relocs: &[nyxstone_tricore_gcc::RelocationInfo]) {
    let _ = p::write_u32(body, relocs.len() as u32);
    for r in relocs {
        p::write_relocation(body, r.offset, r.addend, r.relocation_type,
                            &r.symbol.name, r.symbol.address);
    }
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
