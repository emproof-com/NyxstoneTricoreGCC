#!/usr/bin/env python3
"""Python throughput benchmark, mirrors examples/bench.cpp."""
from __future__ import annotations

import sys
import time

from nyxstone_tricore_gcc import NyxstoneTricoreGCC


INSNS = [
    "mov %d4, %d5", "add %d4, %d5", "sub %d4, %d5", "nop", "ret",
    "mov.aa %a4, %a5", "ld.w %d4, [%a4]", "st.w [%a4], %d4",
    "and %d4, %d5", "or %d4, %d5",
]


def make_pkg(n: int) -> str:
    return "; ".join(INSNS[i % len(INSNS)] for i in range(n))


def run_for(secs: float, fn) -> float:
    for _ in range(3):
        fn()
    t0 = time.perf_counter()
    count = 0
    while True:
        for _ in range(10):
            fn()
            count += 1
        e = time.perf_counter() - t0
        if e >= secs:
            return count / e


def fmt(v: float) -> str:
    if v >= 1e6: return f"{v/1e6:>8.2f} M"
    if v >= 1e3: return f"{v/1e3:>8.2f} k"
    return f"{v:>10.2f}"


def main() -> None:
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
    nx = NyxstoneTricoreGCC()
    print(f"NyxstoneTricoreGCC (Python) bench, target {secs}s/measurement")
    for pkg in (1, 10):
        text = make_pkg(pkg)
        b = nx.assemble(text)
        r_asm = run_for(secs, lambda: nx.assemble(text))
        r_dis = run_for(secs, lambda: nx.disassemble_to_instructions(b))
        print(f"  Package {pkg} ({pkg} insns, {len(b)} bytes)")
        print(f"    assemble    : {fmt(r_asm)} ops/s   {fmt(r_asm*pkg)} insns/s")
        print(f"    disassemble : {fmt(r_dis)} ops/s   {fmt(r_dis*pkg)} insns/s")


if __name__ == "__main__":
    main()
