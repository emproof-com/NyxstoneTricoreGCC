//! Mutex-wrapped UDS stream that enforces strict FIFO request/response.
//!
//! All `NyxstoneTricoreGCC` methods take `&self`.  Concurrent calls from
//! multiple threads serialize through `Mutex<Inner>` here, same contract as
//! the `nyxstone-tricore-gcc` GPL crate's `GLOBAL_LOCK`, just scoped per-instance
//! instead of process-wide (because each MIT-mode instance has its own daemon
//! by design).

use std::io::{BufReader, BufWriter, Write};
use std::sync::Mutex;

use crate::daemon::Daemon;
use crate::protocol::{self as p, status};
use crate::{Error, Instruction, LabelDefinition, RelocationInfo, RelocationSymbol};

pub(crate) struct Connection {
    inner: Mutex<Inner>,
    // Owned daemon; dropped after Inner so the socket closes first.
    _daemon: Daemon,
}

struct Inner {
    reader: BufReader<std::os::unix::net::UnixStream>,
    writer: BufWriter<std::os::unix::net::UnixStream>,
}

impl Connection {
    pub(crate) fn new() -> Result<Self, Error> {
        let daemon = Daemon::spawn()
            .map_err(|e| Error::Init(format!("spawn nyxstone-tcd: {e}")))?;
        let stream = daemon.stream.try_clone()
            .map_err(|e| Error::Init(format!("dup socket: {e}")))?;
        let reader = BufReader::new(stream);
        let writer = BufWriter::new(daemon.stream.try_clone()
            .map_err(|e| Error::Init(format!("dup socket: {e}")))?);
        let mut inner = Inner { reader, writer };

        // Handshake: read the daemon's magic + version, fail loudly on mismatch.
        p::read_handshake(&mut inner.reader)
            .map_err(|e| Error::Init(format!("handshake: {e}")))?;

        Ok(Self { inner: Mutex::new(inner), _daemon: daemon })
    }

    fn round_trip(&self, req: p::Request) -> Result<(u8, Vec<u8>), Error> {
        let mut g = self.inner.lock().map_err(|_| Error::Init("mutex poisoned".into()))?;
        req.write(&mut g.writer).and_then(|_| g.writer.flush())
            .map_err(|e| ipc_err(e))?;
        let (status_byte, _flags, body) = p::read_response_status(&mut g.reader)
            .map_err(|e| ipc_err(e))?;
        Ok((status_byte, body))
    }

    pub(crate) fn assemble(&self, src: &str, address: u64, labels: &[LabelDefinition])
        -> Result<Vec<u8>, Error>
    {
        let (st, body) = self.round_trip(p::Request::Assemble {
            src: src.into(), address,
            labels: to_wire_labels(labels),
            with_relocs: false, want_instructions: false,
        })?;
        if st != status::OK { return Err(decode_err(st, &body)); }
        let mut r = body.as_slice();
        Ok(p::read_bytes(&mut r).map_err(ipc_err)?)
    }

    pub(crate) fn assemble_to_instructions(&self, src: &str, address: u64,
                                           labels: &[LabelDefinition])
        -> Result<Vec<Instruction>, Error>
    {
        let (st, body) = self.round_trip(p::Request::Assemble {
            src: src.into(), address,
            labels: to_wire_labels(labels),
            with_relocs: false, want_instructions: true,
        })?;
        if st != status::OK { return Err(decode_err(st, &body)); }
        let mut r = body.as_slice();
        let wire = p::read_instructions(&mut r).map_err(ipc_err)?;
        Ok(wire.into_iter().map(into_insn).collect())
    }

    pub(crate) fn assemble_with_relocs(&self, src: &str, address: u64,
                                       labels: &[LabelDefinition])
        -> Result<(Vec<u8>, Vec<RelocationInfo>), Error>
    {
        let (st, body) = self.round_trip(p::Request::Assemble {
            src: src.into(), address,
            labels: to_wire_labels(labels),
            with_relocs: true, want_instructions: false,
        })?;
        if st != status::OK { return Err(decode_err(st, &body)); }
        let mut r = body.as_slice();
        let bytes  = p::read_bytes(&mut r).map_err(ipc_err)?;
        let relocs = p::read_relocations(&mut r).map_err(ipc_err)?;
        Ok((bytes, relocs.into_iter().map(into_reloc).collect()))
    }

    pub(crate) fn assemble_to_instructions_with_relocs(
        &self, src: &str, address: u64, labels: &[LabelDefinition]
    ) -> Result<(Vec<Instruction>, Vec<RelocationInfo>), Error>
    {
        let (st, body) = self.round_trip(p::Request::Assemble {
            src: src.into(), address,
            labels: to_wire_labels(labels),
            with_relocs: true, want_instructions: true,
        })?;
        if st != status::OK { return Err(decode_err(st, &body)); }
        let mut r = body.as_slice();
        let wire   = p::read_instructions(&mut r).map_err(ipc_err)?;
        let relocs = p::read_relocations(&mut r).map_err(ipc_err)?;
        Ok((wire.into_iter().map(into_insn).collect(),
            relocs.into_iter().map(into_reloc).collect()))
    }

    pub(crate) fn disassemble(&self, bytes: &[u8], address: u64, count: usize)
        -> Result<String, Error>
    {
        let (st, body) = self.round_trip(p::Request::Disassemble {
            bytes: bytes.to_vec(), address, count: count as u32,
            want_instructions: false,
        })?;
        if st != status::OK { return Err(decode_err(st, &body)); }
        let mut r = body.as_slice();
        let b = p::read_bytes(&mut r).map_err(ipc_err)?;
        String::from_utf8(b).map_err(|e| Error::DisassembleFailed(format!("utf8: {e}")))
    }

    pub(crate) fn disassemble_to_instructions(&self, bytes: &[u8], address: u64, count: usize)
        -> Result<Vec<Instruction>, Error>
    {
        let (st, body) = self.round_trip(p::Request::Disassemble {
            bytes: bytes.to_vec(), address, count: count as u32,
            want_instructions: true,
        })?;
        if st != status::OK { return Err(decode_err(st, &body)); }
        let mut r = body.as_slice();
        let wire = p::read_instructions(&mut r).map_err(ipc_err)?;
        Ok(wire.into_iter().map(into_insn).collect())
    }
}

fn to_wire_labels(ls: &[LabelDefinition]) -> Vec<p::LabelDef> {
    ls.iter().map(|l| p::LabelDef { name: l.name.clone(), address: l.address }).collect()
}

fn into_insn(w: p::WireInstruction) -> Instruction {
    Instruction { address: w.address, assembly: w.assembly, bytes: w.bytes }
}

fn into_reloc(w: p::WireRelocation) -> RelocationInfo {
    RelocationInfo {
        offset: w.offset,
        addend: w.addend,
        symbol: RelocationSymbol { name: w.sym_name, address: w.sym_addr },
        relocation_type: w.reloc_type,
    }
}

fn ipc_err(e: std::io::Error) -> Error {
    Error::Init(format!("ipc: {e}"))
}

fn decode_err(status: u8, body: &[u8]) -> Error {
    let msg = String::from_utf8_lossy(
        body.get(4..).unwrap_or(b"") // skip the u32 length prefix
    ).into_owned();
    match status {
        status::ERR_INIT          => Error::Init(msg),
        status::ERR_NULL_ARG      => Error::NullArg,
        status::ERR_ASSEMBLE      => Error::AssembleFailed(msg),
        status::ERR_SECTION       => Error::SectionViolation(msg),
        status::ERR_DISASM        => Error::DisassembleFailed(msg),
        status::ERR_ALLOC         => Error::Alloc,
        status::ERR_PROTOCOL      => Error::Init(format!("protocol: {msg}")),
        other                     => Error::Unknown(other as i32, msg),
    }
}
