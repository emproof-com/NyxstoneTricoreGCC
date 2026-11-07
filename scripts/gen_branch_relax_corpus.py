#!/usr/bin/env python3
# Regenerate tests/branch_relax_corpus.inc: the bytes that gas emits for every
# PC-relative branch form, forward and backward, at distances that select each
# encoding of its relax chain (16-bit, 32-bit, and 32-bit plus a `j` over the
# limit of disp15).  tests/tests.cpp and the tests of both Rust crates assemble
# each source and compare with these bytes, at two addresses.  Run after a
# change to relax_branches() in src/nyxstone_glue.c or after bumping the pinned
# binutils commit:
#
#   TRICORE_AS=/path/to/tricore-elf-as python3 scripts/gen_branch_relax_corpus.py
#
# The reference is `tricore-elf-as -mtc162 --no-reloc`, the mode that selects
# the shortest reaching encoding of a local branch (the glue mirrors it).  The
# .text bytes are read straight from the ELF object.
#
# Line format of the corpus (C++ initializers, also parsed by the Rust tests):
#   {"<source, \n-escaped>", "<tokens>"},
# where each token is either a run of hex bytes or z<n> for n zero bytes.
import os
import struct
import subprocess
import sys
import tempfile

AS = os.environ.get("TRICORE_AS", "tricore-elf-as")
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "tests", "branch_relax_corpus.inc")

# Every branch mnemonic with the operand forms that have a 16-bit encoding
# (%d15, const4, %a15, zero compare) and the forms that have only a 32-bit one.
INSNS = [
    "j .t", "jl .t", "call .t", "fcall .t",
    "jz %d15, .t", "jnz %d15, .t", "jz %d1, .t", "jnz %d1, .t",
    "jeq %d15, 0, .t", "jne %d15, 0, .t", "jeq %d1, 0, .t", "jne %d1, 0, .t",
    "jeq %d15, 3, .t", "jne %d15, 3, .t", "jeq %d1, 3, .t", "jne %d1, 3, .t",
    "jeq %d15, -8, .t", "jne %d15, -5, .t",
    "jeq %d15, %d2, .t", "jne %d15, %d2, .t", "jeq %d1, %d2, .t", "jne %d1, %d2, .t",
    "jlt %d15, 3, .t", "jlt %d1, 3, .t", "jlt.u %d1, 3, .t",
    "jge %d15, 3, .t", "jge %d1, 3, .t", "jge.u %d1, 3, .t",
    "jlt %d1, %d2, .t", "jlt.u %d1, %d2, .t", "jge %d1, %d2, .t", "jge.u %d1, %d2, .t",
    "jgez %d15, .t", "jgtz %d15, .t", "jlez %d15, .t", "jltz %d15, .t",
    "jgez %d1, .t", "jgtz %d1, .t", "jlez %d1, .t", "jltz %d1, .t",
    "jz.a %a2, .t", "jnz.a %a2, .t", "jz.a %a15, .t", "jnz.a %a15, .t",
    "jeq.a %a2, %a3, .t", "jne.a %a2, %a3, .t",
    "jz.t %d15, 3, .t", "jnz.t %d15, 3, .t", "jz.t %d1, 3, .t", "jnz.t %d1, 3, .t",
    "jz.t %d15, 20, .t", "jnz.t %d1, 31, .t",
    "jned %d1, 3, .t", "jnei %d1, 3, .t", "jned %d1, %d2, .t", "jnei %d1, %d2, .t",
    "loop %a2, .t", "loop %a15, .t", "loopu .t",
]

# Bytes of .space between the branch and its label: inside disp4 (4), inside
# the extended disp4 of v1.6 but beyond the plain one (40), inside disp8 but
# beyond disp4 (200), beyond disp8 but inside disp15 (300), beyond disp15
# (40000).
DISTANCES = [4, 40, 200, 300, 40000]


def sources():
    for insn in INSNS:
        for n in DISTANCES:
            yield f"{insn}\n.space {n}\n.t:\nnop\n"
            yield f".t:\nnop\n.space {n}\n{insn}\n"


def text_bytes(path):
    """The .text section of a 32-bit little-endian ELF object."""
    with open(path, "rb") as f:
        elf = f.read()
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "not ELF32 LE"
    shoff = struct.unpack_from("<I", elf, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    def sh(i):
        return struct.unpack_from("<IIIIII", elf, shoff + i * shentsize)
    _, _, _, _, stroff, _ = sh(shstrndx)
    for i in range(shnum):
        name, _, _, _, off, size = sh(i)
        end = elf.index(b"\0", stroff + name)
        if elf[stroff + name:end] == b".text":
            return elf[off:off + size]
    raise RuntimeError("no .text section")


def tokens(data):
    """Hex runs, with every run of 8 or more zero bytes folded into z<n>."""
    def zero_run(at):
        k = at
        while k < len(data) and data[k] == 0:
            k += 1
        return k - at
    out, i = [], 0
    while i < len(data):
        run = zero_run(i)
        if run >= 8:
            out.append(f"z{run}")
            i += run
            continue
        j = i + 1
        while j < len(data) and zero_run(j) < 8:
            j += 1
        out.append(data[i:j].hex())
        i = j
    text = " ".join(out)
    assert expand(text) == data, "token round trip"
    return text


def expand(text):
    """The inverse of tokens(), as the tests implement it."""
    out = bytearray()
    for tok in text.split():
        if tok[0] == "z":
            out += bytes(int(tok[1:]))
        else:
            out += bytes.fromhex(tok)
    return bytes(out)


def assemble(src, workdir):
    obj = os.path.join(workdir, "p.o")
    r = subprocess.run([AS, "-mtc162", "--no-reloc", "-o", obj, "-"],
                       input=src.encode(), capture_output=True)
    if r.returncode != 0:
        return None, r.stderr.decode().strip()
    return text_bytes(obj), None


def main():
    lines, skipped = [], []
    with tempfile.TemporaryDirectory() as wd:
        for src in sources():
            data, err = assemble(src, wd)
            if data is None:
                skipped.append((src, err))
                continue
            esc = src.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
            lines.append(f'{{"{esc}", "{tokens(data)}"}},')
    with open(OUT, "w") as f:
        f.write("// Generated by scripts/gen_branch_relax_corpus.py from\n")
        f.write(f"// `{os.path.basename(AS)} -mtc162 --no-reloc`; do not edit.\n")
        f.write("// {source, expected}: expected is hex bytes or z<n> (n zero bytes).\n")
        if skipped:
            f.write("// Rejected by gas (not in the corpus):\n")
            for insn in sorted({[l for l in src.splitlines() if l and l[0] != "."][0] for src, _ in skipped}):
                f.write(f"//   {insn}\n")
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} cases written to {os.path.relpath(OUT)}, {len(skipped)} rejected by gas",
          file=sys.stderr)
    for src, err in skipped:
        print(f"  rejected: {src!r}: {err}", file=sys.stderr)


if __name__ == "__main__":
    main()
