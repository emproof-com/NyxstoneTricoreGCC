//! Mutex-wrapped UDS stream that enforces strict FIFO request/response.
//!
//! All `NyxstoneTricoreGCC` methods take `&self`.  Concurrent calls from
//! multiple threads serialize through `Mutex<Inner>` here, same contract as
//! the `nyxstone-tricore-gcc` GPL crate's `GLOBAL_LOCK`, just scoped per-instance
//! instead of process-wide (because each MIT-mode instance has its own daemon
//! by design).
//!
//! # Resilience
//!
//! Every request is bounded by a read+write socket timeout (default 30 s,
//! overridable via `NYXSTONE_TCD_TIMEOUT_MS`, read once when the connection is
//! created; `0` disables the timeout).  After ANY transport error in
//! [`Connection::round_trip`] (timeout, EOF, ECONNRESET, short read) the
//! stream may hold partial frames and is never reused: the daemon is torn
//! down, respawned once with a fresh handshake, and the request is retried
//! exactly once.  Daemon-level error replies (gas diagnostics etc.) are valid
//! responses and are never retried.

use std::io::{self, BufReader, BufWriter, Write};
use std::os::unix::net::UnixStream;
use std::sync::Mutex;
use std::time::Duration;

use crate::daemon::Daemon;
use crate::protocol::{self as p, status};
use crate::{Error, Instruction, LabelDefinition, RelocationInfo, RelocationSymbol};

/// Default per-request socket timeout when `NYXSTONE_TCD_TIMEOUT_MS` is unset.
const DEFAULT_TIMEOUT_MS: u64 = 30_000;

pub(crate) struct Connection {
    /// `None` after a transport failure tore the connection down (a desynced
    /// stream is never kept around: it could hold a stale response that a
    /// later request would silently consume).  The next call reconnects.
    inner: Mutex<Option<Inner>>,
    /// Socket timeout resolved once at connection creation; reused verbatim
    /// when the daemon is respawned.
    timeout: Option<Duration>,
}

struct Inner {
    // Field order matters: reader/writer (socket clones) drop before `daemon`,
    // so the daemon sees EOF before Drop waits for it to exit.
    reader: BufReader<UnixStream>,
    writer: BufWriter<UnixStream>,
    // Owned for its Drop side-effect (tear down + reap the child); only read
    // by the #[cfg(test)] pid hook.
    #[allow(dead_code)]
    daemon: Daemon,
}

/// Resolve the per-request socket timeout from `NYXSTONE_TCD_TIMEOUT_MS`.
/// Read once at connection creation.  `0` disables the timeout; unparsable
/// values fall back to the default.
fn timeout_from_env() -> Option<Duration> {
    match std::env::var("NYXSTONE_TCD_TIMEOUT_MS") {
        Ok(s) => match s.trim().parse::<u64>() {
            Ok(0)  => None,
            Ok(ms) => Some(Duration::from_millis(ms)),
            Err(_) => Some(Duration::from_millis(DEFAULT_TIMEOUT_MS)),
        },
        Err(_) => Some(Duration::from_millis(DEFAULT_TIMEOUT_MS)),
    }
}

/// Spawn a daemon, apply socket timeouts, and complete the handshake.
fn connect(timeout: Option<Duration>) -> Result<Inner, Error> {
    let daemon = Daemon::spawn()
        .map_err(|e| Error::Init(format!("spawn nyxstone-tcd: {e}")))?;
    // SO_RCVTIMEO/SO_SNDTIMEO live on the socket itself, so setting them once
    // here also covers the try_clone()d reader/writer handles below.
    daemon.stream.set_read_timeout(timeout)
        .map_err(|e| Error::Init(format!("set read timeout: {e}")))?;
    daemon.stream.set_write_timeout(timeout)
        .map_err(|e| Error::Init(format!("set write timeout: {e}")))?;
    let reader = BufReader::new(daemon.stream.try_clone()
        .map_err(|e| Error::Init(format!("dup socket: {e}")))?);
    let writer = BufWriter::new(daemon.stream.try_clone()
        .map_err(|e| Error::Init(format!("dup socket: {e}")))?);
    let mut inner = Inner { reader, writer, daemon };

    // Handshake: read the daemon's magic + version, fail loudly on mismatch.
    p::read_handshake(&mut inner.reader)
        .map_err(|e| Error::Init(format!("handshake: {e}")))?;

    Ok(inner)
}

impl Connection {
    pub(crate) fn new() -> Result<Self, Error> {
        let timeout = timeout_from_env();
        let inner = connect(timeout)?;
        Ok(Self { inner: Mutex::new(Some(inner)), timeout })
    }

    /// Write one request and read its response, FIFO.
    fn send_recv(inner: &mut Inner, req: &p::Request) -> io::Result<(u8, Vec<u8>)> {
        req.write(&mut inner.writer)?;
        inner.writer.flush()?;
        let (status_byte, _flags, body) = p::read_response_status(&mut inner.reader)?;
        Ok((status_byte, body))
    }

    fn round_trip(&self, req: p::Request) -> Result<(u8, Vec<u8>), Error> {
        let mut g = self.inner.lock().map_err(|_| Error::Init("mutex poisoned".into()))?;
        // Reconnect lazily if an earlier transport failure tore the
        // connection down.
        if g.is_none() { *g = Some(connect(self.timeout)?); }
        match Self::send_recv(g.as_mut().expect("connected above"), &req) {
            Ok(ok) => Ok(ok),
            // InvalidInput = client-side validation (e.g. payload exceeds
            // MAX_FRAME): nothing was written to the wire, the stream is
            // still in sync, and a respawn+retry would fail identically.
            Err(e) if e.kind() == io::ErrorKind::InvalidInput => Err(ipc_err(e)),
            Err(_first) => {
                // Any other transport error (timeout, EOF, ECONNRESET, short
                // read) leaves the stream desynced; never reuse it.  Tear the
                // daemon down (dropping Inner closes the stale socket and
                // reaps the child), respawn it once (fresh handshake), and
                // retry the request exactly once.
                *g = None;
                let mut fresh = connect(self.timeout)?;
                match Self::send_recv(&mut fresh, &req) {
                    Ok(ok) => { *g = Some(fresh); Ok(ok) }
                    // Keep the connection torn down; the next call reconnects.
                    Err(e) => Err(ipc_err(e)),
                }
            }
        }
    }

    /// Test hook: pid of the currently connected daemon process.
    #[cfg(test)]
    pub(crate) fn daemon_pid(&self) -> u32 {
        self.inner.lock().unwrap().as_ref().expect("no live daemon").daemon.pid()
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
            bytes: bytes.to_vec(), address, count: wire_count(count)?,
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
            bytes: bytes.to_vec(), address, count: wire_count(count)?,
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

/// Convert the API-level `count: usize` to the wire's u32.  `count as u32`
/// would truncate 2^32 to 0, which INVERTS the semantics (0 = "decode all"),
/// so overflow must be a hard error.
fn wire_count(count: usize) -> Result<u32, Error> {
    count.try_into().map_err(|_| Error::DisassembleFailed(format!(
        "count {count} exceeds the wire-format maximum of {}", u32::MAX)))
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
