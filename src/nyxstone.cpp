// NyxstoneTricoreGCC: C++ implementation of the in-process TriCore
// assembler/disassembler.  Public API lives in
// ../include/nyxstone/nyxstone.h; gas internals are wrapped by the C glue
// in nyxstone_glue.c.
//
// Per-assemble flow:
//   1. nyxstone_glue_reset()      , clear symbol table + frag chain.
//   2. Inject `.equ name, value - address` for every LabelDefinition.
//   3. Tokenize `source` on `\n` and `;`; per line strip whitespace and
//      `#...` / `//...` comments.
//   4. Repeatedly strip `<ident>:` label prefixes (calls nyxstone_glue_colon).
//   5. Dispatch:
//        - `.<dir>` → handle_directive (data directives, `.text`/`.section`
//                      acceptance / section-violation flag).
//        - anything else → nyxstone_glue_md_assemble.
//   6. nyxstone_glue_resolve_text_fixups(), relax + apply fixes.
//   7. nyxstone_glue_extract_text_bytes() → return.
//
// `address` is the absolute address of the first instruction.  PC-relative
// branches within the source resolve to the same bytes regardless of
// `address`; the parameter biases (a) the Instruction.address field on
// `assemble_to_instructions` output and (b) every LabelDefinition value
// (encoded as `.equ name, value - address` so external branches encode the
// correct PC-relative displacement).

#include "nyxstone/nyxstone.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
int    nyxstone_glue_init_once (void);
void   nyxstone_glue_reset (void);
void   nyxstone_glue_colon (const char *name);
void   nyxstone_glue_md_assemble (char *line);
void   nyxstone_glue_emit_bytes (const uint8_t *p, size_t n);
size_t nyxstone_glue_frag_now_fix (void);
int    nyxstone_glue_resolve_text_fixups (void);
size_t nyxstone_glue_extract_text_bytes (uint8_t *out, size_t cap);
int    nyxstone_glue_had_errors (void);
int    nyxstone_glue_disasm_one (const uint8_t *bytes, size_t len, uint64_t addr,
                            char **text_out, size_t *n_consumed);

/* Matches the layout of nyxstone_glue_reloc_t in nyxstone_glue.c.  Kept in sync
   by hand, the glue header would pull in gas internals, which we don't
   want in C++ TUs. */
struct nyxstone_glue_reloc_t {
    uint64_t     offset;
    int64_t      addend;
    int          has_addend;
    const char  *symbol_name;
    uint32_t     reloc_type;
};
size_t nyxstone_glue_collect_relocs (nyxstone_glue_reloc_t *out, size_t cap);
}

namespace nyxstone {

namespace {

// Per-call section-violation flag, cleared at the top of every assemble().
bool g_section_violation = false;

// ---- string helpers ------------------------------------------------------
inline void ltrim(std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i) s.erase(0, i);
}
inline void rtrim(std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i-1]))) --i;
    s.resize(i);
}
inline void strip_comment(std::string& s) {
    // Strip `# ...` or `// ...` to end of line.  `;` already split before
    // we get here.
    size_t h = s.find('#');
    size_t l = s.find("//");
    size_t cut = std::min(h, l);
    if (cut != std::string::npos) s.resize(cut);
}

bool parse_int(const char* p, const char* end, int64_t& out, const char*& cur) {
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    if (p >= end) return false;
    char* ep;
    long long v = std::strtoll(p, &ep, 0);
    if (ep == p) return false;
    out = v;
    cur = ep;
    return true;
}

// ---- byte emit helpers ---------------------------------------------------
inline void emit_u8 (uint8_t  v) { nyxstone_glue_emit_bytes(&v, 1); }
inline void emit_u16(uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v>>8) }; nyxstone_glue_emit_bytes(b, 2); }
inline void emit_u32(uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) }; nyxstone_glue_emit_bytes(b, 4); }
inline void emit_u64(uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (8*i)); nyxstone_glue_emit_bytes(b, 8); }

// ---- data directive handlers --------------------------------------------
void emit_int_list(const std::string& args, int width) {
    const char* p = args.c_str();
    const char* end = p + args.size();
    while (p < end) {
        int64_t v = 0;
        const char* next = nullptr;
        if (!parse_int(p, end, v, next)) break;
        switch (width) {
            case 1: emit_u8 (uint8_t (v)); break;
            case 2: emit_u16(uint16_t(v)); break;
            case 4: emit_u32(uint32_t(v)); break;
            case 8: emit_u64(uint64_t(v)); break;
        }
        p = next;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p < end && *p == ',') ++p;
    }
}

void emit_ascii(const std::string& args, bool zero_term) {
    const char* p = args.c_str();
    const char* end = p + args.size();
    while (p < end) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end || *p != '"') break;
        ++p;
        std::string out;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    default: out += *p; break;
                }
                ++p;
            } else {
                out += *p++;
            }
        }
        if (p < end && *p == '"') ++p;
        nyxstone_glue_emit_bytes(reinterpret_cast<const uint8_t*>(out.data()), out.size());
        if (zero_term) emit_u8(0);
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p < end && *p == ',') ++p;
    }
}

// Returns true if `line` was a recognized directive (handled or rejected).
bool handle_directive(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::string d = line.substr(0, i);
    std::string args = (i < line.size()) ? line.substr(i + 1) : "";
    ltrim(args); rtrim(args);

    if (d == ".byte")                                                       { emit_int_list(args, 1); return true; }
    if (d == ".half" || d == ".short" || d == ".2byte")                     { emit_int_list(args, 2); return true; }
    if (d == ".word" || d == ".int" || d == ".long" || d == ".4byte")       { emit_int_list(args, 4); return true; }
    if (d == ".quad" || d == ".8byte")                                      { emit_int_list(args, 8); return true; }
    if (d == ".ascii")                                                      { emit_ascii(args, false); return true; }
    if (d == ".asciz" || d == ".string")                                    { emit_ascii(args, true);  return true; }
    if (d == ".skip" || d == ".space" || d == ".zero") {
        int64_t n = 0; const char* next;
        if (parse_int(args.c_str(), args.c_str() + args.size(), n, next) && n > 0) {
            std::vector<uint8_t> zeros(static_cast<size_t>(n), 0);
            nyxstone_glue_emit_bytes(zeros.data(), zeros.size());
        }
        return true;
    }
    if (d == ".org") {
        int64_t target = 0; const char* next;
        if (parse_int(args.c_str(), args.c_str() + args.size(), target, next)) {
            int64_t cur = static_cast<int64_t>(nyxstone_glue_frag_now_fix());
            if (target > cur) {
                std::vector<uint8_t> zeros(static_cast<size_t>(target - cur), 0);
                nyxstone_glue_emit_bytes(zeros.data(), zeros.size());
            }
        }
        return true;
    }
    if (d == ".align" || d == ".balign") {
        int64_t n = 0; const char* next;
        if (!parse_int(args.c_str(), args.c_str() + args.size(), n, next)) return true;
        size_t boundary;
        if (d == ".align") {
            if (n < 0 || n > 30) return true;
            boundary = size_t(1) << n;
        } else {
            if (n <= 0) return true;
            boundary = static_cast<size_t>(n);
        }
        size_t cur = nyxstone_glue_frag_now_fix();
        size_t pad = (boundary - (cur % boundary)) % boundary;
        if (pad) { std::vector<uint8_t> zeros(pad, 0); nyxstone_glue_emit_bytes(zeros.data(), pad); }
        return true;
    }

    // .text-only restriction (Nyxstone style).
    if (d == ".text") return true;     // already on .text
    if (d == ".section") {
        std::string a = args;
        ltrim(a);
        size_t end = 0;
        while (end < a.size() && !std::isspace(static_cast<unsigned char>(a[end]))
                              && a[end] != ',') ++end;
        std::string secname = a.substr(0, end);
        if (secname == ".text"
            || (secname.size() > 5 && secname.compare(0, 6, ".text.") == 0))
            return true;
        g_section_violation = true;
        return true;
    }
    if (d == ".data" || d == ".bss" || d == ".rodata"
        || d == ".sdata" || d == ".sbss" || d == ".tdata" || d == ".tbss"
        || d == ".pushsection" || d == ".previous") {
        g_section_violation = true;
        return true;
    }

    // Other metadata directives, silently accept; they have no effect on
    // the .text byte stream.
    if (d == ".global" || d == ".globl" || d == ".local" || d == ".weak"
        || d == ".type" || d == ".size" || d == ".file" || d == ".ident"
        || d == ".syntax" || d == ".cpu" || d == ".arch" || d == ".equ"
        || d == ".set" || d == ".extern" || d == ".end") {
        return true;
    }
    return false;
}

// ---- core assemble ------------------------------------------------------
//
// Two operating modes:
//   - `with_relocs == false` (default): inject `.equ name, value - address`
//     for every LabelDefinition.  External labels become absolute symbols;
//     md_apply_fix resolves them; no relocations are emitted.
//
//   - `with_relocs == true` (the `_with_relocs` API path): do NOT inject
//     anything for LabelDefinitions.  References to them stay as undefined
//     symbols; md_apply_fix sees fx_addsy == undefined, leaves fx_done == 0,
//     and records the addend in fx_addnumber (see tc-tricore.c).  We then
//     walk the fix chain and turn each unresolved entry into a
//     RelocationInfo, equivalent to what `gas -r` would write into an ELF
//     object.  The matching LabelDefinition's address is copied into
//     RelocationSymbol::address so the caller has the intended resolution
//     hint in one place.
struct AssembleCore {
    std::vector<uint8_t> bytes;
    std::vector<RelocationInfo> relocations;
};

tl::expected<AssembleCore, std::string> do_assemble(
    const std::string& source,
    uint64_t address,
    const std::vector<NyxstoneTricoreGCC::LabelDefinition>& labels,
    bool with_relocs)
{
    nyxstone_glue_reset();
    g_section_violation = false;
    const int errs_before = nyxstone_glue_had_errors();

    // In the "no relocs" path: prepend `.equ name, value - address` for
    // every external label so gas resolves them inline.
    std::string injected;
    if (!with_relocs) {
        for (const auto& l : labels) {
            const uint64_t rel = l.address - address;
            char buf[64];
            std::snprintf(buf, sizeof(buf), ".equ %s, 0x%llx\n",
                          l.name.c_str(), static_cast<unsigned long long>(rel));
            injected.append(buf);
        }
    }

    auto process = [](std::string line) {
        ltrim(line);
        strip_comment(line);
        rtrim(line);
        if (line.empty()) return;

        // Strip ALL leading `<ident>:` prefixes (gas allows `a: b: nop`).
        while (true) {
            size_t cp = 0;
            while (cp < line.size() && (std::isalnum(static_cast<unsigned char>(line[cp]))
                                        || line[cp] == '_' || line[cp] == '.'
                                        || line[cp] == '$')) ++cp;
            if (cp == 0 || cp >= line.size() || line[cp] != ':') break;
            std::string name = line.substr(0, cp);
            nyxstone_glue_colon(name.c_str());
            line.erase(0, cp + 1);
            ltrim(line);
            if (line.empty()) return;
        }

        if (line[0] == '.') {
            (void) handle_directive(line);
        } else {
            std::vector<char> mut(line.begin(), line.end());
            mut.push_back('\0');
            nyxstone_glue_md_assemble(mut.data());
        }
    };

    auto run = [&](const std::string& text) {
        std::string cur;
        for (char c : text) {
            if (c == '\n' || c == ';') { process(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) process(std::move(cur));
    };

    run(injected);
    run(source);

    if (nyxstone_glue_had_errors() != errs_before)
        return tl::make_unexpected("assemble: gas parse/encode error");
    if (g_section_violation)
        return tl::make_unexpected(
            "assemble: directive switches active section (only .text is allowed)");

    nyxstone_glue_resolve_text_fixups();

    AssembleCore result;
    size_t n = nyxstone_glue_extract_text_bytes(nullptr, 0);
    result.bytes.resize(n);
    if (n) nyxstone_glue_extract_text_bytes(result.bytes.data(), n);

    if (with_relocs) {
        size_t count = nyxstone_glue_collect_relocs(nullptr, 0);
        if (count) {
            std::vector<nyxstone_glue_reloc_t> raw(count);
            nyxstone_glue_collect_relocs(raw.data(), raw.size());
            result.relocations.reserve(count);
            for (const auto& r : raw) {
                RelocationInfo ri;
                ri.offset          = r.offset;
                if (r.has_addend) ri.addend = r.addend;
                ri.relocation_type = r.reloc_type;
                ri.symbol.name     = r.symbol_name ? r.symbol_name : "";
                // Resolve the address hint from the matching LabelDefinition
                // (if any).  Linear scan is fine, `labels` is small.
                for (const auto& l : labels) {
                    if (l.name == ri.symbol.name) {
                        ri.symbol.address = l.address;
                        break;
                    }
                }
                result.relocations.push_back(std::move(ri));
            }
        }
    }
    return result;
}

// ---- core disassemble: returns instructions or an error string ----------
tl::expected<std::vector<NyxstoneTricoreGCC::Instruction>, std::string> do_disassemble(
    const std::vector<uint8_t>& bytes,
    uint64_t address,
    size_t count)
{
    std::vector<NyxstoneTricoreGCC::Instruction> out;
    size_t off = 0;
    size_t decoded = 0;
    while (off < bytes.size() && (count == 0 || decoded < count)) {
        char*  text = nullptr;
        size_t n    = 0;
        int rc = nyxstone_glue_disasm_one(bytes.data() + off, bytes.size() - off,
                                     address + off, &text, &n);
        if (rc <= 0 || n == 0) {
            if (text) std::free(text);
            std::ostringstream err;
            err << "disassemble: libopcodes failed to decode at offset 0x"
                << std::hex << off;
            return tl::make_unexpected(err.str());
        }
        NyxstoneTricoreGCC::Instruction ins;
        ins.address  = address + off;
        ins.assembly = text ? text : "";
        ins.bytes.assign(bytes.begin() + off, bytes.begin() + off + n);
        out.push_back(std::move(ins));
        if (text) std::free(text);
        off += n;
        ++decoded;
    }
    return out;
}

}  // namespace

// =============================================================================
// Public API.
// =============================================================================

tl::expected<std::unique_ptr<NyxstoneTricoreGCC>, std::string> NyxstoneTricoreGCC::create() {
    if (nyxstone_glue_init_once() != 0)
        return tl::make_unexpected(
            "NyxstoneTricoreGCC::create: libbfd init or elf32-tricore target lookup failed");
    return std::unique_ptr<NyxstoneTricoreGCC>(new NyxstoneTricoreGCC());
}

tl::expected<std::vector<uint8_t>, std::string> NyxstoneTricoreGCC::assemble(
    const std::string& assembly,
    uint64_t address,
    const std::vector<LabelDefinition>& labels) const
{
    auto r = do_assemble(assembly, address, labels, /*with_relocs=*/false);
    if (!r) return tl::make_unexpected(r.error());
    return std::move(r->bytes);
}

tl::expected<std::vector<NyxstoneTricoreGCC::Instruction>, std::string>
NyxstoneTricoreGCC::assemble_to_instructions(
    const std::string& assembly,
    uint64_t address,
    const std::vector<LabelDefinition>& labels) const
{
    auto r = do_assemble(assembly, address, labels, /*with_relocs=*/false);
    if (!r) return tl::make_unexpected(r.error());
    return do_disassemble(r->bytes, address, 0);
}

tl::expected<NyxstoneTricoreGCC::AssembleWithRelocsResult, std::string>
NyxstoneTricoreGCC::assemble_with_relocs(
    const std::string& assembly,
    uint64_t address,
    const std::vector<LabelDefinition>& labels) const
{
    auto r = do_assemble(assembly, address, labels, /*with_relocs=*/true);
    if (!r) return tl::make_unexpected(r.error());
    AssembleWithRelocsResult out;
    out.bytes        = std::move(r->bytes);
    out.relocations  = std::move(r->relocations);
    return out;
}

tl::expected<NyxstoneTricoreGCC::AssembleInstructionsWithRelocsResult, std::string>
NyxstoneTricoreGCC::assemble_to_instructions_with_relocs(
    const std::string& assembly,
    uint64_t address,
    const std::vector<LabelDefinition>& labels) const
{
    auto r = do_assemble(assembly, address, labels, /*with_relocs=*/true);
    if (!r) return tl::make_unexpected(r.error());
    auto insns = do_disassemble(r->bytes, address, 0);
    if (!insns) return tl::make_unexpected(insns.error());
    AssembleInstructionsWithRelocsResult out;
    out.instructions = std::move(*insns);
    out.relocations  = std::move(r->relocations);
    return out;
}

tl::expected<std::string, std::string> NyxstoneTricoreGCC::disassemble(
    const std::vector<uint8_t>& bytes,
    uint64_t address,
    size_t count) const
{
    auto v = do_disassemble(bytes, address, count);
    if (!v) return tl::make_unexpected(v.error());
    std::string out;
    for (auto& i : *v) { out += i.assembly; out += '\n'; }
    return out;
}

tl::expected<std::vector<NyxstoneTricoreGCC::Instruction>, std::string>
NyxstoneTricoreGCC::disassemble_to_instructions(
    const std::vector<uint8_t>& bytes,
    uint64_t address,
    size_t count) const
{
    return do_disassemble(bytes, address, count);
}

}  // namespace nyxstone
