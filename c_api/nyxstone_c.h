// NyxstoneTricoreGCC, C ABI for non-C++ language bindings (Rust, Python,
// etc.).  Wraps the C++ NyxstoneTricoreGCC class.
//
// API shape mirrors the sibling project Nyxstone
// (https://github.com/emproof-com/nyxstone, LLVM-MC based): six entry
// points named `assemble`, `assemble_to_instructions`,
// `assemble_with_relocs`, `assemble_to_instructions_with_relocs`,
// `disassemble`, and `disassemble_to_instructions` (the two `_with_relocs`
// variants are NyxstoneTricoreGCC extensions), all taking an explicit
// `address` and (for the assembly entry points) an optional array of
// `LabelDefinition`s.
//
// All "handle" types are opaque pointers.  All buffers returned to the
// caller are heap-allocated; free them with the matching `nyxstone_free_*`
// helper.
//
// Error reporting contract: every function taking an `out_err` parameter
// sets *out_err to NULL on entry (when out_err itself is non-NULL).  On
// failure, *out_err is set to a malloc'd error string when allocation
// permits (it may stay NULL under memory exhaustion); free it via
// nyxstone_free_string.  All other failure paths -- including NULL-argument
// rejection -- set a message too.
//
// Output contract: on any failure return, all output parameters are
// NULL / 0; nothing is handed to the caller that would need freeing.
//
// Pointer/length pairs: every (pointer, length) input pair -- `labels` /
// `labels_len` and `bytes` / `bytes_len` -- accepts a NULL pointer if and
// only if the length is 0 (treated as empty input); a NULL pointer with a
// non-zero length yields NYXSTONE_ERR_NULL_ARG.
//
// No C++ exception ever crosses this ABI: internal exceptions (e.g.
// std::bad_alloc) are caught and reported as NYXSTONE_ERR_ALLOC (or, for
// nyxstone_create, a NULL return) with *out_err carrying the message.
//
// Threading: single-threaded only.  All gas globals are process-wide; do
// not hold two NyxstoneTricoreGCC handles concurrently from different threads.

#ifndef NYXSTONE_C_H
#define NYXSTONE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct nyxstone_handle nyxstone_handle_t;

/* Return codes. */
typedef enum {
    NYXSTONE_OK                   = 0,
    NYXSTONE_ERR_INIT             = 1,   /* libbfd init or target lookup failed. */
    NYXSTONE_ERR_NULL_ARG         = 2,
    NYXSTONE_ERR_ASSEMBLE_FAILED  = 3,   /* gas parse / encode error. */
    NYXSTONE_ERR_SECTION_VIOLATION= 4,   /* non-.text section directive. */
    NYXSTONE_ERR_DISASM_FAILED    = 5,   /* libopcodes couldn't decode buffer. */
    NYXSTONE_ERR_ALLOC            = 6
} nyxstone_status_t;

/* External label definition (input to assemble/_to_instructions). */
typedef struct {
    const char* name;
    uint64_t    address;
} nyxstone_label_def_t;

/* Disassembled instruction record (output). */
typedef struct {
    uint64_t  address;
    char*     assembly;     /* null-terminated; freed by nyxstone_free_instructions. */
    uint8_t*  bytes;        /* freed by nyxstone_free_instructions. */
    size_t    bytes_len;
} nyxstone_instruction_t;

/* Symbol target of a relocation. */
typedef struct {
    char*    name;       /* null-terminated; freed by nyxstone_free_relocations. */
    uint64_t address;    /* resolved address from matching LabelDefinition, or 0. */
} nyxstone_reloc_symbol_t;

/* One relocation entry (gcc/gas "-r" equivalent). */
typedef struct {
    uint64_t            offset;          /* section-relative reloc offset */
    int64_t             addend;          /* ELF rela addend (valid if has_addend) */
    int                 has_addend;      /* 1 for TriCore (RELA); 0 otherwise */
    nyxstone_reloc_symbol_t  symbol;
    uint32_t            relocation_type; /* ELF R_TRICORE_* */
} nyxstone_reloc_t;

/* --- Lifecycle ----------------------------------------------------------- */

/* Create a NyxstoneTricoreGCC handle.  Returns NULL on init failure (e.g. libbfd
   couldn't find elf32-tricore).  If `out_err` is non-NULL and creation
   fails, *out_err is set to a malloc'd error message (free with
   nyxstone_free_string). */
nyxstone_handle_t* nyxstone_create(char** out_err);

/* Free a NyxstoneTricoreGCC handle.  Safe to call with NULL. */
void nyxstone_destroy(nyxstone_handle_t* h);

/* --- Assembly ------------------------------------------------------------ */

/* Assemble `source` at the given absolute `address`, with optional
   external label definitions.

   `source` need not be null-terminated; `src_len` bytes are read.
   `labels` may be NULL only when `labels_len == 0`; a NULL `labels` with
   `labels_len > 0` returns NYXSTONE_ERR_NULL_ARG.

   On success, *out_bytes is a malloc'd buffer of *out_len bytes; free via
   nyxstone_free_bytes.  If `source` assembles to zero bytes, the call
   succeeds with *out_bytes == NULL and *out_len == 0.  On failure,
   *out_bytes is NULL, *out_len is 0, and (if `out_err` is non-NULL)
   *out_err carries a malloc'd error string. */
nyxstone_status_t nyxstone_assemble(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    uint8_t** out_bytes, size_t* out_len,
    char** out_err);

/* Like nyxstone_assemble but returns one nyxstone_instruction_t per encoded insn
   (assembly text comes from libopcodes round-tripping the bytes).  Free
   the array with nyxstone_free_instructions(arr, *out_n). */
nyxstone_status_t nyxstone_assemble_to_instructions(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    nyxstone_instruction_t** out, size_t* out_n,
    char** out_err);

/* Like nyxstone_assemble but ALSO returns one nyxstone_reloc_t per unresolved
   external label reference, equivalent to `gas -r` output.  The label
   references in `source` to names listed in `labels` (or otherwise
   undefined) are left as zero placeholders in `out_bytes`; their
   resolution is described by the `out_relocs` array.  Free
   `out_bytes` with nyxstone_free_bytes and `out_relocs` with
   nyxstone_free_relocations(arr, *out_relocs_n).

   Outputs are all-or-nothing: on failure both `out_bytes` and `out_relocs`
   are NULL (lengths 0); the caller never has to free a partial result. */
nyxstone_status_t nyxstone_assemble_with_relocs(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    uint8_t** out_bytes, size_t* out_bytes_len,
    nyxstone_reloc_t** out_relocs, size_t* out_relocs_n,
    char** out_err);

/* Like nyxstone_assemble_to_instructions but with `-r`-style relocation
   output (see nyxstone_assemble_with_relocs). */
nyxstone_status_t nyxstone_assemble_to_instructions_with_relocs(
    nyxstone_handle_t* h,
    const char* source, size_t src_len,
    uint64_t address,
    const nyxstone_label_def_t* labels, size_t labels_len,
    nyxstone_instruction_t** out_ins, size_t* out_ins_n,
    nyxstone_reloc_t** out_relocs, size_t* out_relocs_n,
    char** out_err);

/* --- Disassembly --------------------------------------------------------- */

/* Disassemble `bytes` starting at absolute `address`.  Decodes at most
   `count` instructions; pass 0 for "all".

   `bytes` may be NULL only when `bytes_len == 0` (treated as empty input);
   a NULL `bytes` with `bytes_len > 0` returns NYXSTONE_ERR_NULL_ARG.

   On success, *out_text is a malloc'd null-terminated string with one
   instruction per line.  Free via nyxstone_free_string. */
nyxstone_status_t nyxstone_disassemble(
    nyxstone_handle_t* h,
    const uint8_t* bytes, size_t bytes_len,
    uint64_t address,
    size_t count,
    char** out_text,
    char** out_err);

/* Same as nyxstone_disassemble but returns an array of nyxstone_instruction_t.
   Free via nyxstone_free_instructions(arr, *out_n). */
nyxstone_status_t nyxstone_disassemble_to_instructions(
    nyxstone_handle_t* h,
    const uint8_t* bytes, size_t bytes_len,
    uint64_t address,
    size_t count,
    nyxstone_instruction_t** out, size_t* out_n,
    char** out_err);

/* --- Free helpers -------------------------------------------------------- */

void nyxstone_free_bytes(uint8_t* p);
void nyxstone_free_string(char* p);
void nyxstone_free_instructions(nyxstone_instruction_t* p, size_t n);
void nyxstone_free_relocations(nyxstone_reloc_t* p, size_t n);

#ifdef __cplusplus
}
#endif

#endif  /* NYXSTONE_C_H */
