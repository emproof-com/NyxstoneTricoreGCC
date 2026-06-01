#!/usr/bin/env python3
"""Python smoke test, mirrors examples/smoke.cpp and rust/examples/smoke.rs."""
from nyxstone_tricore_gcc import LabelDefinition, NyxstoneTricoreGCC


def main() -> None:
    nx = NyxstoneTricoreGCC()
    base = 0x80000000

    blob = b""
    for src in [
        "nop",
        "ret",
        "mov %d4, %d5",
        "add %d4, %d5, %d6",
        "movh %d4, 0x1234",
        "start:\n nop\n j here\nhere:\n ret\n",
        ".byte 0x11, 0x22, 0x33, 0x44",
        ".word 0xdeadbeef",
    ]:
        b = nx.assemble(src, address=base + len(blob))
        print(f"--- {src}\n    [{len(b)} bytes]: {b.hex(' ')}")
        blob += b

    print("\n--- external label (j ext, ext = base + 8) ---")
    ext = nx.assemble("nop\n nop\n nop\n j ext\n",
                      address=base,
                      labels=[LabelDefinition("ext", base + 8)])
    print(f"    [{len(ext)} bytes]: {ext.hex(' ')}")

    print("\n--- assemble_with_relocs (gcc/gas -r equivalent) ---")
    rel_bytes, relocs = nx.assemble_with_relocs(
        "nop\n j ext\n",
        address=0x1000,
        labels=[LabelDefinition("ext", 0x2000)])
    print(f"    bytes ({len(rel_bytes)}): {rel_bytes.hex(' ')}")
    for r in relocs:
        print(f"    reloc: off=0x{r.offset:x} type={r.relocation_type} "
              f"sym={r.symbol.name}(@0x{r.symbol.address:x}) addend={r.addend}")

    print("\n--- disassemble_to_instructions ---")
    for ins in nx.disassemble_to_instructions(blob, address=base):
        print(f"    0x{ins.address:08x}  [{ins.bytes.hex(' ')}]  {ins.assembly}")

    print("\n--- disassemble (text, first 3) ---")
    print(nx.disassemble(blob, address=base, count=3), end="")


if __name__ == "__main__":
    main()
