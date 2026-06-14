# Architecture

NyxstoneTricoreGCC is a thin outer driver that calls into gas's TriCore
encoder (`md_assemble`) and libopcodes' `print_insn_tricore` decoder
in-process.  Everything else, the line tokenizer, label handling, data
directive parsing, section-restriction logic, fixup/relax resolution, and
byte extraction, is hand-written C++/C in this repo.

The public C++ API mirrors that of the sibling project
[Nyxstone](https://github.com/emproof-com/nyxstone), a separate codebase
built on LLVM-MC, covering the architectures LLVM supports.  This project
is an independent implementation using GNU binutils to cover TriCore
(which LLVM-MC has no backend for).  Both projects expose six methods --
`assemble`, `assemble_to_instructions`, `assemble_with_relocs`,
`assemble_to_instructions_with_relocs`, `disassemble`, and
`disassemble_to_instructions` -- with the same argument order and the same
`tl::expected<T, std::string>` error channel.  The Rust and Python
bindings present the same shape one level up the stack.

## File map

```
src/nyxstone.cpp          ←─ C++ outer driver (public API impl)
src/nyxstone_glue.c       ←─ C glue: the *only* TU that includes gas headers
src/nyxstone_c.cpp        ←─ C ABI wrapper for Rust/Python bindings

include/nyxstone/nyxstone.h    public C++ API
include/nyxstone/expected.hpp  vendored Sy Brand tl::expected (header-only)
c_api/nyxstone_c.h             public C ABI
```

`nyxstone_glue.c` exposes flat-C functions like `nyxstone_glue_md_assemble`,
`nyxstone_glue_colon`, `nyxstone_glue_resolve_text_fixups`, …  The C++ side
never sees gas's types or macros, which keeps `nyxstone.cpp` pure C++17.

> The diagrams below abbreviate `nyxstone_glue_*` to just `glue_*` for
> alignment; every `glue_xxx()` you see is `nyxstone_glue_xxx()` in the
> actual source.

## Per-call flow (assemble)

```
                                                       │
        ┌──────────────────────────────────────────────▼──────────┐
        │  glue_reset()  +  glue_begin_capture()                  │
        │    symbol_end + symbol_begin (wipe sy_hash)             │
        │    reset frag chains for .text/.data/.bss               │
        │    refresh frag_now / frchain_now / now_seg             │
        │    swap `stderr` for a fopencookie stream so gas        │
        │    diagnostics land in the error string, not the host   │
        └──────────────────────────────────────────────┬──────────┘
                                                       │
        ┌──────────────────────────────────────────────▼──────────┐
        │  Tokenize source                                        │
        │    split on  '\n'  and  ';'  (never inside "...")       │
        │    strip leading WS, # ... / // ... comments            │
        │    (also quote-aware)                                   │
        │    repeatedly strip <ident>: label prefixes →           │
        │       glue_colon(name) / glue_fb_label(n) for digits    │
        └──────────────────────────────────────────────┬──────────┘
                                                       │
              .<dir> ?  yes ─────────────┐             │  no ───┐
                                         │                     │
        ┌────────────────────────────────▼─────┐  ┌────────────▼──────┐
        │ handle_directive(line)               │  │ glue_md_          │
        │   data: .byte, .word, ... → bytes    │  │   assemble(line)  │
        │   via glue_emit_bytes, symbol refs   │  │   (gas's encoder) │
        │   via glue_emit_cons (gas `cons`)    │  └────────────┬──────┘
        │   .align/.p2align/.balign/.org →     │               │
        │   real rs_align / rs_org frags       │               │
        │   (sized post-relax, like gas)       │               │
        │   .equ/.set → glue_set_sym (`equals`)│               │
        │   .text / .section .text*:  no-op    │               │
        │   .data / .bss / .section .foo /     │               │
        │   .pushsection:  set violation flag  │               │
        │   unknown directive: error           │               │
        └────────────────────────────────┬─────┘               │
                                         │                     │
                                         └────────────┬────────┘
                                                      ▼
        ┌──────────────────────────────────────────────────────────┐
        │  Plain path only: glue_define_abs(name, value) for each  │
        │  LabelDefinition.  Runs AFTER parsing: at parse time the │
        │  names look undefined, so gas emits the longest          │
        │  (value-independent) branch forms; mutating the symbols  │
        │  in place before the fixup pass makes references resolve │
        │  (the fixups hold pointers to the parse-time symbolS).   │
        └──────────────────────────────────────────────┬───────────┘
                                                       │
        ┌──────────────────────────────────────────────▼───────────┐
        │  glue_resolve_text_fixups(address)                       │
        │    relax_branches      : seed subtypes (shortest for     │
        │                          local targets, longest for      │
        │                          external/absolute), then        │
        │                          relax_segment passes            │
        │    md_convert_frag     : final opcode bytes per branch   │
        │                          (displacements stay as fixups)  │
        │    finalize_align_org  : size rs_align/rs_org exactly,   │
        │                          error on `.org` backwards       │
        │    layout()            : final fr_address (fill-aware)   │
        │    apply_text_fixups   : re-resolve symbols, encode      │
        │                          displacements / absolute fields,│
        │                          range-check (error if too far)  │
        │    → unresolved count  : >0 in the plain path = error    │
        │                          listing the undefined labels    │
        └──────────────────────────────────────────────┬───────────┘
                                                       │
        ┌──────────────────────────────────────────────▼───────────┐
        │  glue_end_capture()  →  diagnostics into error string    │
        │  glue_extract_text_bytes()                               │
        │    walk frchain → frag_root → fr_next                    │
        │    for in-progress tail frag use frag_now_fix()          │
        │    emit fr_literal bytes + align/org fill repetitions    │
        └──────────────────────────────────────────────────────────┘
```

## Per-call flow (disassemble)

```
        repeat over the input buffer:
        ┌────────────────────────────────────────────────────────┐
        │ glue_disasm_one(bytes+offset, len-offset, addr)        │
        │   wraps libopcodes' print_insn_tricore                 │
        │   capture text into a heap-allocated buffer            │
        │   returns 2 or 4 (bytes consumed)                      │
        └────────────────────────────────────────────────────────┘
        accumulate Instruction { address, assembly, bytes } records.
```

## What we *don't* call from gas

To stay fast and small, we skip every per-pass init that isn't
strictly needed:

| Skipped | Reason |
|---|---|
| `eh_begin` / CFI machinery       | No `.cfi_*` directives exposed |
| `dwarf2_init` / DWARF emission   | No debug info needed for the byte stream |
| `macro_init` / `.macro` / `.include` | Nyxstone doesn't expose macros |
| `input_scrub_begin` / `read_a_source_file` | We parse the source ourselves |
| `output_file_create` / `bfd_close` over a file | BFD is in-memory; we never write the ELF |
| `listing_*`, codeview, stabs, sframe | Not used by Nyxstone callers |
| `write_object_file`              | We do the layout + apply_fix subset by hand |

## Relocation extraction (`assemble_with_relocs` / `assemble_to_instructions_with_relocs`)

The `*_with_relocs` API path produces the same shape gas emits when run
with `-r`: a byte stream with reloc fields left as zero placeholders, plus
a list of `RelocationInfo` records describing every unresolved external
symbol reference.

Note the asymmetry with the plain path: the "undefined label is an error"
rule applies only to `assemble` / `assemble_to_instructions` (where nobody
will ever patch the bytes again).  In the relocs path an undefined symbol
is the expected case -- it is what becomes a relocation record for the
linker -- so no `LabelDefinition` is required at all; a matching entry only
fills the `symbol.address` hint field.

How this differs from plain `assemble`:

| stage | `assemble` | `assemble_with_relocs` |
|---|---|---|
| label definition | `glue_define_abs(name, value)` after parsing for every LabelDefinition (absolute symbols) | none, labels stay undefined |
| fixup pass | every fix resolves; leftover undefined symbols are an error | undefined symbol fixes left at fx_done=0 |
| reloc collection | n/a | walks `text_section`'s `frchain.fix_root`, emits one entry per `fx_done==0 && fx_addsy` |

For each unresolved fix the glue produces:
- `offset = fx_frag->fr_address + fx_where` (section-relative)
- `addend = fx_addnumber` (gas's RELA-style addend, set by md_apply_fix)
- `relocation_type = bfd_reloc_type_lookup(stdoutput, fx_r_type)->type`
  (the ELF `R_TRICORE_*` value, e.g. 3 for `R_TRICORE_24REL`)
- `symbol.name = S_GET_NAME(fx_addsy)`
- `symbol.address` is filled in by the C++ side from the matching
  LabelDefinition (or left 0 if the caller didn't supply one)

## What we *do* call from gas

| Used | What it does |
|---|---|
| `bfd_create` + `bfd_make_writable` | In-memory BFD; no file ever touched |
| `subseg_new` / `subseg_set`        | Create / switch sections (always .text) |
| `symbol_begin`/`symbol_end`/`colon`/`resolve_symbol_value` | Local label support |
| `symbol_find`/`symbol_new`/`symbol_table_insert` | LabelDefinition → absolute symbols |
| `fb_label_instance_inc`/`fb_label_name` | Numeric local labels (`1:` / `1b` / `1f`) |
| `equals` (read.c)                  | `.equ`/`.set` with gas's full expression grammar |
| `cons` (read.c)                    | `.word label` etc.: data lists with symbol refs |
| `frag_init`/`subsegs_begin`/`frag_alloc`/`frag_more`/`frag_now_fix` | Frag chain |
| `frag_align`/`frag_var(rs_org)`    | `.align`/`.org` as real frags, sized post-relax |
| `relax_segment`                    | Generic relaxer (branch growth, align/org sizing) |
| `read_begin`/`expr_begin`          | Expression evaluator (used by md_assemble) |
| `md_begin` (TriCore)               | Build `hash_ops`/`hash_sfr`/`pseudo_codes` |
| `md_assemble` (TriCore)            | **The encoder.** Bit-packs operands. |
| `md_estimate_size_before_relax`    | Pick the size class for a relaxable frag |
| `md_convert_frag`                  | Emit final bytes for a relaxable frag |
| `md_apply_fix` / `md_pcrel_from_section` | Resolve PC-relative branches |
| `as_bad` (via stderr capture)      | Diagnostics; captured into the error string |
| `bfd_reloc_type_lookup` (libbfd) | Map `fx_r_type` → ELF `R_TRICORE_*` for `assemble_with_relocs` output |

## Section restriction (`.text`-only)

A `static bool g_section_violation` flag is cleared at the top of
`assemble()` and set by `handle_directive()` whenever it sees a directive
that would switch to a non-`.text` section.  After the source has been
fully processed, `assemble()` checks the flag; if it's set, the function
returns `nullopt` instead of returning whatever partial bytes accumulated.

Accepted:
- `.text`             , already on .text, no-op
- `.section .text`    , equivalent
- `.section .text.foo`, accepted (subsection names like `.text.startup`)

Rejected (always returns `nullopt`):
- `.data` / `.bss` / `.rodata` / `.sdata` / `.sbss` / `.tdata` / `.tbss`
- `.section <anything not .text*>`
- `.pushsection` / `.previous`
