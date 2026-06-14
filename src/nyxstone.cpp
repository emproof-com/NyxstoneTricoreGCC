// NyxstoneTricoreGCC: C++ implementation of the in-process TriCore
// assembler/disassembler.  Public API lives in
// ../include/nyxstone/nyxstone.h; gas internals are wrapped by the C glue
// in nyxstone_glue.c.
//
// Per-assemble flow:
//   1. nyxstone_glue_reset()      , clear symbol table + frag chain.
//   2. nyxstone_glue_begin_capture(), capture gas's stderr diagnostics.
//   3. Plain path only: nyxstone_glue_define_abs(name, address) for every
//      LabelDefinition (absolute symbols; resolved during fixups).
//   4. Tokenize `source` on `\n` and `;` (never inside string literals);
//      per line strip whitespace and `#...` / `//...` comments.
//   5. Repeatedly strip `<ident>:` label prefixes (nyxstone_glue_colon;
//      all-digit names go through nyxstone_glue_fb_label).
//   6. Dispatch:
//        - `.<dir>` → handle_directive (data directives, `.equ`/`.set`,
//                      `.align`/`.org` frags, `.text`/`.section` acceptance
//                      / section-violation flag; unknown directives error).
//        - anything else → nyxstone_glue_md_assemble.
//   7. nyxstone_glue_resolve_text_fixups(address), relax + apply fixes.
//   8. Fail on captured gas errors / unresolved symbols, else
//      nyxstone_glue_extract_text_bytes() → return.
//
// `address` is the absolute address of the first instruction.  PC-relative
// branches within the source resolve to the same bytes regardless of
// `address`; the parameter biases (a) the Instruction.address field on
// `assemble_to_instructions` output and (b) the displacement encoded for
// references to LabelDefinition symbols (displacement = label - (address +
// PC offset), computed in apply_text_fixups).

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
void   nyxstone_glue_define_abs (const char *name, uint64_t value);
void   nyxstone_glue_set_sym (const char *name, const char *value_expr);
void   nyxstone_glue_fb_label (unsigned int n);
void   nyxstone_glue_md_assemble (char *line);
void   nyxstone_glue_emit_bytes (const uint8_t *p, size_t n);
void   nyxstone_glue_emit_cons (const char *args, int nbytes);
void   nyxstone_glue_align (unsigned int p2, int fill, unsigned int max);
void   nyxstone_glue_org (uint64_t target, int fill);
void   nyxstone_glue_error (const char *msg);
void   nyxstone_glue_begin_capture (void);
const char *nyxstone_glue_end_capture (void);
int    nyxstone_glue_resolve_text_fixups (uint64_t base);
const char *nyxstone_glue_unsupported_reloc (void);
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

// Per-call directive error (unknown directive, malformed operand, ...).
// First error wins; checked by do_assemble after the source is processed.
std::string g_directive_error;

void directive_error(std::string msg) {
    if (g_directive_error.empty()) g_directive_error = std::move(msg);
}

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
    // Strip `# ...` (gas's TriCore comment char) or `// ...` to end of line,
    // but never inside a "..." string literal -- `.asciz "a#b"` keeps its
    // hash.  `;` statement separators were already split off before we get
    // here (equally quote-aware, see split_statements).
    bool in_str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) ++i;       // skip escaped char
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '#' || (c == '/' && i + 1 < s.size() && s[i+1] == '/')) {
            s.resize(i);
            return;
        }
    }
}

// Turn gas's captured stderr text into a single-line diagnostic: drop the
// "Assembler messages:" banner, strip per-line "Error: "/"Warning: "
// prefixes' surrounding noise, and join the remaining lines with "; ".
std::string clean_gas_diag(const char* captured) {
    std::string out;
    if (!captured) return out;
    std::istringstream in(captured);
    std::string line;
    while (std::getline(in, line)) {
        rtrim(line); ltrim(line);
        if (line.empty()) continue;
        if (line == "Assembler messages:") continue;
        if (!out.empty()) out += "; ";
        out += line;
    }
    return out;
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

// True if `args` is a (possibly empty) comma-separated list of plain integer
// literals — i.e. the fast emit_int_list path is sufficient.  A symbol or
// expression (e.g. `label`, `end-start`, `4+x`) makes this false, in which case
// the directive is forwarded to gas's `cons` so it can emit bytes + fixups.
bool all_int_literals(const std::string& args) {
    const char* p = args.c_str();
    const char* end = p + args.size();
    while (p < end) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end) break;
        int64_t v; const char* next = nullptr;
        if (!parse_int(p, end, v, next)) return false;
        p = next;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end) break;
        if (*p != ',') return false;   // trailing token after a literal (expr)
        ++p;
    }
    return true;
}

// Parse up to `max_args` comma-separated integer literals from `args` into
// `out`.  Returns the number parsed, or -1 if the text is malformed (extra
// tokens, non-literal operand).  Missing trailing operands keep their
// caller-provided defaults.
int parse_int_args(const std::string& args, int64_t* out, int max_args) {
    const char* p = args.c_str();
    const char* end = p + args.size();
    int n = 0;
    while (n < max_args) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end) return n;
        const char* next = nullptr;
        if (!parse_int(p, end, out[n], next)) return -1;
        ++n;
        p = next;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end) return n;
        if (*p != ',') return -1;
        ++p;
    }
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    return (p >= end) ? n : -1;
}

void emit_ascii(const std::string& d, const std::string& args, bool zero_term) {
    const char* p = args.c_str();
    const char* end = p + args.size();
    bool any = false;
    while (p < end) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p >= end) break;
        if (*p != '"') {
            directive_error(d + ": expected a quoted string, got '" + args + "'");
            return;
        }
        any = true;
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
        if (p >= end) {
            directive_error(d + ": unterminated string");
            return;
        }
        ++p;  // closing quote
        nyxstone_glue_emit_bytes(reinterpret_cast<const uint8_t*>(out.data()), out.size());
        if (zero_term) emit_u8(0);
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p < end && *p == ',') ++p;
    }
    if (!any) directive_error(d + ": expected a quoted string");
}

// Returns true if `line` was a recognized directive (handled or rejected).
bool handle_directive(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    std::string d = line.substr(0, i);
    std::string args = (i < line.size()) ? line.substr(i + 1) : "";
    ltrim(args); rtrim(args);

    // Data lists: pure-literal lists take the fast in-house path; lists that
    // contain a symbol or expression are forwarded to gas's `cons`, which emits
    // the bytes and creates fixups (resolved locally / recorded as relocs).
    {
        int w = 0;
        if (d == ".byte")                                                       w = 1;
        else if (d == ".half" || d == ".hword" || d == ".short" || d == ".2byte") w = 2;
        else if (d == ".word" || d == ".int" || d == ".long" || d == ".4byte")  w = 4;
        else if (d == ".quad" || d == ".8byte")                                 w = 8;
        if (w) {
            if (all_int_literals(args)) emit_int_list(args, w);
            else                        nyxstone_glue_emit_cons(args.c_str(), w);
            return true;
        }
    }
    if (d == ".ascii")                                                      { emit_ascii(d, args, false); return true; }
    if (d == ".asciz" || d == ".string")                                    { emit_ascii(d, args, true);  return true; }
    if (d == ".skip" || d == ".space" || d == ".zero") {
        // .skip/.space size[, fill]   .zero size   (fill defaults to 0)
        int64_t vals[2] = {0, 0};
        int n_args = parse_int_args(args, vals, (d == ".zero") ? 1 : 2);
        if (n_args < 1) { directive_error(d + ": expected a size operand, got '" + args + "'"); return true; }
        if (vals[0] < 0) { directive_error(d + ": negative size"); return true; }
        if (vals[0] > 0) {
            std::vector<uint8_t> fill(static_cast<size_t>(vals[0]),
                                      static_cast<uint8_t>(vals[1]));
            nyxstone_glue_emit_bytes(fill.data(), fill.size());
        }
        return true;
    }
    if (d == ".org") {
        // .org target[, fill]: emitted as a real rs_org frag so the padding
        // is sized after branch relaxation; moving backwards is a gas error.
        int64_t vals[2] = {0, 0};
        int n_args = parse_int_args(args, vals, 2);
        if (n_args < 1) { directive_error(d + ": expected an offset operand, got '" + args + "'"); return true; }
        if (vals[0] < 0) { directive_error(d + ": negative offset"); return true; }
        nyxstone_glue_org(static_cast<uint64_t>(vals[0]), static_cast<int>(vals[1]));
        return true;
    }
    if (d == ".align" || d == ".p2align" || d == ".balign") {
        // .align/.p2align p2[, fill[, max]] (power-of-two exponent, the gas
        // s_align_ptwo semantics this TriCore port uses);
        // .balign bytes[, fill[, max]] (byte boundary, must be a power of 2).
        // Emitted as a real rs_align frag so padding is sized post-relax.
        int64_t vals[3] = {0, 0, 0};
        int n_args = parse_int_args(args, vals, 3);
        if (n_args < 1) { directive_error(d + ": expected an alignment operand, got '" + args + "'"); return true; }
        unsigned p2;
        if (d == ".balign") {
            int64_t b = vals[0];
            if (b <= 0 || (b & (b - 1)) != 0) {
                directive_error(d + ": alignment is not a power of 2");
                return true;
            }
            p2 = 0;
            while ((int64_t(1) << p2) < b) ++p2;
        } else {
            if (vals[0] < 0 || vals[0] > 30) {
                directive_error(d + ": alignment exponent out of range (0..30)");
                return true;
            }
            p2 = static_cast<unsigned>(vals[0]);
        }
        nyxstone_glue_align(p2, static_cast<int>(vals[1]),
                            (n_args >= 3 && vals[2] > 0)
                                ? static_cast<unsigned>(vals[2]) : 0u);
        return true;
    }
    if (d == ".equ" || d == ".set") {
        // .equ name, expr -- gas's `equals` handles the expression grammar
        // (constants, label arithmetic, forward references).
        size_t comma = args.find(',');
        std::string name = args.substr(0, comma == std::string::npos ? args.size() : comma);
        rtrim(name);
        std::string expr = (comma == std::string::npos) ? "" : args.substr(comma + 1);
        ltrim(expr); rtrim(expr);
        if (name.empty() || expr.empty()) {
            directive_error(d + ": expected 'name, expression', got '" + args + "'");
            return true;
        }
        nyxstone_glue_set_sym(name.c_str(), expr.c_str());
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
        || d == ".syntax" || d == ".cpu" || d == ".arch"
        || d == ".extern" || d == ".end") {
        return true;
    }
    return false;
}

// ---- core assemble ------------------------------------------------------
//
// Two operating modes:
//   - `with_relocs == false` (default): define every LabelDefinition as an
//     absolute symbol (nyxstone_glue_define_abs).  References resolve in
//     apply_text_fixups; an unresolved symbol (no definition anywhere) is
//     an error.  No relocations are emitted.
//
//   - `with_relocs == true` (the `_with_relocs` API path): do NOT define
//     LabelDefinitions.  References to them stay as undefined symbols;
//     md_apply_fix sees fx_addsy == undefined, leaves fx_done == 0,
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
    g_directive_error.clear();
    const int errs_before = nyxstone_glue_had_errors();

    // Capture gas's stderr for the whole parse + fixup window: as_bad/as_warn
    // text becomes part of our error strings instead of polluting the host
    // process's stderr.
    nyxstone_glue_begin_capture();

    auto process = [](std::string line) {
        ltrim(line);
        strip_comment(line);
        rtrim(line);
        if (line.empty()) return;

        // Strip ALL leading `<ident>:` prefixes (gas allows `a: b: nop`).
        // All-digit names are gas "fb" numeric local labels (`1:`,
        // referenced as `1b`/`1f`) and need the dedicated instance counter.
        while (true) {
            size_t cp = 0;
            bool all_digits = true;
            while (cp < line.size() && (std::isalnum(static_cast<unsigned char>(line[cp]))
                                        || line[cp] == '_' || line[cp] == '.'
                                        || line[cp] == '$')) {
                if (!std::isdigit(static_cast<unsigned char>(line[cp]))) all_digits = false;
                ++cp;
            }
            if (cp == 0 || cp >= line.size() || line[cp] != ':') break;
            std::string name = line.substr(0, cp);
            if (all_digits)
                nyxstone_glue_fb_label(static_cast<unsigned int>(std::strtoul(name.c_str(), nullptr, 10)));
            else
                nyxstone_glue_colon(name.c_str());
            line.erase(0, cp + 1);
            ltrim(line);
            if (line.empty()) return;
        }

        if (line[0] == '.') {
            if (!handle_directive(line)) {
                size_t sp = 0;
                while (sp < line.size() && !std::isspace(static_cast<unsigned char>(line[sp]))) ++sp;
                directive_error("unsupported directive '" + line.substr(0, sp) + "'");
            }
        } else {
            std::vector<char> mut(line.begin(), line.end());
            mut.push_back('\0');
            nyxstone_glue_md_assemble(mut.data());
        }
    };

    // Split on '\n' and ';' (gas's TriCore line separator), but never inside
    // a "..." string literal: `.ascii "a;b"` is one statement.
    auto run = [&](const std::string& text) {
        std::string cur;
        bool in_str = false;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (in_str) {
                cur.push_back(c);
                if (c == '\\' && i + 1 < text.size()) cur.push_back(text[++i]);
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; cur.push_back(c); continue; }
            if (c == '\n' || c == ';') { process(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) process(std::move(cur));
    };

    run(source);

    // Plain path: define every LabelDefinition as an absolute symbol holding
    // its full address, so the fixup pass resolves references to it.  This
    // happens AFTER parsing on purpose: at parse time the names look
    // undefined, so gas emits the longest (value-independent) branch forms
    // and the produced bytes are invariant under the `address` parameter.
    // In the relocs path the labels stay undefined so each reference is
    // reported as a relocation instead.
    if (!with_relocs)
        for (const auto& l : labels)
            nyxstone_glue_define_abs(l.name.c_str(), l.address);

    const bool parse_err = nyxstone_glue_had_errors() != errs_before;
    int unresolved = 0;
    if (!parse_err && !g_section_violation && g_directive_error.empty())
        unresolved = nyxstone_glue_resolve_text_fixups(address);

    const std::string diag = clean_gas_diag(nyxstone_glue_end_capture());

    if (!g_directive_error.empty())
        return tl::make_unexpected("assemble: " + g_directive_error);
    if (g_section_violation)
        return tl::make_unexpected(
            "assemble: directive switches active section (only .text is allowed)");
    if (parse_err)
        return tl::make_unexpected(
            "assemble: " + (diag.empty() ? std::string("gas parse/encode error") : diag));

    if (const char* bad = nyxstone_glue_unsupported_reloc())
        return tl::make_unexpected(
            std::string("assemble: unsupported relocation ") + bad
            + " (local branch displacement could not be encoded)");

    // Errors raised during relax / md_convert_frag / fixup (e.g. branch
    // displacement out of range, `.org` moving backwards) happen after the
    // parse-time check above, so re-check the error counter and fail loudly
    // rather than return bytes.
    if (nyxstone_glue_had_errors() != errs_before)
        return tl::make_unexpected(
            "assemble: " + (diag.empty() ? std::string("gas error during relax/fixup") : diag));

    // Plain path: every symbol must have resolved -- a leftover unresolved
    // fixup means a reference to a label that is neither defined in the
    // source nor supplied via LabelDefinition.  Emitting 0 silently (gas's
    // object-file behaviour) is wrong for a raw byte stream, so fail with
    // the offending names.
    if (!with_relocs && unresolved > 0) {
        size_t count = nyxstone_glue_collect_relocs(nullptr, 0);
        std::vector<nyxstone_glue_reloc_t> raw(count);
        if (count) nyxstone_glue_collect_relocs(raw.data(), raw.size());
        std::string names;
        for (const auto& r : raw) {
            if (!r.symbol_name) continue;
            if (!names.empty()) names += ", ";
            names += r.symbol_name;
        }
        return tl::make_unexpected(
            "assemble: undefined label(s): " + (names.empty() ? std::string("<unknown>") : names)
            + " (define them in the source or pass a LabelDefinition)");
    }

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
