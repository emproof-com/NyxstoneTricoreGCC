// NyxstoneTricoreGCC C ABI implementation, wraps the C++
// NyxstoneTricoreGCC class in plain-C functions so language bindings (Rust,
// Python via CFFI) can link directly.  See ../c_api/nyxstone_c.h for the
// public contract.
//
// Hardening invariants enforced here (mirrored in the header docs):
//   - No C++ exception ever crosses the C boundary (UB for C/CFFI/Rust
//     callers).  Every entry point's throwing work runs inside a try block
//     closed by NYXSTONE_CATCH_ALL.
//   - *out_err is set to NULL on entry of every function that takes it,
//     and every failure path sets a message when allocation permits.
//   - Out-parameters are zeroed up front and only assigned once *all*
//     allocations for the call have succeeded, so on any failure (including
//     an exception) the caller observes NULL/0 outputs and nothing leaks.

#include "nyxstone_c.h"
#include "nyxstone/nyxstone.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct nyxstone_handle {
    std::unique_ptr<nyxstone::NyxstoneTricoreGCC> inner;
    explicit nyxstone_handle(std::unique_ptr<nyxstone::NyxstoneTricoreGCC>&& n)
        : inner(std::move(n)) {}
};

namespace {

constexpr const char* kErrNullArg = "null argument";
constexpr const char* kErrAlloc   = "out of memory";

char* dup_cstring(const char* s, size_t len) noexcept {
    auto* p = static_cast<char*>(std::malloc(len + 1));
    if (!p) return nullptr;
    std::memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char* dup_string(const std::string& s) noexcept {
    return dup_cstring(s.data(), s.size());
}

void set_err(char** out_err, const std::string& s) noexcept {
    if (out_err) *out_err = dup_string(s);
}

// const char* overload so the exception handlers below never have to
// construct a std::string (which could itself throw on OOM).
void set_err(char** out_err, const char* s) noexcept {
    if (out_err) *out_err = dup_cstring(s, std::strlen(s));
}

// Exception barrier.  Closes the try block of an entry point.  Because the
// bodies only assign out-parameters after every allocation has succeeded
// (and assignment itself cannot throw), there is never partially
// transferred output to clean up here; the outputs still hold the NULL/0
// written at function entry.
#define NYXSTONE_CATCH_ALL(out_err_, fail_result_)                          \
    catch (const std::exception& e) {                                       \
        set_err(out_err_, e.what());                                        \
        return fail_result_;                                                \
    }                                                                       \
    catch (...) {                                                           \
        set_err(out_err_, "unknown C++ exception");                         \
        return fail_result_;                                                \
    }

nyxstone_status_t classify_assemble_error(const std::string& msg) {
    // The C++ layer's section-violation diagnostic is guaranteed to contain
    // exactly this phrase (src/nyxstone.cpp).  Do not loosen the match to
    // just "section": other assembler errors embed arbitrary gas diagnostic
    // text, which may well mention the word.
    if (msg.find("only .text is allowed") != std::string::npos)
        return NYXSTONE_ERR_SECTION_VIOLATION;
    return NYXSTONE_ERR_ASSEMBLE_FAILED;
}

std::vector<nyxstone::NyxstoneTricoreGCC::LabelDefinition>
to_label_vector(const nyxstone_label_def_t* labels, size_t labels_len) {
    // Callers have already rejected labels == NULL with labels_len > 0.
    std::vector<nyxstone::NyxstoneTricoreGCC::LabelDefinition> v;
    v.reserve(labels_len);
    for (size_t i = 0; i < labels_len; ++i) {
        v.push_back({ std::string(labels[i].name ? labels[i].name : ""),
                      labels[i].address });
    }
    return v;
}

// Returns a fully-built array or NULL; never leaks a partially-built one.
// On success every element satisfies: assembly != NULL, and bytes == NULL
// iff bytes_len == 0.
nyxstone_instruction_t* convert_instructions(
    const std::vector<nyxstone::NyxstoneTricoreGCC::Instruction>& v) noexcept
{
    auto* arr = static_cast<nyxstone_instruction_t*>(
        std::calloc(v.size() ? v.size() : 1, sizeof(nyxstone_instruction_t)));
    if (!arr) return nullptr;
    for (size_t i = 0; i < v.size(); ++i) {
        arr[i].address  = v[i].address;
        arr[i].assembly = dup_string(v[i].assembly);
        if (!arr[i].assembly) {
            nyxstone_free_instructions(arr, i);
            return nullptr;
        }
        if (!v[i].bytes.empty()) {
            arr[i].bytes = static_cast<uint8_t*>(std::malloc(v[i].bytes.size()));
            if (!arr[i].bytes) {
                // arr[i].assembly is already set; i + 1 frees it too
                // (arr[i].bytes is NULL from calloc, free(NULL) is a no-op).
                nyxstone_free_instructions(arr, i + 1);
                return nullptr;
            }
            std::memcpy(arr[i].bytes, v[i].bytes.data(), v[i].bytes.size());
            arr[i].bytes_len = v[i].bytes.size();
        }
    }
    return arr;
}

// Same contract as convert_instructions: all-or-nothing.
nyxstone_reloc_t* convert_relocations(
    const std::vector<nyxstone::RelocationInfo>& v) noexcept
{
    auto* arr = static_cast<nyxstone_reloc_t*>(
        std::calloc(v.size() ? v.size() : 1, sizeof(nyxstone_reloc_t)));
    if (!arr) return nullptr;
    for (size_t i = 0; i < v.size(); ++i) {
        arr[i].offset          = v[i].offset;
        arr[i].addend          = v[i].addend ? *v[i].addend : 0;
        arr[i].has_addend      = v[i].addend ? 1 : 0;
        arr[i].relocation_type = v[i].relocation_type;
        arr[i].symbol.name     = dup_string(v[i].symbol.name);
        if (!arr[i].symbol.name) {
            nyxstone_free_relocations(arr, i);
            return nullptr;
        }
        arr[i].symbol.address  = v[i].symbol.address;
    }
    return arr;
}

}  // namespace

extern "C" {

nyxstone_handle_t* nyxstone_create(char** out_err) {
    if (out_err) *out_err = nullptr;
    try {
        auto r = nyxstone::NyxstoneTricoreGCC::create();
        if (!r) {
            set_err(out_err, r.error());
            return nullptr;
        }
        auto* h = new (std::nothrow) nyxstone_handle(std::move(*r));
        if (!h) set_err(out_err, kErrAlloc);
        return h;
    }
    NYXSTONE_CATCH_ALL(out_err, nullptr)
}

void nyxstone_destroy(nyxstone_handle_t* h) {
    // unique_ptr / NyxstoneTricoreGCC destructors are noexcept; nothing here
    // can throw across the C boundary.
    delete h;
}

nyxstone_status_t nyxstone_assemble(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    uint8_t** out_bytes, size_t* out_len,
    char** out_err)
{
    if (out_err) *out_err = nullptr;
    if (!h || !source || !out_bytes || !out_len
        || (!labels && labels_len > 0)) {
        set_err(out_err, kErrNullArg);
        return NYXSTONE_ERR_NULL_ARG;
    }
    *out_bytes = nullptr;
    *out_len   = 0;
    try {
        auto r = h->inner->assemble(
            std::string(source, src_len), address, to_label_vector(labels, labels_len));
        if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
        if (r->empty()) return NYXSTONE_OK;
        auto* p = static_cast<uint8_t*>(std::malloc(r->size()));
        if (!p) { set_err(out_err, kErrAlloc); return NYXSTONE_ERR_ALLOC; }
        std::memcpy(p, r->data(), r->size());
        *out_bytes = p;
        *out_len   = r->size();
        return NYXSTONE_OK;
    }
    NYXSTONE_CATCH_ALL(out_err, NYXSTONE_ERR_ALLOC)
}

nyxstone_status_t nyxstone_assemble_to_instructions(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    nyxstone_instruction_t** out, size_t* out_n,
    char** out_err)
{
    if (out_err) *out_err = nullptr;
    if (!h || !source || !out || !out_n
        || (!labels && labels_len > 0)) {
        set_err(out_err, kErrNullArg);
        return NYXSTONE_ERR_NULL_ARG;
    }
    *out   = nullptr;
    *out_n = 0;
    try {
        auto r = h->inner->assemble_to_instructions(
            std::string(source, src_len), address, to_label_vector(labels, labels_len));
        if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
        auto* arr = convert_instructions(*r);
        if (!arr) { set_err(out_err, kErrAlloc); return NYXSTONE_ERR_ALLOC; }
        *out   = arr;
        *out_n = r->size();
        return NYXSTONE_OK;
    }
    NYXSTONE_CATCH_ALL(out_err, NYXSTONE_ERR_ALLOC)
}

nyxstone_status_t nyxstone_assemble_with_relocs(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    uint8_t** out_bytes, size_t* out_bytes_len,
    nyxstone_reloc_t** out_relocs, size_t* out_relocs_n,
    char** out_err)
{
    if (out_err) *out_err = nullptr;
    if (!h || !source || !out_bytes || !out_bytes_len
        || !out_relocs || !out_relocs_n
        || (!labels && labels_len > 0)) {
        set_err(out_err, kErrNullArg);
        return NYXSTONE_ERR_NULL_ARG;
    }
    *out_bytes = nullptr; *out_bytes_len = 0;
    *out_relocs = nullptr; *out_relocs_n = 0;
    try {
        auto r = h->inner->assemble_with_relocs(
            std::string(source, src_len), address,
            to_label_vector(labels, labels_len));
        if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
        // Build both outputs in locals first; assign the out-params only
        // once everything has been allocated, so a late failure cannot
        // leave the caller holding half of the result.
        uint8_t* bytes_buf = nullptr;
        if (!r->bytes.empty()) {
            bytes_buf = static_cast<uint8_t*>(std::malloc(r->bytes.size()));
            if (!bytes_buf) { set_err(out_err, kErrAlloc); return NYXSTONE_ERR_ALLOC; }
            std::memcpy(bytes_buf, r->bytes.data(), r->bytes.size());
        }
        nyxstone_reloc_t* relocs = nullptr;
        if (!r->relocations.empty()) {
            relocs = convert_relocations(r->relocations);
            if (!relocs) {
                std::free(bytes_buf);
                set_err(out_err, kErrAlloc);
                return NYXSTONE_ERR_ALLOC;
            }
        }
        *out_bytes     = bytes_buf;
        *out_bytes_len = r->bytes.size();
        *out_relocs    = relocs;
        *out_relocs_n  = r->relocations.size();
        return NYXSTONE_OK;
    }
    NYXSTONE_CATCH_ALL(out_err, NYXSTONE_ERR_ALLOC)
}

nyxstone_status_t nyxstone_assemble_to_instructions_with_relocs(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    nyxstone_instruction_t** out_ins, size_t* out_ins_n,
    nyxstone_reloc_t** out_relocs, size_t* out_relocs_n,
    char** out_err)
{
    if (out_err) *out_err = nullptr;
    if (!h || !source || !out_ins || !out_ins_n
        || !out_relocs || !out_relocs_n
        || (!labels && labels_len > 0)) {
        set_err(out_err, kErrNullArg);
        return NYXSTONE_ERR_NULL_ARG;
    }
    *out_ins = nullptr; *out_ins_n = 0;
    *out_relocs = nullptr; *out_relocs_n = 0;
    try {
        auto r = h->inner->assemble_to_instructions_with_relocs(
            std::string(source, src_len), address,
            to_label_vector(labels, labels_len));
        if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
        auto* arr = convert_instructions(r->instructions);
        if (!arr) { set_err(out_err, kErrAlloc); return NYXSTONE_ERR_ALLOC; }
        nyxstone_reloc_t* relocs = nullptr;
        if (!r->relocations.empty()) {
            relocs = convert_relocations(r->relocations);
            if (!relocs) {
                nyxstone_free_instructions(arr, r->instructions.size());
                set_err(out_err, kErrAlloc);
                return NYXSTONE_ERR_ALLOC;
            }
        }
        *out_ins      = arr;
        *out_ins_n    = r->instructions.size();
        *out_relocs   = relocs;
        *out_relocs_n = r->relocations.size();
        return NYXSTONE_OK;
    }
    NYXSTONE_CATCH_ALL(out_err, NYXSTONE_ERR_ALLOC)
}

nyxstone_status_t nyxstone_disassemble(
    nyxstone_handle_t* h,
    const uint8_t* bytes, size_t bytes_len,
    uint64_t address,
    size_t count,
    char** out_text,
    char** out_err)
{
    if (out_err) *out_err = nullptr;
    if (!h || !out_text || (!bytes && bytes_len > 0)) {
        set_err(out_err, kErrNullArg);
        return NYXSTONE_ERR_NULL_ARG;
    }
    *out_text = nullptr;
    try {
        std::vector<uint8_t> buf;
        if (bytes_len > 0) buf.assign(bytes, bytes + bytes_len);
        auto r = h->inner->disassemble(buf, address, count);
        if (!r) { set_err(out_err, r.error()); return NYXSTONE_ERR_DISASM_FAILED; }
        char* text = dup_string(*r);
        if (!text) { set_err(out_err, kErrAlloc); return NYXSTONE_ERR_ALLOC; }
        *out_text = text;
        return NYXSTONE_OK;
    }
    NYXSTONE_CATCH_ALL(out_err, NYXSTONE_ERR_ALLOC)
}

nyxstone_status_t nyxstone_disassemble_to_instructions(
    nyxstone_handle_t* h,
    const uint8_t* bytes, size_t bytes_len,
    uint64_t address,
    size_t count,
    nyxstone_instruction_t** out, size_t* out_n,
    char** out_err)
{
    if (out_err) *out_err = nullptr;
    if (!h || !out || !out_n || (!bytes && bytes_len > 0)) {
        set_err(out_err, kErrNullArg);
        return NYXSTONE_ERR_NULL_ARG;
    }
    *out   = nullptr;
    *out_n = 0;
    try {
        std::vector<uint8_t> buf;
        if (bytes_len > 0) buf.assign(bytes, bytes + bytes_len);
        auto r = h->inner->disassemble_to_instructions(buf, address, count);
        if (!r) { set_err(out_err, r.error()); return NYXSTONE_ERR_DISASM_FAILED; }
        auto* arr = convert_instructions(*r);
        if (!arr) { set_err(out_err, kErrAlloc); return NYXSTONE_ERR_ALLOC; }
        *out   = arr;
        *out_n = r->size();
        return NYXSTONE_OK;
    }
    NYXSTONE_CATCH_ALL(out_err, NYXSTONE_ERR_ALLOC)
}

// The free helpers only call std::free, which cannot throw; no exception
// barrier needed.

void nyxstone_free_bytes(uint8_t* p) { std::free(p); }
void nyxstone_free_string(char* p)   { std::free(p); }

void nyxstone_free_instructions(nyxstone_instruction_t* p, size_t n) {
    if (!p) return;
    for (size_t i = 0; i < n; ++i) {
        std::free(p[i].assembly);
        std::free(p[i].bytes);
    }
    std::free(p);
}

void nyxstone_free_relocations(nyxstone_reloc_t* p, size_t n) {
    if (!p) return;
    for (size_t i = 0; i < n; ++i)
        std::free(p[i].symbol.name);
    std::free(p);
}

}  // extern "C"
