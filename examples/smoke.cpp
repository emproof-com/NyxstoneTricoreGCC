// Smoke test / example use of NyxstoneTricoreGCC.
//
// Demonstrates the public C++ API: assemble(), assemble_to_instructions(),
// disassemble(), and disassemble_to_instructions().  See tests/tests.cpp
// for the full 118-test matrix.

#include "nyxstone/nyxstone.h"

#include <cstdio>
#include <iostream>

int main() {
    auto created = nyxstone::NyxstoneTricoreGCC::create();
    if (!created) {
        std::cerr << "NyxstoneTricoreGCC::create failed: " << created.error() << "\n";
        return 1;
    }
    auto& a = **created;

    std::vector<uint8_t> all;
    const uint64_t base = 0x80000000;

    for (const char* src : {
        "nop",
        "ret",
        "mov %d4, %d5",
        "add %d4, %d5, %d6",
        "movh %d4, 0x1234",
        "start:\n nop\n j here\nhere:\n ret\n",
        ".byte 0x11, 0x22, 0x33, 0x44",
        ".word 0xdeadbeef",
    }) {
        auto b = a.assemble(src, base + all.size(), /*labels=*/{});
        std::printf("--- %s\n", src);
        if (!b) { std::printf("    FAIL: %s\n", b.error().c_str()); continue; }
        std::printf("    [%zu bytes]:", b->size());
        for (auto x : *b) std::printf(" %02x", x);
        std::printf("\n");
        all.insert(all.end(), b->begin(), b->end());
    }

    std::printf("\n--- assemble_to_instructions example ---\n");
    auto ins_asm = a.assemble_to_instructions("nop\n ret\n", base, {});
    if (ins_asm) {
        for (auto& i : *ins_asm)
            std::printf("    0x%08lx  %s\n", (unsigned long) i.address, i.assembly.c_str());
    }

    std::printf("\n--- external label (j ext, ext=base+8) ---\n");
    auto ext_b = a.assemble("nop\n nop\n nop\n j ext\n", base, {{"ext", base + 8}});
    if (ext_b) {
        std::printf("    [%zu bytes]:", ext_b->size());
        for (auto x : *ext_b) std::printf(" %02x", x);
        std::printf("\n");
    } else {
        std::printf("    FAIL: %s\n", ext_b.error().c_str());
    }

    std::printf("\n--- assemble_with_relocs (gcc/gas -r equivalent) ---\n");
    auto rel = a.assemble_with_relocs(
        "nop\n j ext\n",
        /*address=*/0x1000,
        /*labels=*/{{"ext", 0x2000}});
    if (rel) {
        std::printf("    bytes (%zu):", rel->bytes.size());
        for (auto x : rel->bytes) std::printf(" %02x", x);
        std::printf("\n");
        for (const auto& r : rel->relocations) {
            std::printf("    reloc: off=0x%lx type=%u sym=%s(@0x%lx) addend=%lld\n",
                        (unsigned long) r.offset, r.relocation_type,
                        r.symbol.name.c_str(), (unsigned long) r.symbol.address,
                        (long long) (r.addend ? *r.addend : 0));
        }
    } else {
        std::printf("    FAIL: %s\n", rel.error().c_str());
    }

    std::printf("\n--- disassemble_to_instructions of the concatenated bytes ---\n");
    auto ins = a.disassemble_to_instructions(all, base, /*count=*/0);
    if (!ins) {
        std::cerr << "disassemble failed: " << ins.error() << "\n";
        return 1;
    }
    for (auto& i : *ins) {
        std::printf("    0x%08lx  [", (unsigned long) i.address);
        for (auto x : i.bytes) std::printf(" %02x", x);
        std::printf(" ]  %s\n", i.assembly.c_str());
    }

    std::printf("\n--- disassemble as text (first 3 insns) ---\n");
    auto text = a.disassemble(all, base, /*count=*/3);
    if (text) std::printf("%s", text->c_str());
    return 0;
}
