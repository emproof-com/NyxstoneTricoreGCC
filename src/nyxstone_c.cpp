// NyxstoneTricoreGCC C ABI implementation, wraps the C++
// NyxstoneTricoreGCC class in plain-C functions so language bindings (Rust,
// Python via CFFI) can link directly.  See ../c_api/nyxstone_c.h for the
// public contract.

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

char* dup_string(const std::string& s) {
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return p;
}

void set_err(char** out_err, const std::string& s) {
    if (out_err) *out_err = dup_string(s);
}

nyxstone_status_t classify_assemble_error(const std::string& msg) {
    if (msg.find("section") != std::string::npos) return NYXSTONE_ERR_SECTION_VIOLATION;
    return NYXSTONE_ERR_ASSEMBLE_FAILED;
}

std::vector<nyxstone::NyxstoneTricoreGCC::LabelDefinition>
to_label_vector(const nyxstone_label_def_t* labels, size_t labels_len) {
    std::vector<nyxstone::NyxstoneTricoreGCC::LabelDefinition> v;
    v.reserve(labels_len);
    for (size_t i = 0; i < labels_len; ++i) {
        v.push_back({ std::string(labels[i].name ? labels[i].name : ""),
                      labels[i].address });
    }
    return v;
}

nyxstone_instruction_t* convert_instructions(
    const std::vector<nyxstone::NyxstoneTricoreGCC::Instruction>& v)
{
    auto* arr = static_cast<nyxstone_instruction_t*>(
        std::calloc(v.size() ? v.size() : 1, sizeof(nyxstone_instruction_t)));
    if (!arr) return nullptr;
    for (size_t i = 0; i < v.size(); ++i) {
        arr[i].address  = v[i].address;
        arr[i].assembly = dup_string(v[i].assembly);
        arr[i].bytes_len = v[i].bytes.size();
        arr[i].bytes = static_cast<uint8_t*>(
            v[i].bytes.empty() ? nullptr
                               : std::malloc(v[i].bytes.size()));
        if (arr[i].bytes)
            std::memcpy(arr[i].bytes, v[i].bytes.data(), v[i].bytes.size());
    }
    return arr;
}

nyxstone_reloc_t* convert_relocations(
    const std::vector<nyxstone::RelocationInfo>& v)
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
        arr[i].symbol.address  = v[i].symbol.address;
    }
    return arr;
}

}  // namespace

extern "C" {

nyxstone_handle_t* nyxstone_create(char** out_err) {
    auto r = nyxstone::NyxstoneTricoreGCC::create();
    if (!r) {
        set_err(out_err, r.error());
        return nullptr;
    }
    return new (std::nothrow) nyxstone_handle(std::move(*r));
}

void nyxstone_destroy(nyxstone_handle_t* h) {
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
    if (!h || !source || !out_bytes || !out_len) return NYXSTONE_ERR_NULL_ARG;
    *out_bytes = nullptr;
    *out_len   = 0;
    auto r = h->inner->assemble(
        std::string(source, src_len), address, to_label_vector(labels, labels_len));
    if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
    if (r->empty()) return NYXSTONE_OK;
    auto* p = static_cast<uint8_t*>(std::malloc(r->size()));
    if (!p) return NYXSTONE_ERR_ALLOC;
    std::memcpy(p, r->data(), r->size());
    *out_bytes = p;
    *out_len   = r->size();
    return NYXSTONE_OK;
}

nyxstone_status_t nyxstone_assemble_to_instructions(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    nyxstone_instruction_t** out, size_t* out_n,
    char** out_err)
{
    if (!h || !source || !out || !out_n) return NYXSTONE_ERR_NULL_ARG;
    *out   = nullptr;
    *out_n = 0;
    auto r = h->inner->assemble_to_instructions(
        std::string(source, src_len), address, to_label_vector(labels, labels_len));
    if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
    auto* arr = convert_instructions(*r);
    if (!arr) return NYXSTONE_ERR_ALLOC;
    *out   = arr;
    *out_n = r->size();
    return NYXSTONE_OK;
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
    if (!h || !source || !out_bytes || !out_bytes_len
        || !out_relocs || !out_relocs_n) return NYXSTONE_ERR_NULL_ARG;
    *out_bytes = nullptr; *out_bytes_len = 0;
    *out_relocs = nullptr; *out_relocs_n = 0;
    auto r = h->inner->assemble_with_relocs(
        std::string(source, src_len), address,
        to_label_vector(labels, labels_len));
    if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
    if (!r->bytes.empty()) {
        auto* p = static_cast<uint8_t*>(std::malloc(r->bytes.size()));
        if (!p) return NYXSTONE_ERR_ALLOC;
        std::memcpy(p, r->bytes.data(), r->bytes.size());
        *out_bytes     = p;
        *out_bytes_len = r->bytes.size();
    }
    if (!r->relocations.empty()) {
        auto* rs = convert_relocations(r->relocations);
        if (!rs) return NYXSTONE_ERR_ALLOC;
        *out_relocs   = rs;
        *out_relocs_n = r->relocations.size();
    }
    return NYXSTONE_OK;
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
    if (!h || !source || !out_ins || !out_ins_n
        || !out_relocs || !out_relocs_n) return NYXSTONE_ERR_NULL_ARG;
    *out_ins = nullptr; *out_ins_n = 0;
    *out_relocs = nullptr; *out_relocs_n = 0;
    auto r = h->inner->assemble_to_instructions_with_relocs(
        std::string(source, src_len), address,
        to_label_vector(labels, labels_len));
    if (!r) { set_err(out_err, r.error()); return classify_assemble_error(r.error()); }
    auto* arr = convert_instructions(r->instructions);
    if (!arr) return NYXSTONE_ERR_ALLOC;
    *out_ins   = arr;
    *out_ins_n = r->instructions.size();
    if (!r->relocations.empty()) {
        auto* rs = convert_relocations(r->relocations);
        if (!rs) return NYXSTONE_ERR_ALLOC;
        *out_relocs   = rs;
        *out_relocs_n = r->relocations.size();
    }
    return NYXSTONE_OK;
}

nyxstone_status_t nyxstone_disassemble(
    nyxstone_handle_t* h,
    const uint8_t* bytes, size_t bytes_len,
    uint64_t address,
    size_t count,
    char** out_text,
    char** out_err)
{
    if (!h || !out_text) return NYXSTONE_ERR_NULL_ARG;
    *out_text = nullptr;
    std::vector<uint8_t> buf(bytes, bytes + bytes_len);
    auto r = h->inner->disassemble(buf, address, count);
    if (!r) { set_err(out_err, r.error()); return NYXSTONE_ERR_DISASM_FAILED; }
    *out_text = dup_string(*r);
    return *out_text ? NYXSTONE_OK : NYXSTONE_ERR_ALLOC;
}

nyxstone_status_t nyxstone_disassemble_to_instructions(
    nyxstone_handle_t* h,
    const uint8_t* bytes, size_t bytes_len,
    uint64_t address,
    size_t count,
    nyxstone_instruction_t** out, size_t* out_n,
    char** out_err)
{
    if (!h || !out || !out_n) return NYXSTONE_ERR_NULL_ARG;
    *out   = nullptr;
    *out_n = 0;
    std::vector<uint8_t> buf(bytes, bytes + bytes_len);
    auto r = h->inner->disassemble_to_instructions(buf, address, count);
    if (!r) { set_err(out_err, r.error()); return NYXSTONE_ERR_DISASM_FAILED; }
    auto* arr = convert_instructions(*r);
    if (!arr) return NYXSTONE_ERR_ALLOC;
    *out   = arr;
    *out_n = r->size();
    return NYXSTONE_OK;
}

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
