# nyxstone-tricore-gcc (Python)

Python bindings to the [NyxstoneTricoreGCC](..) in-process TriCore
assembler/disassembler.

Uses CFFI to compile a small C extension directly from the C++/C wrapper
sources, linking in the bundled binutils-tricore prebuilt archives and gas
object files.  The build is self-contained -- no prior `make` step and no
runtime shared library needed.

The API mirrors that of the sibling project
[Nyxstone](https://github.com/emproof-com/nyxstone) (LLVM-MC based, covers
LLVM-supported architectures; this package uses GNU binutils for TriCore).
Six methods: `assemble`, `assemble_to_instructions`, `disassemble`, and
`disassemble_to_instructions` mirror Nyxstone, plus `assemble_with_relocs`
and `assemble_to_instructions_with_relocs` for `gas -r`-style relocation
output.  Each takes an absolute `address` (and for the assembly entry
points, an iterable of `LabelDefinition`).

## Install

```sh
pip install ./    # from the python/ directory
```

Or develop in-place:

```sh
python setup.py build_ext --inplace
PYTHONPATH=. python examples/smoke.py
```

## Usage

```python
from nyxstone_tricore_gcc import LabelDefinition, NyxstoneTricoreGCC

nx = NyxstoneTricoreGCC()

# Assemble.  Local branches get their displacement encoded directly.
bytes_ = nx.assemble("start:\n nop\n j here\nhere:\n ret\n", address=0)
assert bytes_ == b"\x00\x00\x3c\x01\x00\x90"

# Assemble with an external label.
bytes2 = nx.assemble(
    "nop\n j ext\n",
    address=0x1000,
    labels=[LabelDefinition("ext", 0x2000)],
)

# Disassemble to instructions.
for ins in nx.disassemble_to_instructions(bytes_, address=0x80000000):
    print(f"0x{ins.address:08x}  {ins.assembly}")

# Or as a single string.
print(nx.disassemble(bytes_, address=0x80000000))

# Limit the number of instructions decoded.
first_two = nx.disassemble_to_instructions(bytes_, address=0, count=2)
```

## Errors

```python
from nyxstone_tricore_gcc import NyxstoneTricoreGCC, AssembleError, SectionViolationError

nx = NyxstoneTricoreGCC()

try:
    nx.assemble("not_a_real_mnemonic %d4")
except AssembleError as e:
    print("encoder failed:", e)

try:
    nx.assemble(".data\n .byte 0x42\n")
except (SectionViolationError, AssembleError) as e:
    # The library is .text-only; data/bss/section .foo are rejected.
    print("non-.text section:", e)
```

## Threading

Methods are safe to call from multiple threads, a process-wide
`threading.Lock` serializes them.
