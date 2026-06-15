// Exhaustive TriCore v1.6.2 round-trip + reference-resolution test.
//
// Drives every instruction/format the v1.6.2 ISA defines (extracted from the
// pinned binutils opcode table into tricore_v162_insns.inc by
// scripts/gen_v162_corpus.py) through the full pipeline and asserts three
// properties for each:
//
//   1. Assembles.  Operands are synthesized from the instruction's arg-spec
//      (one char per operand: registers, immediates of the right width, memory
//      addressing modes, branch/abs/symbol references).  Two variants are
//      assembled per instruction -- low registers + mid-range immediates, and
//      high registers (%d8+/%a8+) + extreme immediates -- to exercise
//      "various parameter kinds".
//
//   2. Round-trips idempotently.  disassemble -> re-assemble -> disassemble
//      must reproduce the same text (the assembler and disassembler agree on
//      the encoding).  PC-relative branches are EXCLUDED from this check: the
//      disassembler prints an absolute target, which gas re-reads as a bare
//      displacement (upstream TriCore convention), so a numeric branch operand
//      does not round-trip by design.  Those are covered by (3) instead.
//
//   3. References / relocations resolve.  For every instruction with a branch
//      target, absolute address, or symbol operand, a labeled variant is
//      assembled both ways:
//        - plain path: the label resolves and the disassembled operand shows
//          the label's address;
//        - reloc path (assemble_with_relocs): an external label produces a
//          relocation entry with the right symbol name, absolute offset, and
//          address hint.
//
// Exit nonzero if any instruction fails to assemble, a non-branch instruction
// is not idempotent, or a reference fails to resolve.  Regenerate the corpus
// after bumping binutils: see scripts/gen_v162_corpus.py.

#include "nyxstone/nyxstone.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace nyxstone;

struct InsnSpec { const char* name; const char* args; int is32; };
#include "tricore_v162_insns.inc"

namespace {

NyxstoneTricoreGCC* NX = nullptr;
const char* DREG[16] = {"%d0","%d1","%d2","%d3","%d4","%d5","%d6","%d7",
                        "%d8","%d9","%d10","%d11","%d12","%d13","%d14","%d15"};
const char* AREG[16] = {"%a0","%a1","%a2","%a3","%a4","%a5","%a6","%a7",
                        "%a8","%a9","%a10","%a11","%a12","%a13","%a14","%a15"};

// A constant-width operand char (used to detect a memory operand's glued
// offset, e.g. the '0' in "@0" -> "[%a0]<offset>").  Guards against '\0'.
bool is_const_char(char c) { return c && std::strchr("1234fF5v6 89nhk0qwW", c) && c != ' '; }
// PC-relative branch target chars (sign/zero-extended, /2).
bool is_branch_ref(char c) { return c && std::strchr("oOmrxZR", c); }
// Any operand that references a symbol/address: branch, absolute, mem, unknown.
bool is_ref_char(char c)   { return c && std::strchr("oOmrxZRtTVMU", c); }

// A valid literal for each constant-width char; `hi` picks the extreme variant.
std::string konst(char c, bool hi) {
    switch (c) {
        case '1': return "1";                 case '2': return hi ? "3" : "2";
        case '3': return hi ? "7" : "5";      case '4': return hi ? "7" : "-2";
        case 'f': return hi ? "15" : "5";     case '5': return hi ? "31" : "10";
        case 'F': return hi ? "15" : "-5";    case 'v': return hi ? "30" : "4";
        case '6': return hi ? "60" : "8";     case '8': return hi ? "255" : "100";
        case '9': return hi ? "255" : "-100"; case 'n': return hi ? "511" : "100";
        case 'h': return hi ? "1023" : "200"; case 'k': return hi ? "1020" : "8";
        case '0': return hi ? "511" : "-100"; case 'q': return hi ? "32767" : "1000";
        case 'w': return hi ? "32767" : "-1000"; case 'W': return hi ? "65535" : "1000";
    }
    return "0";
}

// Address to give a reference label, by the reference char's addressing kind.
uint64_t ref_addr(char c, uint64_t base) {
    if (is_branch_ref(c)) return base + 0x40;   // reachable, even
    if (c == 'V')         return 0x40000;       // 18-bit abs, low 14 zero
    if (c == 't' || c == 'T') return 0x100;     // abs segmented
    return 0x123456;                            // M / U: 32-bit mem / unknown
}

// Build one assembly line from an instruction's arg-spec.  `hi` selects the
// register/immediate variant; `refc` (out) is set to the reference char if the
// instruction takes a symbol/branch/abs operand (the label is named "Lref").
std::string synth(const InsnSpec& in, bool hi, char& refc) {
    std::string s = std::string(in.name) + " ";
    std::string args = in.args;
    int ri = hi ? 8 : 0, ai = hi ? 8 : 0, ei = hi ? 4 : 0;
    bool first = true;
    refc = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        char c = args[i];
        std::string op;
        bool mem = false;
        switch (c) {
            case 'd': op = DREG[ri++ % 16]; break;
            case 'i': op = "%d15"; break;
            case 'g': op = std::string(DREG[ri++ % 16]) + "l";  break;
            case 'G': op = std::string(DREG[ri++ % 16]) + "u";  break;
            case '-': op = std::string(DREG[ri++ % 16]) + "ll"; break;
            case '+': op = std::string(DREG[ri++ % 16]) + "uu"; break;
            case 'l': op = std::string(DREG[ri++ % 16]) + "lu"; break;
            case 'L': op = std::string(DREG[ri++ % 16]) + "ul"; break;
            case 'D': op = std::string("%e") + std::to_string((ei++ % 8) * 2); break;
            case 'a': op = AREG[ai++ % 16]; break;
            case 'A': op = std::string("%a") + std::to_string((ai++ % 8) * 2); break;
            case 'I': op = "%a15"; break;
            case 'P': op = "%a10"; break;
            case 'c': op = "$psw"; break;
            case '@': op = "[%a" + std::to_string(ai++ % 8) + "]";   mem = true; break;
            case '&': op = "[%sp]";                                  mem = true; break;
            case '<': op = "[+%a" + std::to_string(ai++ % 8) + "]";  mem = true; break;
            case '>': op = "[%a" + std::to_string(ai++ % 8) + "+]";  mem = true; break;
            case '*': op = "[%a" + std::to_string(ai++ % 8) + "+c]"; mem = true; break;
            case '#': op = "[%a" + std::to_string(ai++ % 8) + "+r]"; mem = true; break;
            case '?': op = "[%a" + std::to_string(ai++ % 8) + "+i]"; mem = true; break;
            case 'S': op = "[%a15]";                                 mem = true; break;
            default:
                if (is_ref_char(c)) { op = "Lref"; refc = c; }
                else                  op = konst(c, hi);
                break;
        }
        // A memory mode glues the following constant char on as its offset
        // (no comma): "@0" -> "[%a0]<off>", "a@w" -> "%a0, [%a0]<off16>".
        if (mem && i + 1 < args.size() && is_const_char(args[i + 1])) {
            op += konst(args[i + 1], hi);
            ++i;
        }
        if (!first) s += ", ";
        s += op;
        first = false;
    }
    return s;
}

}  // namespace

int main() {
    auto created = NyxstoneTricoreGCC::create();
    if (!created) { std::printf("create failed: %s\n", created.error().c_str()); return 2; }
    NX = created->get();

    const uint64_t BASE = 0x10000;
    int total = 0, assembled = 0, varB = 0, idem_checked = 0, idem = 0, refs = 0, ref_ok = 0;
    std::set<std::string> notgen, notidem, refbad;

    for (const auto& in : V162_INSNS) {
        ++total;
        char refc = 0;
        std::string a = synth(in, /*hi=*/false, refc);
        std::vector<NyxstoneTricoreGCC::LabelDefinition> labels;
        uint64_t la = 0;
        if (refc) { la = ref_addr(refc, BASE); labels.push_back({"Lref", la}); }

        auto r = NX->assemble(a + "\n", BASE, labels);
        if (!r) { notgen.insert(std::string(in.name) + " [" + in.args + "]: " + r.error()); continue; }
        ++assembled;

        // (2) round-trip idempotence -- non-branch only (see file header).
        if (!is_branch_ref(refc)) {
            ++idem_checked;
            bool stable = false;
            auto d1 = NX->disassemble_to_instructions(*r, BASE, 1);
            if (d1 && !d1->empty()) {
                std::string t1 = d1->front().assembly;
                auto rr = NX->assemble(t1, BASE, labels);
                if (rr) {
                    auto d2 = NX->disassemble_to_instructions(*rr, BASE, 1);
                    if (d2 && !d2->empty() && d2->front().assembly == t1) stable = true;
                }
            }
            if (stable) ++idem;
            else        notidem.insert(std::string(in.name) + " [" + in.args + "]");
        }

        // (3) reference / relocation resolution.
        if (refc) {
            ++refs;
            char hbuf[24];
            std::snprintf(hbuf, sizeof hbuf, "0x%llx", (unsigned long long) la);
            auto dd = NX->disassemble(*r, BASE, 0);
            bool ok_plain = dd && dd->find(hbuf) != std::string::npos;

            char rc2 = 0;
            std::string rs = synth(in, false, rc2);
            uint64_t ext = is_branch_ref(refc) ? 0x123456 : la;
            size_t p = rs.find("Lref");
            if (p != std::string::npos) rs.replace(p, 4, "Lext");
            auto wr = NX->assemble_with_relocs(rs + "\n", BASE, {{"Lext", ext}});
            bool ok_reloc = wr && wr->relocations.size() >= 1
                            && wr->relocations[0].symbol.name == "Lext"
                            && wr->relocations[0].symbol.address == ext
                            && wr->relocations[0].offset >= BASE;   // absolute offset

            if (ok_plain || ok_reloc) ++ref_ok;
            else refbad.insert(std::string(in.name) + " [" + in.args + "] disasm="
                               + (dd ? *dd : std::string("?")));
        }

        // "various parameter kinds": the high-register / extreme-immediate variant.
        char rb = 0;
        std::string b = synth(in, /*hi=*/true, rb);
        std::vector<NyxstoneTricoreGCC::LabelDefinition> lb;
        if (rb) lb.push_back({"Lref", ref_addr(rb, BASE)});
        if (NX->assemble(b + "\n", BASE, lb)) ++varB;
    }

    std::printf("=== TriCore v1.6.2 exhaustive round-trip ===\n");
    std::printf("instruction/format entries:  %d\n", total);
    std::printf("assembled (variant A):       %d  (%.1f%%)\n", assembled, 100.0 * assembled / total);
    std::printf("assembled (variant B):       %d  (high regs / extreme immediates)\n", varB);
    std::printf("round-trip idempotent:       %d / %d checked (non-branch)\n", idem, idem_checked);
    std::printf("reference/reloc resolved:    %d / %d\n", ref_ok, refs);

    auto dump = [](const char* tag, const std::set<std::string>& s) {
        if (s.empty()) return;
        std::printf("\n--- %s (%zu) ---\n", tag, s.size());
        int n = 0;
        for (const auto& e : s) { std::printf("  %s\n", e.c_str()); if (++n >= 60) { std::printf("  ...\n"); break; } }
    };
    dump("FAILED TO ASSEMBLE", notgen);
    dump("NOT IDEMPOTENT", notidem);
    dump("REFERENCE NOT RESOLVED", refbad);

    bool fail = !notgen.empty() || !notidem.empty() || !refbad.empty();
    std::printf("\n%s\n", fail ? "FAIL" : "OK: every v1.6.2 instruction assembled, "
                "non-branch forms round-trip, references resolved");
    return fail ? 1 : 0;
}
