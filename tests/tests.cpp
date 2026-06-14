// NyxstoneTricoreGCC test suite.
//
// 155 matrix tests across eleven groups:
//   - insn       (47): every TriCore format we exercise.
//   - label      (12): forward/backward branches, multi-label lines,
//                       various identifier styles, label-only sources.
//   - relax       (2): short-form selection for local branches.
//   - data       (36): every data directive Nyxstone supports.
//   - mixed       (4): instructions + labels + data interleaved.
//   - edge        (9): empty / comments-only / whitespace / `;` separators.
//   - forbid   (7+3): Nyxstone-style .text-only restriction (+ accepted).
//   - quote       (5): `#`/`;`/`//` inside string literals.
//   - dirsem     (13): fill operands, .equ/.set, numeric local labels,
//                       .p2align, alignment fill/max-skip.
//   - alignrelax  (4): .align/.org padding sized after branch relaxation.
//   - error      (13): undefined labels, bad directives, range errors --
//                       must reject, never emit silently wrong bytes.
//
// Expected bytes are embedded (no runtime gas dependency).  They were
// captured from a known-good run cross-validated against tricore-elf-as
// (the EEESlab/tricore-binutils-gdb fork's stock GNU assembler).
//
// Two outcome modes per test:
//   BYTES     : assemble() must return the embedded bytes.
//   MUST_FAIL : assemble() must return nullopt.
//
// After Round 1 byte-correctness, every passing test is repeated 100× to
// catch state-reset drift across consecutive assemble() calls.  Post-matrix
// checks cover disassembly round-trips, external label resolution (exact
// displacement bytes), relocations, branch displacement semantics, data
// symbol references, error-message quality, and 32-bit address masking.

#include "nyxstone/nyxstone.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

enum Mode { BYTES, MUST_FAIL };

struct Test {
    const char* group;
    const char* name;
    const char* src;
    Mode        mode;
    std::vector<uint8_t> expected;
};

std::string hex(const std::vector<uint8_t>& v) {
    std::string s; char b[8];
    for (auto x : v) { std::snprintf(b, sizeof(b), "%02x ", x); s += b; }
    if (!s.empty()) s.pop_back();
    return s;
}

// Extract the absolute branch target from a disassembled instruction such as
// "j 0x1064" or "jeq %d15,0,0x1008": the last 0x... token.  Returns -1 if the
// text has no hex literal (i.e. not an address-bearing instruction).
long branch_target(const std::string& asm_text) {
    auto p = asm_text.rfind("0x");
    if (p == std::string::npos) return -1;
    return static_cast<long>(std::strtoull(asm_text.c_str() + p + 2, nullptr, 16));
}

inline std::vector<uint8_t> B(std::initializer_list<int> il) {
    std::vector<uint8_t> v; v.reserve(il.size());
    for (int x : il) v.push_back(static_cast<uint8_t>(x));
    return v;
}

const std::vector<Test> TESTS = {
    // -------------------------------------------------------------------------
    // insn, every TriCore instruction format we cover (46 tests).
    // -------------------------------------------------------------------------
    {"insn", "SR  nop",                "nop",                          BYTES, B({0x00, 0x00})},
    {"insn", "SR  ret",                "ret",                          BYTES, B({0x00, 0x90})},
    {"insn", "SR  debug",              "debug",                        BYTES, B({0x00, 0xa0})},
    {"insn", "SR  rfe",                "rfe",                          BYTES, B({0x00, 0x80})},
    {"insn", "SR  rfm",                "rfm",                          BYTES, B({0x0d, 0x00, 0x40, 0x01})},
    {"insn", "SRR mov d,d",            "mov %d4, %d5",                 BYTES, B({0x02, 0x54})},
    {"insn", "SRR add d,d",            "add %d4, %d5",                 BYTES, B({0x42, 0x54})},
    {"insn", "SRR sub d,d",            "sub %d4, %d5",                 BYTES, B({0xa2, 0x54})},
    {"insn", "SRR and d,d",            "and %d4, %d5",                 BYTES, B({0x26, 0x54})},
    {"insn", "SRR or d,d",             "or %d4, %d5",                  BYTES, B({0xa6, 0x54})},
    {"insn", "SRR xor d,d",            "xor %d4, %d5",                 BYTES, B({0xc6, 0x54})},
    {"insn", "SRR addsh",              "add %d15, %d5",                BYTES, B({0x42, 0x5f})},
    {"insn", "SRR mov.aa a,a",         "mov.aa %a4, %a5",              BYTES, B({0x40, 0x54})},
    {"insn", "SRR mov.aa a0,a1",       "mov.aa %a0, %a1",              BYTES, B({0x40, 0x10})},
    {"insn", "SRR mov.aa a15,a10",     "mov.aa %a15, %a10",            BYTES, B({0x40, 0xaf})},
    {"insn", "SLR ld.w",               "ld.w %d4, [%a4]",              BYTES, B({0x54, 0x44})},
    {"insn", "SLR ld.bu",              "ld.bu %d4, [%a4]",             BYTES, B({0x14, 0x44})},
    {"insn", "SLR ld.h",               "ld.h %d4, [%a4]",              BYTES, B({0x94, 0x44})},
    {"insn", "SLR ld.a",               "ld.a %a4, [%a4]",              BYTES, B({0xd4, 0x44})},
    {"insn", "SSR st.w",               "st.w [%a4], %d4",              BYTES, B({0x74, 0x44})},
    {"insn", "SSR st.b",               "st.b [%a4], %d4",              BYTES, B({0x34, 0x44})},
    {"insn", "SSR st.h",               "st.h [%a4], %d4",              BYTES, B({0xb4, 0x44})},
    {"insn", "SSR st.a",               "st.a [%a4], %a4",              BYTES, B({0xf4, 0x44})},
    {"insn", "SC  mov d15,imm8",       "mov %d15, 100",                BYTES, B({0xda, 0x64})},
    {"insn", "SC  mov d15,0",          "mov %d15, 0",                  BYTES, B({0x82, 0x0f})},
    {"insn", "SC  mov d15,255",        "mov %d15, 255",                BYTES, B({0xda, 0xff})},
    {"insn", "SRC mov d,imm4",         "mov %d4, 5",                   BYTES, B({0x82, 0x54})},
    {"insn", "SRC mov d,-1",           "mov %d4, -1",                  BYTES, B({0x82, 0xf4})},
    {"insn", "SRC add d,imm4",         "add %d4, 3",                   BYTES, B({0xc2, 0x34})},
    {"insn", "RC  add d,d,imm9",       "add %d4, %d5, 100",            BYTES, B({0x8b, 0x45, 0x06, 0x40})},
    {"insn", "RC  and d,d,imm9",       "and %d4, %d5, 0xff",           BYTES, B({0x8f, 0xf5, 0x0f, 0x41})},
    {"insn", "RC  or d,d,imm9",        "or  %d4, %d5, 0xff",           BYTES, B({0x8f, 0xf5, 0x4f, 0x41})},
    {"insn", "RC  add neg imm",        "add %d4, %d5, -16",            BYTES, B({0x8b, 0x05, 0x1f, 0x40})},
    {"insn", "RR  add 3-op d",         "add %d4, %d5, %d6",            BYTES, B({0x0b, 0x65, 0x00, 0x40})},
    {"insn", "RR  sub 3-op d",         "sub %d4, %d5, %d6",            BYTES, B({0x0b, 0x65, 0x80, 0x40})},
    {"insn", "RR  add.a 3-op a",       "add.a %a4, %a5, %a6",          BYTES, B({0x01, 0x65, 0x10, 0x40})},
    {"insn", "RR  sub.a 3-op a",       "sub.a %a4, %a5, %a6",          BYTES, B({0x01, 0x65, 0x20, 0x40})},
    {"insn", "RR  and 3-op d",         "and %d4, %d5, %d6",            BYTES, B({0x0f, 0x65, 0x80, 0x40})},
    {"insn", "RR  or 3-op d",          "or  %d4, %d5, %d6",            BYTES, B({0x0f, 0x65, 0xa0, 0x40})},
    {"insn", "RR  xor 3-op d",         "xor %d4, %d5, %d6",            BYTES, B({0x0f, 0x65, 0xc0, 0x40})},
    {"insn", "RLC mov d,imm16",        "mov %d4, 1000",                BYTES, B({0x3b, 0x80, 0x3e, 0x40})},
    {"insn", "RLC movh d,imm16",       "movh %d4, 0x1234",             BYTES, B({0x7b, 0x40, 0x23, 0x41})},
    {"insn", "RLC mov d,neg",          "mov %d4, -1000",               BYTES, B({0x3b, 0x80, 0xc1, 0x4f})},
    {"insn", "RLC movh d,max",         "movh %d4, 0xffff",             BYTES, B({0x7b, 0xf0, 0xff, 0x4f})},
    {"insn", "B   j abs",              "j 0x10",                       BYTES, B({0x3c, 0x08})},
    {"insn", "B   jl abs",             "jl 0x10",                      BYTES, B({0x5d, 0x00, 0x08, 0x00})},
    {"insn", "B   call abs",           "call 0x10",                    BYTES, B({0x5c, 0x08})},

    // -------------------------------------------------------------------------
    // label, forward / backward / multi / naming (12 tests).
    // -------------------------------------------------------------------------
    // Local (same-section) branches relax to their 2-byte short form (opcode
    // 0x3c) and carry the correct PC-relative displacement: `j here` targets
    // the next insn (disp 0x01), `j loop` jumps back one insn (disp 0xff = -1).
    {"label", "fwd j",                 "start: nop; j here; here: ret\n",                   BYTES, B({0x00, 0x00, 0x3c, 0x01, 0x00, 0x90})},
    {"label", "fwd j with addrs",      ".L0:\n nop\n j .L1\n .L1:\n nop\n ret\n",           BYTES, B({0x00, 0x00, 0x3c, 0x01, 0x00, 0x00, 0x00, 0x90})},
    {"label", "back j short",          "loop: nop; j loop\n",                               BYTES, B({0x00, 0x00, 0x3c, 0xff})},
    // Relaxation holds across a real distance and on either side of the label:
    // a same-section `j` is the 2-byte short form (opcode 0x3c) whether the
    // target sits before or after the branch, with surrounding insns intact.
    {"relax", "back j over distance",  "a:\n nop\n nop\n j a\n",                             BYTES, B({0x00, 0x00, 0x00, 0x00, 0x3c, 0xfe})},
    {"relax", "fwd j over distance",   "j t\n nop\n nop\nt:\n ret\n",                        BYTES, B({0x3c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90})},
    {"label", "back jeq",              "loop: nop; nop; jeq %d15, 0, loop\n",               BYTES, B({0x00, 0x00, 0x00, 0x00, 0xdf, 0x0f, 0xfe, 0x7f})},
    {"label", "lone label then insn",  "foo:\n bar:\n nop\n",                               BYTES, B({0x00, 0x00})},
    {"label", "two labels same line",  "a: b: nop\n",                                       BYTES, B({0x00, 0x00})},
    {"label", "dot-prefix .L0",        ".L0: nop\n",                                        BYTES, B({0x00, 0x00})},
    {"label", "dollar prefix",         "$x: nop\n",                                         BYTES, B({0x00, 0x00})},
    {"label", "underscore",            "_x: nop\n",                                         BYTES, B({0x00, 0x00})},
    {"label", "alnum mix",             "foo_bar_42: nop\n",                                 BYTES, B({0x00, 0x00})},
    {"label", "label only no insn",    "only_label:\n",                                     BYTES, {}},
    {"label", "label-only mixed",      "a: # comment\n",                                    BYTES, {}},

    // -------------------------------------------------------------------------
    // data, every data directive (40 tests).
    // -------------------------------------------------------------------------
    {"data", ".byte single",           ".byte 0x42",                          BYTES, B({0x42})},
    {"data", ".byte list",             ".byte 0x11, 0x22, 0x33, 0x44",        BYTES, B({0x11, 0x22, 0x33, 0x44})},
    {"data", ".byte dec",              ".byte 1, 2, 3, 4, 5",                 BYTES, B({1, 2, 3, 4, 5})},
    {"data", ".byte zero",             ".byte 0",                             BYTES, B({0})},
    {"data", ".byte negative",         ".byte -1",                            BYTES, B({0xff})},
    {"data", ".half",                  ".half 0x1234",                        BYTES, B({0x34, 0x12})},
    {"data", ".short",                 ".short 0x1234",                       BYTES, B({0x34, 0x12})},
    {"data", ".2byte",                 ".2byte 0x1234",                       BYTES, B({0x34, 0x12})},
    {"data", ".half list",             ".half 0x1111, 0x2222, 0x3333",        BYTES, B({0x11, 0x11, 0x22, 0x22, 0x33, 0x33})},
    {"data", ".word LE",               ".word 0xdeadbeef",                    BYTES, B({0xef, 0xbe, 0xad, 0xde})},
    {"data", ".int LE",                ".int 0xcafebabe",                     BYTES, B({0xbe, 0xba, 0xfe, 0xca})},
    {"data", ".long LE",               ".long 0x01020304",                    BYTES, B({0x04, 0x03, 0x02, 0x01})},
    {"data", ".4byte LE",              ".4byte 0xff00ff00",                   BYTES, B({0x00, 0xff, 0x00, 0xff})},
    {"data", ".word list",             ".word 0xaa, 0xbb",                    BYTES, B({0xaa, 0, 0, 0, 0xbb, 0, 0, 0})},
    {"data", ".quad LE",               ".quad 0x1122334455667788",            BYTES, B({0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11})},
    {"data", ".8byte LE",              ".8byte 0xabcd",                       BYTES, B({0xcd, 0xab, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})},
    {"data", ".ascii hi",              ".ascii \"hi\"",                       BYTES, B({0x68, 0x69})},
    {"data", ".ascii empty",           ".ascii \"\"",                         BYTES, {}},
    {"data", ".ascii escape \\n",      ".ascii \"a\\nb\"",                    BYTES, B({0x61, 0x0a, 0x62})},
    {"data", ".ascii escape \\t",      ".ascii \"a\\tb\"",                    BYTES, B({0x61, 0x09, 0x62})},
    {"data", ".ascii escape \\\\",     ".ascii \"a\\\\b\"",                   BYTES, B({0x61, 0x5c, 0x62})},
    {"data", ".asciz null-term",       ".asciz \"hi\"",                       BYTES, B({0x68, 0x69, 0x00})},
    {"data", ".string null-term",      ".string \"abc\"",                     BYTES, B({0x61, 0x62, 0x63, 0x00})},
    {"data", ".skip 4",                ".skip 4",                             BYTES, B({0, 0, 0, 0})},
    {"data", ".space 3",               ".space 3",                            BYTES, B({0, 0, 0})},
    {"data", ".zero 2",                ".zero 2",                             BYTES, B({0, 0})},
    {"data", ".skip 0",                ".skip 0",                             BYTES, {}},
    {"data", ".org 0",                 ".org 0",                              BYTES, {}},
    {"data", ".org 4",                 ".org 4",                              BYTES, B({0, 0, 0, 0})},
    {"data", ".org 8 after .byte",     ".byte 0xaa; .org 8",                  BYTES, B({0xaa, 0, 0, 0, 0, 0, 0, 0})},
    {"data", ".align 0",               ".byte 0x11; .align 0",                BYTES, B({0x11})},
    {"data", ".align 1",               ".byte 0x11; .align 1",                BYTES, B({0x11, 0})},
    {"data", ".align 2",               ".byte 0x11; .align 2",                BYTES, B({0x11, 0, 0, 0})},
    {"data", ".align 3 from 1",        ".byte 0x11; .align 3",                BYTES, B({0x11, 0, 0, 0, 0, 0, 0, 0})},
    {"data", ".balign 4 from 1",       ".byte 0x11; .balign 4",               BYTES, B({0x11, 0, 0, 0})},
    {"data", ".balign 4 from 4",       ".byte 1, 2, 3, 4; .balign 4",         BYTES, B({1, 2, 3, 4})},

    // -------------------------------------------------------------------------
    // mixed, labels + insns + data interleaved (4 tests).
    // -------------------------------------------------------------------------
    {"mixed", "data then insn",        ".byte 0x42; nop",                     BYTES, B({0x42, 0x00, 0x00})},
    {"mixed", "insn then data",        "nop; .byte 0x42",                     BYTES, B({0x00, 0x00, 0x42})},
    {"mixed", "label-data-insn-label", "lbl1:\n .byte 0x42\n nop\n lbl2: ret\n", BYTES, B({0x42, 0x00, 0x00, 0x00, 0x90})},
    {"mixed", "block",
        "start:\n  nop\n  nop\n  .word 0xdeadbeef\n  ret\nend:\n",
        BYTES, B({0x00, 0x00, 0x00, 0x00, 0xef, 0xbe, 0xad, 0xde, 0x00, 0x90})},

    // -------------------------------------------------------------------------
    // edge, empty / comments / whitespace / separators (9 tests).
    // -------------------------------------------------------------------------
    {"edge", "empty",                  "",                                    BYTES, {}},
    {"edge", "only newlines",          "\n\n\n",                              BYTES, {}},
    {"edge", "only whitespace",        "   \t  ",                             BYTES, {}},
    {"edge", "only line comment",      "# this is a comment\n",               BYTES, {}},
    {"edge", "only c++ comment",       "// also a comment\n",                 BYTES, {}},
    {"edge", "leading newlines",       "\n\n\nnop\n",                         BYTES, B({0x00, 0x00})},
    {"edge", "trailing newlines",      "nop\n\n\n",                           BYTES, B({0x00, 0x00})},
    {"edge", "; separator",            "nop; ret",                            BYTES, B({0x00, 0x00, 0x00, 0x90})},
    {"edge", ";; double",              "nop;; ret",                           BYTES, B({0x00, 0x00, 0x00, 0x90})},

    // -------------------------------------------------------------------------
    // forbid, .text-only restriction (Nyxstone-style).  Each MUST_FAIL
    // proves we reject the directive cleanly without producing partial bytes.
    // -------------------------------------------------------------------------
    {"forbid", ".data switch",         ".data\n .byte 0x42\n",                MUST_FAIL, {}},
    {"forbid", ".bss switch",          ".bss\n",                              MUST_FAIL, {}},
    {"forbid", ".rodata switch",       ".rodata\n",                           MUST_FAIL, {}},
    {"forbid", ".section .data",       ".section .data\n",                    MUST_FAIL, {}},
    {"forbid", ".section .rodata",     ".section .rodata\n",                  MUST_FAIL, {}},
    {"forbid", ".section .foo",        ".section .foo\n",                     MUST_FAIL, {}},
    {"forbid", ".pushsection",         ".pushsection .data\n",                MUST_FAIL, {}},
    {"forbid-ok", ".text",             ".text\n nop\n",                       BYTES, B({0x00, 0x00})},
    {"forbid-ok", ".section .text",    ".section .text\n nop\n",              BYTES, B({0x00, 0x00})},
    {"forbid-ok", ".section .text.foo",".section .text.foo\n nop\n",          BYTES, B({0x00, 0x00})},

    // -------------------------------------------------------------------------
    // quote, the statement splitter and comment stripper must not fire
    // inside "..." string literals.
    // -------------------------------------------------------------------------
    {"quote", "# inside string",       ".asciz \"a#b\"\n",                    BYTES, B({0x61, 0x23, 0x62, 0x00})},
    {"quote", "; inside string",       ".ascii \"a;b\"\n",                    BYTES, B({0x61, 0x3b, 0x62})},
    {"quote", "// inside string",      ".ascii \"a//b\"\n",                   BYTES, B({0x61, 0x2f, 0x2f, 0x62})},
    {"quote", "escaped quote + #",     ".ascii \"a\\\"#\"\n",                 BYTES, B({0x61, 0x22, 0x23})},
    {"quote", "comment after string",  ".ascii \"ab\"  # trailing\n",         BYTES, B({0x61, 0x62})},

    // -------------------------------------------------------------------------
    // dirsem, directive semantics: fill operands, .equ/.set constants,
    // numeric local labels, .p2align, alignment fill / max-skip.
    // -------------------------------------------------------------------------
    {"dirsem", ".skip with fill",      ".skip 3, 0xff\n",                     BYTES, B({0xff, 0xff, 0xff})},
    {"dirsem", ".space with fill",     ".space 2, 0x41\n",                    BYTES, B({0x41, 0x41})},
    {"dirsem", ".zero",                ".zero 2\n",                           BYTES, B({0x00, 0x00})},
    {"dirsem", ".equ constant",        ".equ five, 5\n mov %d0, five\n",      BYTES, B({0x82, 0x50})},
    {"dirsem", ".set constant",        ".set six, 6\n mov %d0, six\n",        BYTES, B({0x82, 0x60})},
    {"dirsem", ".equ forward label",   ".equ tgt, after\n j tgt\nafter: nop\n", BYTES, B({0x1d, 0x00, 0x02, 0x00, 0x00, 0x00})},
    {"dirsem", "numeric label back",   "1: nop\n j 1b\n",                     BYTES, B({0x00, 0x00, 0x3c, 0xff})},
    {"dirsem", "numeric label fwd",    "j 1f\n nop\n1: ret\n",                BYTES, B({0x3c, 0x02, 0x00, 0x00, 0x00, 0x90})},
    {"dirsem", "numeric label reuse",  "1: nop\n j 1b\n1: ret\n j 1b\n",      BYTES, B({0x00, 0x00, 0x3c, 0xff, 0x00, 0x90, 0x3c, 0xff})},
    {"dirsem", ".p2align",             ".byte 1\n .p2align 2\n .byte 2\n",    BYTES, B({0x01, 0x00, 0x00, 0x00, 0x02})},
    {"dirsem", ".align fill",          ".byte 1\n .align 2, 0xaa\n .byte 2\n", BYTES, B({0x01, 0xaa, 0xaa, 0xaa, 0x02})},
    {"dirsem", ".align max-skip",      ".byte 1\n .align 2, 0, 1\n .byte 2\n", BYTES, B({0x01, 0x02})},
    {"dirsem", ".balign",              ".byte 1\n .balign 4\n .byte 2\n",     BYTES, B({0x01, 0x00, 0x00, 0x00, 0x02})},

    // -------------------------------------------------------------------------
    // alignrelax, .align/.org sized AFTER branch relaxation: a branch frag
    // before the directive used to make the padding silently wrong (computed
    // from parse-time offsets in the wrong frag).
    // -------------------------------------------------------------------------
    {"alignrelax", ".align after relaxed j", "start: nop\n j start\n .align 3\n ret\n",
        BYTES, B({0x00, 0x00, 0x3c, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90})},
    {"alignrelax", ".org after relaxed j",   "start: nop\n j start\n .org 8\n ret\n",
        BYTES, B({0x00, 0x00, 0x3c, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90})},
    {"alignrelax", "fwd j over .align",      "j t\n nop\n .align 3\nt: ret\n",
        BYTES, B({0x3c, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90})},
    {"alignrelax", ".org exact (no pad)",    "nop\n nop\n .org 4\n ret\n",
        BYTES, B({0x00, 0x00, 0x00, 0x00, 0x00, 0x90})},

    // -------------------------------------------------------------------------
    // error, inputs that must be rejected instead of producing silently
    // wrong bytes (message content is asserted separately below).
    // -------------------------------------------------------------------------
    {"error", "undefined label",       "j nowhere_defined\n",                 MUST_FAIL, {}},
    {"error", "undefined data symbol", ".word nowhere_defined\n",             MUST_FAIL, {}},
    {"error", "unknown directive",     ".frobnicate 1, 2\n",                  MUST_FAIL, {}},
    {"error", "unknown mnemonic",      "frobnicate %d0\n",                    MUST_FAIL, {}},
    {"error", "duplicate label",       "x: nop\nx: ret\n",                    MUST_FAIL, {}},
    {"error", ".org backwards",        "nop\n nop\n .org 2\n ret\n",          MUST_FAIL, {}},
    {"error", ".balign non-pow2",      ".balign 3\n",                         MUST_FAIL, {}},
    {"error", ".align exponent range", ".align 31\n",                         MUST_FAIL, {}},
    {"error", ".skip negative",        ".skip -1\n",                          MUST_FAIL, {}},
    {"error", ".ascii non-string",     ".ascii 5\n",                          MUST_FAIL, {}},
    {"error", ".ascii unterminated",   ".ascii \"abc\n",                      MUST_FAIL, {}},
    {"error", ".equ missing operand",  ".equ five\n",                         MUST_FAIL, {}},
    {"error", "imm out of range",      "mov %d0, 0x123456\n",                 MUST_FAIL, {}},
};

struct Result { int passed = 0; int failed = 0; int drift = 0; };

void run(const Test& t, nyxstone::NyxstoneTricoreGCC& a, Result& r) {
    auto out = a.assemble(t.src, /*address=*/0, /*labels=*/{});
    if (t.mode == MUST_FAIL) {
        if (out) {
            std::printf("  FAIL  [%s] %-30s should-fail but got %s\n",
                        t.group, t.name, hex(*out).c_str());
            ++r.failed;
        } else {
            ++r.passed;
        }
        return;
    }
    if (!out) {
        std::printf("  FAIL  [%s] %-30s unexpected assemble() failure: %s\n",
                    t.group, t.name, out.error().c_str());
        ++r.failed;
        return;
    }
    if (*out == t.expected) { ++r.passed; return; }
    std::printf("  FAIL  [%s] %-30s\n"
                "              src: %s\n"
                "          got bytes: %s\n"
                "         want bytes: %s\n",
                t.group, t.name, t.src, hex(*out).c_str(), hex(t.expected).c_str());
    ++r.failed;
}

}  // namespace

int main() {
    auto created = nyxstone::NyxstoneTricoreGCC::create();
    if (!created) {
        std::fprintf(stderr, "NyxstoneTricoreGCC::create failed: %s\n", created.error().c_str());
        return 1;
    }
    auto& a = **created;

    Result r;
    const char* group = nullptr;
    for (auto& t : TESTS) {
        if (!group || std::string(group) != t.group) {
            group = t.group;
            std::printf("\n--- group: %s ---\n", group);
        }
        run(t, a, r);
    }

    // 100x stress, catches state-reset drift.
    std::printf("\n--- 100x stress (each non-MUST_FAIL test 100 iterations) ---\n");
    size_t stress_n = 0;
    for (auto& t : TESTS) {
        if (t.mode == MUST_FAIL) continue;
        ++stress_n;
        std::vector<uint8_t> last;
        bool ok = true;
        for (int k = 0; k < 100; ++k) {
            auto o = a.assemble(t.src, 0, {});
            if (!o) { ok = false; break; }
            if (k == 0) last = *o;
            else if (*o != last) { ok = false; break; }
        }
        if (!ok) {
            std::printf("  DRIFT [%s] %s\n", t.group, t.name);
            ++r.drift;
        }
    }
    if (r.drift == 0) std::printf("  PASS: no drift across %zu non-fail tests * 100 iterations\n",
                                   stress_n);

    // Round-trip: every BYTES test that produced ≥2 bytes must disassemble
    // cleanly (we just check we don't crash and get the right number of
    // bytes back, disassembled text shape varies by gas/libopcodes
    // version, so we don't compare strings).
    std::printf("\n--- disassembly round-trip ---\n");
    int rt_pass = 0, rt_fail = 0;
    for (auto& t : TESTS) {
        if (t.mode != BYTES || t.expected.size() < 2) continue;
        auto v = a.disassemble_to_instructions(t.expected, 0, 0);
        if (!v) {
            std::printf("  FAIL  [%s] %s, disassemble failed: %s\n",
                        t.group, t.name, v.error().c_str());
            ++rt_fail;
            continue;
        }
        size_t total = 0;
        for (auto& ins : *v) total += ins.bytes.size();
        if (total != t.expected.size()) {
            std::printf("  FAIL  [%s] %s, disasm consumed %zu / %zu bytes\n",
                        t.group, t.name, total, t.expected.size());
            ++rt_fail;
        } else {
            ++rt_pass;
        }
    }
    std::printf("  %d round-trip pass, %d fail\n", rt_pass, rt_fail);

    // External label support, `j ext` with ext supplied via LabelDefinition.
    std::printf("\n--- external label resolution ---\n");
    int el_pass = 0, el_fail = 0;
    {
        // ext is 8 bytes ahead of address 0x1000.  External labels stay in
        // the longest (value-independent) branch form: 6 nop + 4-byte j = 10.
        // The j sits at 0x1006, so disp = (0x1008 - 0x1006) / 2 = 1, encoded
        // in B-format as 1d 00 01 00.  Asserting the exact displacement
        // bytes is the point: a regression that silently encodes 0 must not
        // pass on size alone.
        const std::vector<uint8_t> expect =
            {0x00,0x00, 0x00,0x00, 0x00,0x00, 0x1d,0x00,0x01,0x00};
        auto o = a.assemble("nop\n nop\n nop\n j ext\n",
                            /*address=*/0x1000,
                            /*labels=*/{{"ext", 0x1008}});
        if (!o) {
            std::printf("  FAIL  external 0x1000 → 0x1008: %s\n", o.error().c_str());
            ++el_fail;
        } else if (*o != expect) {
            std::printf("  FAIL  external 0x1000 → 0x1008: got %s\n", hex(*o).c_str());
            ++el_fail;
        } else {
            ++el_pass;
        }
        // Address invariance: the same source with (address=0, ext=0x8) and
        // (address=0x1000, ext=0x1008) must produce identical bytes; the
        // displacement (label - PC) is the same in both cases and the
        // branch form does not depend on the absolute value.
        auto o2 = a.assemble("nop\n nop\n nop\n j ext\n", 0, {{"ext", 0x8}});
        if (o && o2 && *o == *o2) ++el_pass;
        else { std::printf("  FAIL  external-label address invariance\n"); ++el_fail; }
        // A label far away resolves with the full 24-bit displacement.
        auto o3 = a.assemble_to_instructions("j ext\n", 0x1000, {{"ext", 0x2000}});
        if (o3 && o3->size() == 1
            && o3->front().assembly.find("0x2000") != std::string::npos) ++el_pass;
        else { std::printf("  FAIL  external far target not encoded\n"); ++el_fail; }
    }
    std::printf("  %d external-label pass, %d fail\n", el_pass, el_fail);

    // assemble_to_instructions: address must propagate to Instruction.address.
    std::printf("\n--- assemble_to_instructions address ---\n");
    int ai_pass = 0, ai_fail = 0;
    {
        auto v = a.assemble_to_instructions("nop\n ret\n", /*address=*/0x4000, {});
        if (!v) {
            std::printf("  FAIL  assemble_to_instructions: %s\n", v.error().c_str());
            ++ai_fail;
        } else if (v->size() != 2 || (*v)[0].address != 0x4000 || (*v)[1].address != 0x4002) {
            std::printf("  FAIL  assemble_to_instructions: wrong address sequence\n");
            ++ai_fail;
        } else {
            ++ai_pass;
        }
    }
    std::printf("  %d assemble_to_instructions pass, %d fail\n", ai_pass, ai_fail);

    // disassemble: count parameter must limit instruction count.
    std::printf("\n--- disassemble count ---\n");
    int dc_pass = 0, dc_fail = 0;
    {
        std::vector<uint8_t> bytes = {0x00, 0x00, 0x00, 0x00, 0x00, 0x90};
        auto v = a.disassemble_to_instructions(bytes, /*address=*/0, /*count=*/2);
        if (!v) { std::printf("  FAIL  count=2: %s\n", v.error().c_str()); ++dc_fail; }
        else if (v->size() != 2) { std::printf("  FAIL  count=2 returned %zu\n", v->size()); ++dc_fail; }
        else ++dc_pass;
        auto v2 = a.disassemble_to_instructions(bytes, 0, 0);
        if (!v2 || v2->size() != 3) { std::printf("  FAIL  count=0 (all) failed\n"); ++dc_fail; } else ++dc_pass;
    }
    std::printf("  %d disassemble count pass, %d fail\n", dc_pass, dc_fail);

    // assemble_with_relocs: external label → reloc entry, bytes zero-placeholder.
    std::printf("\n--- assemble_with_relocs ---\n");
    int wr_pass = 0, wr_fail = 0;
    {
        auto r = a.assemble_with_relocs(
            "nop\n j ext\n",
            /*address=*/0x1000,
            /*labels=*/{{"ext", 0x2000}});
        if (!r) {
            std::printf("  FAIL: %s\n", r.error().c_str()); ++wr_fail;
        } else {
            // 2-byte nop + 4-byte long-form j to absolute (displacement = 0).
            const std::vector<uint8_t> expect_bytes =
                {0x00, 0x00, 0x1d, 0x00, 0x00, 0x00};
            if (r->bytes != expect_bytes) {
                std::printf("  FAIL  byte mismatch: got %s\n", hex(r->bytes).c_str());
                ++wr_fail;
            } else ++wr_pass;
            if (r->relocations.size() != 1) {
                std::printf("  FAIL  expected 1 reloc, got %zu\n", r->relocations.size());
                ++wr_fail;
            } else {
                const auto& rl = r->relocations[0];
                if (rl.offset != 0x2 || rl.symbol.name != "ext"
                    || rl.symbol.address != 0x2000
                    || rl.relocation_type != 3 /* R_TRICORE_24REL */) {
                    std::printf("  FAIL  reloc fields: off=%lx name=%s addr=%lx type=%u\n",
                                (unsigned long)rl.offset, rl.symbol.name.c_str(),
                                (unsigned long)rl.symbol.address, rl.relocation_type);
                    ++wr_fail;
                } else ++wr_pass;
                if (!rl.addend.has_value()) {
                    std::printf("  FAIL  reloc has no addend (TriCore is RELA)\n");
                    ++wr_fail;
                } else ++wr_pass;
            }
        }
        // Unlisted symbol → NOT an error (unlike the plain path, which
        // rejects undefined labels): the reference comes back as a reloc
        // entry with symbol.address == 0 for the linker to resolve.
        auto r2 = a.assemble_with_relocs("j foobar\n", 0, {});
        if (!r2 || r2->relocations.size() != 1
            || r2->relocations[0].symbol.name != "foobar"
            || r2->relocations[0].symbol.address != 0
            || r2->relocations[0].relocation_type != 3 /* R_TRICORE_24REL */) {
            std::printf("  FAIL  unlisted symbol case\n");
            ++wr_fail;
        } else ++wr_pass;
    }
    std::printf("  %d assemble_with_relocs pass, %d fail\n", wr_pass, wr_fail);

    // assemble_to_instructions_with_relocs: both instructions and relocs.
    std::printf("\n--- assemble_to_instructions_with_relocs ---\n");
    int air_pass = 0, air_fail = 0;
    {
        auto r = a.assemble_to_instructions_with_relocs(
            "nop\n j ext\n", 0x1000, {{"ext", 0x9000}});
        if (!r) {
            std::printf("  FAIL: %s\n", r.error().c_str()); ++air_fail;
        } else {
            if (r->instructions.size() != 2 || r->instructions[0].address != 0x1000
                || r->instructions[1].address != 0x1002) {
                std::printf("  FAIL  instructions\n"); ++air_fail;
            } else ++air_pass;
            if (r->relocations.size() != 1 || r->relocations[0].offset != 0x2
                || r->relocations[0].symbol.address != 0x9000) {
                std::printf("  FAIL  relocations\n"); ++air_fail;
            } else ++air_pass;
        }
    }
    std::printf("  %d assemble_to_instructions_with_relocs pass, %d fail\n", air_pass, air_fail);

    // Internal labels (defined in source) should NOT produce relocations:
    // they're resolvable, so apply_fix sets fx_done=1 and no entry is emitted.
    std::printf("\n--- with_relocs ignores internal labels ---\n");
    int wri_pass = 0, wri_fail = 0;
    {
        auto r = a.assemble_with_relocs(
            "start: nop\n j start\n", 0, {});
        if (!r) { std::printf("  FAIL: %s\n", r.error().c_str()); ++wri_fail; }
        else if (!r->relocations.empty()) {
            std::printf("  FAIL  expected 0 relocs (internal label), got %zu\n",
                        r->relocations.size());
            ++wri_fail;
        } else ++wri_pass;
    }
    std::printf("  %d internal-label pass, %d fail\n", wri_pass, wri_fail);

    // Branch displacement / relative references: a local branch must encode a
    // displacement that actually targets the referenced label.  We verify by
    // disassembling and checking the recovered target address equals the
    // address of the instruction the label denotes, across forward/backward
    // references, several distances, and several branch kinds.  The label is
    // always either the first instruction (backward refs) or the last
    // (forward refs), so its address is unambiguous from the decoded stream.
    std::printf("\n--- branch displacement (relative references) ---\n");
    int bd_pass = 0, bd_fail = 0;
    {
        const uint64_t base = 0x100000;  // away from 0 so a self-target stands out
        struct BD { const char* name; const char* src; bool fwd; };
        const BD bds[] = {
            {"j   fwd  +1 insn", "j l\n nop\nl: ret\n",                          true},
            {"j   fwd  +3 insn", "j l\n nop\n nop\n nop\nl: ret\n",              true},
            {"jl  fwd  +2 insn", "jl l\n nop\n nop\nl: ret\n",                   true},
            {"call fwd +2 insn", "call l\n nop\n nop\nl: ret\n",                 true},
            {"jeq fwd  +2 insn", "jeq %d15, 0, l\n nop\n nop\nl: ret\n",         true},
            {"jne fwd  +2 insn", "jne %d15, 0, l\n nop\n nop\nl: ret\n",         true},
            {"j   back -1 insn", "l: nop\n j l\n",                               false},
            {"j   back -3 insn", "l: nop\n nop\n nop\n j l\n",                   false},
            {"jeq back -2 insn", "l: nop\n nop\n jeq %d15, 0, l\n",              false},
            {"loop back",        "l: nop\n nop\n loop %a2, l\n",                 false},
        };
        for (const auto& bd : bds) {
            auto o = a.assemble(bd.src, base, {});
            if (!o) { std::printf("  FAIL  [%s] assemble: %s\n", bd.name, o.error().c_str()); ++bd_fail; continue; }
            auto v = a.disassemble_to_instructions(*o, base, 0);
            if (!v || v->empty()) { std::printf("  FAIL  [%s] disassemble failed\n", bd.name); ++bd_fail; continue; }
            const auto& branch = bd.fwd ? v->front() : v->back();
            uint64_t want   = bd.fwd ? v->back().address : v->front().address;
            long     got    = branch_target(branch.assembly);
            if (static_cast<long>(want) == got) {
                ++bd_pass;
            } else {
                std::printf("  FAIL  [%s] target 0x%lx, expected 0x%lx  (%s)\n",
                            bd.name, got, (unsigned long)want, branch.assembly.c_str());
                ++bd_fail;
            }
        }
    }
    std::printf("  %d branch-displacement pass, %d fail\n", bd_pass, bd_fail);

    // Data directives referencing symbols/expressions: must emit the right
    // bytes (a local label's absolute address, or a folded label difference)
    // instead of being silently dropped.
    std::printf("\n--- data symbol references ---\n");
    int ds_pass = 0, ds_fail = 0;
    {
        struct DS { const char* name; const char* src; uint64_t base;
                    size_t at; std::vector<uint8_t> want; };
        const DS cases[] = {
            {"word local",  "start: nop\n .word here\nhere: ret\n", 0x1000, 2, {0x06,0x10,0x00,0x00}},
            {"word diff",   "a: nop\n nop\nb: .word b - a\n",        0x1000, 4, {0x04,0x00,0x00,0x00}},
            {"short local", "l: nop\n .short l\n",                   0x1000, 2, {0x00,0x10}},
            {"byte local",  "l: nop\n .byte l\n",                    0x40,   2, {0x40}},
            {"word literal", ".word 0x11223344\n",                   0,      0, {0x44,0x33,0x22,0x11}},
        };
        for (const auto& c : cases) {
            auto o = a.assemble(c.src, c.base, {});
            if (!o) { std::printf("  FAIL  [%s] %s\n", c.name, o.error().c_str()); ++ds_fail; continue; }
            bool ok = o->size() >= c.at + c.want.size();
            for (size_t i = 0; ok && i < c.want.size(); ++i) ok = (*o)[c.at + i] == c.want[i];
            if (ok) ++ds_pass;
            else { std::printf("  FAIL  [%s] got %s\n", c.name, hex(*o).c_str()); ++ds_fail; }
        }
    }
    std::printf("  %d data-symbol-reference pass, %d fail\n", ds_pass, ds_fail);

    // Error-message quality: failures must carry the gas diagnostic / the
    // offending symbol name, not a generic string (and nothing may leak to
    // the host stderr -- gas output is captured into the error channel).
    std::printf("\n--- error message quality ---\n");
    int em_pass = 0, em_fail = 0;
    {
        struct EM { const char* name; const char* src; const char* expect_substr; };
        const EM cases[] = {
            {"undefined label names symbol", "j nowhere_defined\n",  "nowhere_defined"},
            {"gas diagnostic propagated",    "frobnicate %d0\n",     "Unknown instruction"},
            {"duplicate label diagnostic",   "x: nop\nx: ret\n",     "already defined"},
            {"unknown directive named",      ".frobnicate\n",        "unsupported directive '.frobnicate'"},
            {".org backwards diagnostic",    "nop\nnop\n.org 2\n",   ".org backwards"},
            {"section violation phrase",     ".data\n",              "only .text is allowed"},
        };
        for (const auto& c : cases) {
            auto o = a.assemble(c.src, 0, {});
            if (o) { std::printf("  FAIL  [%s] unexpectedly assembled\n", c.name); ++em_fail; continue; }
            if (o.error().find(c.expect_substr) == std::string::npos) {
                std::printf("  FAIL  [%s] error lacks \"%s\": %s\n",
                            c.name, c.expect_substr, o.error().c_str());
                ++em_fail;
            } else ++em_pass;
        }
        // Conflict between a source label and a LabelDefinition of the same
        // name must be reported, not silently resolved either way.
        auto o = a.assemble("dup: nop\n j dup\n", 0x1000, {{"dup", 0x2000}});
        if (!o && o.error().find("dup") != std::string::npos) ++em_pass;
        else { std::printf("  FAIL  source+LabelDefinition conflict not reported\n"); ++em_fail; }
    }
    std::printf("  %d error-message pass, %d fail\n", em_pass, em_fail);

    // Disassembly address masking: TriCore is 32-bit; branch targets print
    // masked to 32 bits like objdump (0xfffffffe, never a sign-extended
    // 64-bit value).
    std::printf("\n--- disassembly address masking ---\n");
    int am_pass = 0, am_fail = 0;
    {
        auto v = a.disassemble_to_instructions({0xff, 0xff, 0xff, 0xff}, 0, 0);
        if (v && v->size() == 1
            && v->front().assembly.find("0xfffffffe") != std::string::npos
            && v->front().assembly.find("0xffffffffffff") == std::string::npos) ++am_pass;
        else {
            std::printf("  FAIL  expected masked 0xfffffffe target, got: %s\n",
                        v ? v->front().assembly.c_str() : v.error().c_str());
            ++am_fail;
        }
    }
    std::printf("  %d address-masking pass, %d fail\n", am_pass, am_fail);

    int extra_fail = el_fail + ai_fail + dc_fail + wr_fail + air_fail + wri_fail + bd_fail + ds_fail
                   + em_fail + am_fail;
    std::printf("\nSummary: %d passed, %d failed, %d drifts (of %zu tests); "
                "%d disasm round-trips passed, %d failed; "
                "%d additional API checks passed, %d failed\n",
                r.passed, r.failed, r.drift, TESTS.size(), rt_pass, rt_fail,
                el_pass + ai_pass + dc_pass + wr_pass + air_pass + wri_pass + bd_pass + ds_pass
                + em_pass + am_pass,
                extra_fail);
    return (r.failed == 0 && r.drift == 0 && rt_fail == 0 && extra_fail == 0) ? 0 : 1;
}
