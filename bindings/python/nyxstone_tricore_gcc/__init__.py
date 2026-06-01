"""NyxstoneTricoreGCC, Python bindings.

In-process TriCore assembler/disassembler.  The Python surface is a thin,
Pythonic wrapper around the same C ABI used by the Rust binding.  The API
shape mirrors that of the sibling project
`Nyxstone <https://github.com/emproof-com/nyxstone>`_, a separate codebase
built on LLVM-MC, covering the architectures LLVM supports.  This package
is an independent implementation using GNU binutils to cover TriCore
(which LLVM-MC has no backend for).  Both projects expose four methods
named :meth:`NyxstoneTricoreGCC.assemble`,
:meth:`NyxstoneTricoreGCC.assemble_to_instructions`,
:meth:`NyxstoneTricoreGCC.disassemble`, and
:meth:`NyxstoneTricoreGCC.disassemble_to_instructions`, all taking an
explicit ``address`` and (for the assembly entry points) an iterable of
:class:`LabelDefinition`\\ s for external symbols.

Example::

    >>> from nyxstone_tricore_gcc import NyxstoneTricoreGCC
    >>> nx = NyxstoneTricoreGCC()
    >>> nx.assemble("start:\\n nop\\n j here\\nhere:\\n ret\\n", address=0)
    b'\\x00\\x00\\x1d\\x00\\x00\\x00\\x00\\x90'
    >>> for ins in nx.disassemble_to_instructions(b, address=0x80000000):
    ...     print(f"{ins.address:#010x}  {ins.assembly}")

Threading: a process-wide lock serializes calls into the C library because
all gas globals are process-wide.  Holding multiple ``NyxstoneTricoreGCC``
instances is fine; calls are simply serialized.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Iterable, List, Optional, Sequence

from ._native import ffi, lib  # type: ignore[import-not-found]

__all__ = [
    "NyxstoneTricoreGCC",
    "Instruction",
    "LabelDefinition",
    "RelocationInfo",
    "RelocationSymbol",
    "NyxstoneError",
    "AssembleError",
    "SectionViolationError",
    "DisassembleError",
]

_LOCK = threading.Lock()


class NyxstoneError(Exception):
    """Base class for all Nyxstone-TriCore errors."""


class AssembleError(NyxstoneError):
    """gas couldn't parse / encode the source."""


class SectionViolationError(NyxstoneError):
    """The source contained a directive that would switch the active section
    to something other than .text (e.g. .data, .bss, .section .foo)."""


class DisassembleError(NyxstoneError):
    """libopcodes couldn't decode some byte in the buffer."""


@dataclass
class LabelDefinition:
    """External label definition (input to assemble / assemble_to_instructions)."""
    name:    str
    address: int


@dataclass
class Instruction:
    """One disassembled (or freshly assembled) instruction."""
    address:  int
    assembly: str
    bytes:    bytes


@dataclass
class RelocationSymbol:
    """Symbol target of a relocation.

    `address` is copied from the matching :class:`LabelDefinition` (or 0 if
    the caller didn't supply one)."""
    name:    str
    address: int


@dataclass
class RelocationInfo:
    """One relocation entry, same shape gas/gcc emits with ``-r``."""
    offset:          int
    addend:          Optional[int]
    symbol:          RelocationSymbol
    relocation_type: int


_STATUS_OK                    = 0
_STATUS_INIT                  = 1
_STATUS_NULL_ARG              = 2
_STATUS_ASSEMBLE_FAILED       = 3
_STATUS_SECTION_VIOLATION     = 4
_STATUS_DISASM_FAILED         = 5
_STATUS_ALLOC                 = 6


def _take_err(err_pp) -> str:
    p = err_pp[0]
    if p == ffi.NULL:
        return ""
    s = ffi.string(p).decode("utf-8", "replace")
    lib.nyxstone_free_string(p)
    return s


def _check(status: int, kind: str, msg: str) -> None:
    if status == _STATUS_OK:
        return
    detail = f": {msg}" if msg else ""
    if status == _STATUS_SECTION_VIOLATION:
        raise SectionViolationError(
            f"source contains a non-.text section directive{detail}")
    if status == _STATUS_ASSEMBLE_FAILED:
        raise AssembleError(f"gas parse/encode failed{detail}")
    if status == _STATUS_DISASM_FAILED:
        raise DisassembleError(f"libopcodes decode failed{detail}")
    if status == _STATUS_ALLOC:
        raise NyxstoneError(f"out of memory{detail}")
    if status == _STATUS_INIT:
        raise NyxstoneError(
            f"NyxstoneTricoreGCC init failed (libbfd/elf32-tricore?){detail}")
    raise NyxstoneError(f"{kind}: unknown status {status}{detail}")


def _pack_labels(labels: Optional[Sequence[LabelDefinition]]):
    """Convert an iterable of LabelDefinition into a (raw_array, name_keepalives)
    tuple.  The keepalives hold the CFFI-owned C strings so they live for the
    duration of the C call."""
    if not labels:
        return ffi.NULL, 0, []
    n = len(labels)
    raw = ffi.new("nyxstone_label_def_t[]", n)
    keep = []
    for i, l in enumerate(labels):
        nbuf = ffi.new("char[]", l.name.encode("utf-8"))
        keep.append(nbuf)
        raw[i].name    = nbuf
        raw[i].address = l.address
    return raw, n, keep


def _unpack_relocations(out_p, n: int) -> List[RelocationInfo]:
    result: List[RelocationInfo] = []
    for i in range(n):
        entry = out_p[0][i]
        name = ffi.string(entry.symbol.name).decode("utf-8", "replace") \
            if entry.symbol.name != ffi.NULL else ""
        result.append(RelocationInfo(
            offset=int(entry.offset),
            addend=int(entry.addend) if entry.has_addend else None,
            symbol=RelocationSymbol(name=name, address=int(entry.symbol.address)),
            relocation_type=int(entry.relocation_type),
        ))
    if out_p[0] != ffi.NULL:
        lib.nyxstone_free_relocations(out_p[0], n)
    return result


def _unpack_instructions(out_p, n: int) -> List[Instruction]:
    result: List[Instruction] = []
    for i in range(n):
        entry = out_p[0][i]
        assembly = ffi.string(entry.assembly).decode("utf-8", "replace")
        blen = int(entry.bytes_len)
        insn_bytes = bytes(ffi.buffer(entry.bytes, blen)) if blen else b""
        result.append(Instruction(
            address=int(entry.address),
            assembly=assembly,
            bytes=insn_bytes,
        ))
    if out_p[0] != ffi.NULL:
        lib.nyxstone_free_instructions(out_p[0], n)
    return result


class NyxstoneTricoreGCC:
    """In-process TriCore assembler/disassembler.

    The first instance per process triggers gas's one-time init; subsequent
    instances reuse it.  Methods are safe to call from multiple threads
    (a process-wide lock serializes them)."""

    def __init__(self) -> None:
        with _LOCK:
            err = ffi.new("char**")
            handle = lib.nyxstone_create(err)
            if handle == ffi.NULL:
                raise NyxstoneError(
                    f"nyxstone_create returned NULL: {_take_err(err)}")
            self._handle = handle

    def __del__(self) -> None:
        h = getattr(self, "_handle", None)
        if h is None or h == ffi.NULL:
            return
        with _LOCK:
            lib.nyxstone_destroy(h)
        self._handle = ffi.NULL

    # ----- assembly --------------------------------------------------------

    def assemble(
        self,
        assembly: str,
        address: int = 0,
        labels: Optional[Sequence[LabelDefinition]] = None,
    ) -> bytes:
        """Translate ``assembly`` to ``.text`` bytes at the given absolute
        ``address``, with optional external label definitions.  Raises
        :class:`AssembleError` on parse failure, :class:`SectionViolationError`
        on attempts to switch sections."""
        raw = assembly.encode("utf-8")
        labels_p, labels_n, _keep = _pack_labels(labels)
        out_p = ffi.new("uint8_t**")
        out_n = ffi.new("size_t*")
        err   = ffi.new("char**")
        with _LOCK:
            st = lib.nyxstone_assemble(self._handle, raw, len(raw),
                                  address,
                                  labels_p, labels_n,
                                  out_p, out_n, err)
        _check(st, "assemble", _take_err(err))
        n = int(out_n[0])
        if n == 0 or out_p[0] == ffi.NULL:
            return b""
        data = bytes(ffi.buffer(out_p[0], n))
        with _LOCK:
            lib.nyxstone_free_bytes(out_p[0])
        return data

    def assemble_to_instructions(
        self,
        assembly: str,
        address: int = 0,
        labels: Optional[Sequence[LabelDefinition]] = None,
    ) -> List[Instruction]:
        """Translate ``assembly`` to a list of :class:`Instruction`\\ s."""
        raw = assembly.encode("utf-8")
        labels_p, labels_n, _keep = _pack_labels(labels)
        out_p = ffi.new("nyxstone_instruction_t**")
        out_n = ffi.new("size_t*")
        err   = ffi.new("char**")
        with _LOCK:
            st = lib.nyxstone_assemble_to_instructions(self._handle, raw, len(raw),
                                                  address,
                                                  labels_p, labels_n,
                                                  out_p, out_n, err)
        _check(st, "assemble_to_instructions", _take_err(err))
        return _unpack_instructions(out_p, int(out_n[0]))

    def assemble_with_relocs(
        self,
        assembly: str,
        address: int = 0,
        labels: Optional[Sequence[LabelDefinition]] = None,
    ) -> "tuple[bytes, List[RelocationInfo]]":
        """Like :meth:`assemble` but leaves @p labels unresolved and returns
        one :class:`RelocationInfo` per external reference, equivalent to
        ``gcc/gas -r``.

        Returns ``(bytes, relocations)``."""
        raw = assembly.encode("utf-8")
        labels_p, labels_n, _keep = _pack_labels(labels)
        out_b = ffi.new("uint8_t**")
        out_n = ffi.new("size_t*")
        out_r = ffi.new("nyxstone_reloc_t**")
        out_rn = ffi.new("size_t*")
        err   = ffi.new("char**")
        with _LOCK:
            st = lib.nyxstone_assemble_with_relocs(self._handle, raw, len(raw),
                                              address,
                                              labels_p, labels_n,
                                              out_b, out_n,
                                              out_r, out_rn,
                                              err)
        _check(st, "assemble_with_relocs", _take_err(err))
        n = int(out_n[0])
        if n == 0 or out_b[0] == ffi.NULL:
            data = b""
        else:
            data = bytes(ffi.buffer(out_b[0], n))
            with _LOCK:
                lib.nyxstone_free_bytes(out_b[0])
        relocs = _unpack_relocations(out_r, int(out_rn[0]))
        return data, relocs

    def assemble_to_instructions_with_relocs(
        self,
        assembly: str,
        address: int = 0,
        labels: Optional[Sequence[LabelDefinition]] = None,
    ) -> "tuple[List[Instruction], List[RelocationInfo]]":
        """Like :meth:`assemble_to_instructions` but with ``-r``-style
        relocation output.  Returns ``(instructions, relocations)``."""
        raw = assembly.encode("utf-8")
        labels_p, labels_n, _keep = _pack_labels(labels)
        out_i = ffi.new("nyxstone_instruction_t**")
        out_in = ffi.new("size_t*")
        out_r = ffi.new("nyxstone_reloc_t**")
        out_rn = ffi.new("size_t*")
        err   = ffi.new("char**")
        with _LOCK:
            st = lib.nyxstone_assemble_to_instructions_with_relocs(
                self._handle, raw, len(raw),
                address,
                labels_p, labels_n,
                out_i, out_in,
                out_r, out_rn,
                err)
        _check(st, "assemble_to_instructions_with_relocs", _take_err(err))
        return _unpack_instructions(out_i, int(out_in[0])), \
               _unpack_relocations(out_r, int(out_rn[0]))

    # ----- disassembly -----------------------------------------------------

    def disassemble(
        self,
        data: bytes,
        address: int = 0,
        count: int = 0,
    ) -> str:
        """Disassemble ``data`` to text, starting at absolute ``address``.
        Decodes at most ``count`` instructions; pass ``0`` for all."""
        buf = ffi.new("uint8_t[]", len(data))
        ffi.memmove(buf, data, len(data))
        out_text = ffi.new("char**")
        err      = ffi.new("char**")
        with _LOCK:
            st = lib.nyxstone_disassemble(self._handle, buf, len(data),
                                     address, count, out_text, err)
        _check(st, "disassemble", _take_err(err))
        if out_text[0] == ffi.NULL:
            return ""
        s = ffi.string(out_text[0]).decode("utf-8", "replace")
        with _LOCK:
            lib.nyxstone_free_string(out_text[0])
        return s

    def disassemble_to_instructions(
        self,
        data: bytes,
        address: int = 0,
        count: int = 0,
    ) -> List[Instruction]:
        """Disassemble ``data`` into a list of :class:`Instruction`\\ s."""
        buf = ffi.new("uint8_t[]", len(data))
        ffi.memmove(buf, data, len(data))
        out_p = ffi.new("nyxstone_instruction_t**")
        out_n = ffi.new("size_t*")
        err   = ffi.new("char**")
        with _LOCK:
            st = lib.nyxstone_disassemble_to_instructions(self._handle, buf, len(data),
                                                     address, count,
                                                     out_p, out_n, err)
        _check(st, "disassemble_to_instructions", _take_err(err))
        return _unpack_instructions(out_p, int(out_n[0]))
