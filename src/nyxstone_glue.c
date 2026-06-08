/* nyxstone_glue: tiny C glue layer that includes gas's internal headers and
   exposes a flat C API to the C++ implementation in nyxstone.cpp.

   This file is the only C translation unit in the project that touches gas
   internals directly (md_assemble, the symbol table, the frag chain, BFD).
   Everything else stays in plain C++17.

   Per-call architecture:
     1. nyxstone_glue_reset()             , wipe symbol table + frag chain.
     2. nyxstone_glue_colon(name)         , define a label at the current PC.
     3. nyxstone_glue_md_assemble(line)   , run gas's TriCore encoder; bytes
                                        land in the in-memory frag chain.
     4. nyxstone_glue_emit_bytes(p, n)    , raw bytes for data directives.
     5. nyxstone_glue_resolve_text_fixups()
                                     , run a mini relax pass + apply
                                        every fix; finalize relax-frag
                                        bytes and PC-relative branches.
     6. nyxstone_glue_extract_text_bytes(), concat the resulting frag bytes.

   Disassembly is independent of the assembler state, it just calls into
   libopcodes' print_insn_tricore.

   See ../include/nyxstone/nyxstone.h for the public C++ API contract. */

/* gas insists config.h is included before any system header. */
#include "config.h"

#include "as.h"
#include "subsegs.h"     /* frchainS, segment_info_type, seg_info */
#include "obstack.h"     /* _obstack_free etc. */
/* symbols.h, frags.h, read.h are pulled in by as.h; including them
   explicitly causes enum-redeclaration errors (no include guards).  */
extern symbolS *colon (const char *);
extern void     symbol_end (void);
extern void     symbol_begin (void);

/* libopcodes' disassembler API.  Lives in `binutils-src/include/dis-asm.h`. */
#include "dis-asm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

extern void md_begin (void);
extern void md_assemble (char *);
extern void md_apply_fix (fixS *, valueT *, segT);
extern long md_pcrel_from_section (fixS *, segT);
extern int  md_estimate_size_before_relax (fragS *, segT);
extern void md_convert_frag (bfd *, segT, fragS *);
extern fragS *frag_alloc (struct obstack *);

extern int    print_insn_tricore (unsigned long, struct disassemble_info *);

static int g_initialized = 0;

/* ----- 1. One-time process init ---------------------------------------- */
int nyxstone_glue_init_once (void)
{
    if (g_initialized) return 0;

    if (bfd_init () != BFD_INIT_MAGIC) return 1;
    obstack_begin (&notes, 0);

    symbol_begin ();
    frag_init ();
    subsegs_begin ();
    read_begin ();
    expr_begin ();
    /* Intentionally NOT called: eh_begin, dwarf2_init, macro_init,
       input_scrub_begin (no parser-driven input), listing_init. */

    /* bfd_create with NULL template leaves xvec unset, which would crash
       bfd_set_arch_mach.  Look up the elf32-tricore target vector explicitly
       and use a stub bfd as the template. */
    {
        const bfd_target *tvec = bfd_find_target ("elf32-tricore", NULL);
        if (!tvec) return 2;
        bfd templ;
        memset (&templ, 0, sizeof (templ));
        templ.xvec = tvec;
        stdoutput = bfd_create ("nyxstone-tricore", &templ);
        if (!stdoutput) return 2;
    }
    bfd_make_writable (stdoutput);
    bfd_set_format (stdoutput, bfd_object);
    bfd_set_arch_mach (stdoutput, bfd_arch_tricore, 0);

    text_section = subseg_new (".text", 0);
    data_section = subseg_new (".data", 0);
    bss_section  = subseg_new (".bss",  0);
    subseg_new (BFD_ABS_SECTION_NAME, 0);
    subseg_new (BFD_UND_SECTION_NAME, 0);
    reg_section  = subseg_new ("*reg*",  0);
    expr_section = subseg_new ("*expr*", 0);

    subseg_set (text_section, 0);

    md_begin ();   /* populates hash_ops / hash_sfr / pseudo_codes */

    g_initialized = 1;
    return 0;
}

/* ----- 2. Per-call reset ----------------------------------------------- */
static void reset_section (segT seg)
{
    segment_info_type *si = seg_info (seg);
    if (!si) return;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        _obstack_free (&fc->frch_obstack, NULL);
        obstack_begin (&fc->frch_obstack, 0);
#if __GNUC__ >= 2
        obstack_alignment_mask (&fc->frch_obstack) = __alignof__ (fragS) - 1;
#endif
        fragS *fr = frag_alloc (&fc->frch_obstack);
        fr->fr_type = rs_fill;
        fc->frch_root     = fr;
        fc->frch_last     = fr;
        fc->frch_frag_now = fr;
        fc->fix_root      = NULL;
        fc->fix_tail      = NULL;
    }
}

void nyxstone_glue_reset (void)
{
    symbol_end ();
    symbol_begin ();
    reset_section (text_section);
    reset_section (data_section);
    reset_section (bss_section);

    /* subseg_set is a no-op when now_seg is already text_section, so it
       won't refresh frag_now/frchain_now to the new fresh frag we just
       created.  Update both globals by hand, matching subseg_get's contract. */
    segment_info_type *si = seg_info (text_section);
    if (si && si->frchainP) {
        frchain_now = si->frchainP;
        frag_now    = si->frchainP->frch_frag_now;
        now_seg     = text_section;
        now_subseg  = 0;
    }
}

/* ----- 3. Forwarders into gas ------------------------------------------ */
void nyxstone_glue_colon (const char *name)
{
    colon (name);
}

void nyxstone_glue_md_assemble (char *line)
{
    md_assemble (line);
}

void nyxstone_glue_emit_bytes (const uint8_t *p, size_t n)
{
    if (!n) return;
    char *dst = frag_more ((int) n);
    memcpy (dst, p, n);
}

size_t nyxstone_glue_frag_now_fix (void)
{
    return (size_t) frag_now_fix ();
}

int nyxstone_glue_had_errors (void)
{
    return had_errors ();
}

/* ----- 4. Layout + relax + fixup pass ---------------------------------- */
/* Layout pass: walks the frag chain and writes each frag's fr_address as
   its cumulative byte offset.  gas's write_object_file normally does this
   in size_seg()/relax_segment(). */
static void layout (segT seg)
{
    segment_info_type *si = seg_info (seg);
    if (!si) return;
    addressT addr = 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            fr->fr_address = addr;
            size_t n = (fr == fc->frch_last && fr == frag_now)
                       ? (size_t) frag_now_fix ()
                       : (size_t) fr->fr_fix;
            addr += n;
        }
    }
}

/* Finalize relax frags (rs_machine_dependent).  gas's write_object_file
   does this in relax_segment + size_segs; we do a minimum subset by calling
   md_estimate_size_before_relax to pick the initial subtype, then
   md_convert_frag to emit the final bytes.  We do NOT iterate; the relax
   pass would normally re-run if frag size changes affect symbol distances,
   but for the Nyxstone usage (typically a single instruction or a small
   basic block) the initial subtype is correct. */
static void finalize_relax (segT seg)
{
    segment_info_type *si = seg_info (seg);
    if (!si) return;
    addressT addr = 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            fr->fr_address = addr;
            if (fr->fr_type == rs_machine_dependent) {
                (void) md_estimate_size_before_relax (fr, seg);
                md_convert_frag (stdoutput, seg, fr);
            }
            size_t n = (fr == fc->frch_last && fr == frag_now)
                       ? (size_t) frag_now_fix ()
                       : (size_t) fr->fr_fix;
            addr += n;
        }
    }
}

int nyxstone_glue_resolve_text_fixups (void)
{
    layout (text_section);
    finalize_relax (text_section);
    layout (text_section);

    segment_info_type *si = seg_info (text_section);
    if (!si) return 0;
    int unresolved = 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        for (fixS *fx = fc->fix_root; fx; fx = fx->fx_next) {
            if (!fx->fx_addsy) continue;
            resolve_symbol_value (fx->fx_addsy);
            valueT sym_val = S_GET_VALUE (fx->fx_addsy);
            valueT val = sym_val + fx->fx_offset;
            if (fx->fx_pcrel) val -= md_pcrel_from_section (fx, text_section);
            md_apply_fix (fx, &val, text_section);
            if (fx->fx_addsy && !fx->fx_done) ++unresolved;
        }
    }
    return unresolved;
}

/* ----- 4b. Relocation extraction (gcc/gas "-r" equivalent) ------------- */
/* After nyxstone_glue_resolve_text_fixups has run md_apply_fix on every fix in
   text_section, undefined-symbol fixes are left with fx_done == 0 and
   their addend recorded in fx_addnumber (see tc-tricore.c md_apply_fix +
   tc_gen_reloc).  This function walks the same fix chain and reports one
   entry per unresolved fix, equivalent to what gas emits in the ELF .o
   when invoked with `-r`.

   The returned `symbol_name` pointer aliases gas's internal symtab; it
   stays valid until the next nyxstone_glue_reset().  The C++ caller copies
   into std::string before the next reset.

   Pass `out == NULL` (and `cap == 0`) to query the required size. */
typedef struct {
    uint64_t     offset;        /* section-relative offset of the reloc */
    int64_t      addend;        /* gas's fx_addnumber, the ELF rela addend */
    int          has_addend;    /* always 1 for TriCore (RELA) */
    const char  *symbol_name;   /* points into gas symtab; copy immediately */
    uint32_t     reloc_type;    /* ELF R_TRICORE_* value */
} nyxstone_glue_reloc_t;

size_t nyxstone_glue_collect_relocs (nyxstone_glue_reloc_t *out, size_t cap)
{
    size_t count = 0;
    segment_info_type *si = seg_info (text_section);
    if (!si) return 0;

    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        for (fixS *fx = fc->fix_root; fx; fx = fx->fx_next) {
            if (fx->fx_done)      continue;
            if (!fx->fx_addsy)    continue;
            /* tc-tricore's md_apply_fix leaves fx_done == 0 even for fixes
               that the relax pass already wrote into the bytes (it just
               records the addend and returns).  We only want relocs for
               symbols that the linker would actually need to resolve,
               i.e. undefined symbols.  Defined-in-section symbols had
               their displacement written by md_convert_frag earlier. */
            if (S_IS_DEFINED (fx->fx_addsy)) continue;

            if (out && count < cap) {
                out[count].offset     = (uint64_t) (fx->fx_frag->fr_address
                                                    + (offsetT) fx->fx_where);
                out[count].addend     = (int64_t)  fx->fx_addnumber;
                out[count].has_addend = 1;
                out[count].symbol_name = S_GET_NAME (fx->fx_addsy);

                reloc_howto_type *howto =
                    bfd_reloc_type_lookup (stdoutput, fx->fx_r_type);
                out[count].reloc_type = howto
                    ? (uint32_t) howto->type
                    : 0u;
            }
            ++count;
        }
    }
    return count;
}

/* ----- 5. Byte extraction ---------------------------------------------- */
/* Walk text_section's frag chain.  For closed frags use fr_fix (final).
   For the in-progress tail frag use frag_now_fix() (live obstack growth).
   Without the latter, the bytes of the only/last insn in a one-line input
   would be missed (their fr_fix is still 0 because frag_new never closed
   the frag). */
size_t nyxstone_glue_extract_text_bytes (uint8_t *out, size_t cap)
{
    size_t total = 0;
    segment_info_type *si = seg_info (text_section);
    if (!si) return 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            size_t n = (fr == fc->frch_last && fr == frag_now)
                       ? (size_t) frag_now_fix ()
                       : (size_t) fr->fr_fix;
            if (n == 0) continue;
            if (out && total + n <= cap)
                memcpy (out + total, fr->fr_literal, n);
            total += n;
        }
    }
    return total;
}

/* ----- 6. Disassembly via libopcodes' print_insn_tricore --------------- */
/* The libopcodes disassembler emits text through a printf-like callback
   passed in via `disassemble_info`.  We capture into a heap-allocated
   buffer that the C++ side reads. */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} dis_ctx_t;

static void dis_ensure_cap (dis_ctx_t *c, size_t want) {
    if (c->cap >= want) return;
    size_t nc = c->cap ? c->cap : 64;
    while (nc < want) nc *= 2;
    char *nb = (char *) realloc (c->buf, nc);
    if (!nb) return;
    c->buf = nb;
    c->cap = nc;
}

static int dis_printf (void *s, const char *fmt, ...) {
    dis_ctx_t *c = (dis_ctx_t *) s;
    char tmp[256];
    va_list ap;
    va_start (ap, fmt);
    int n = vsnprintf (tmp, sizeof (tmp), fmt, ap);
    va_end (ap);
    if (n <= 0) return n;
    size_t add = (size_t) (n < (int) sizeof (tmp) ? n : (int) sizeof (tmp) - 1);
    dis_ensure_cap (c, c->len + add + 1);
    if (c->buf) {
        memcpy (c->buf + c->len, tmp, add);
        c->len += add;
        c->buf[c->len] = 0;
    }
    return n;
}

static int dis_printf_styled (void *s, enum disassembler_style style, const char *fmt, ...) {
    (void) style;
    dis_ctx_t *c = (dis_ctx_t *) s;
    char tmp[256];
    va_list ap;
    va_start (ap, fmt);
    int n = vsnprintf (tmp, sizeof (tmp), fmt, ap);
    va_end (ap);
    if (n <= 0) return n;
    size_t add = (size_t) (n < (int) sizeof (tmp) ? n : (int) sizeof (tmp) - 1);
    dis_ensure_cap (c, c->len + add + 1);
    if (c->buf) {
        memcpy (c->buf + c->len, tmp, add);
        c->len += add;
        c->buf[c->len] = 0;
    }
    return n;
}

/* Print a branch/call target address WITHOUT the zero-padding that the
   default generic_print_address (bfd_sprintf_vma -> "%08lx") applies.
   Mirrors objdump's objdump_print_value, which strips leading zeros, so
   we emit "j 0x1068" instead of "j 0x00001068".  Routes through the same
   fprintf_func so the text lands in the same output buffer. */
static void dis_print_address (bfd_vma addr, struct disassemble_info *info) {
    (*info->fprintf_func) (info->stream, "0x%" PRIx64, (uint64_t) addr);
}

/* Disassemble one instruction starting at byte offset 0 of `bytes`/`len`.
   Returns the number of bytes consumed (2 or 4), or <=0 on failure.  On
   success, *text_out points to a freshly malloc'd null-terminated string
   (caller frees).  The bytes_out parameter is ignored, caller already has
   the bytes; we just consume `n` of them. */
int nyxstone_glue_disasm_one (const uint8_t *bytes, size_t len, uint64_t addr,
                         char **text_out, size_t *n_consumed)
{
    if (!bytes || !text_out || !n_consumed) return -1;
    *text_out = NULL;
    *n_consumed = 0;

    dis_ctx_t ctx = { NULL, 0, 0 };
    struct disassemble_info info;
    INIT_DISASSEMBLE_INFO (info, &ctx,
        (fprintf_ftype) &dis_printf,
        (fprintf_styled_ftype) &dis_printf_styled);
    info.arch          = bfd_arch_tricore;
    info.mach          = 0x00100000;     /* EF_EABI_TRICORE_V1_6_2 */
    info.buffer        = (bfd_byte *) bytes;
    info.buffer_vma    = addr;
    info.buffer_length = len;
    info.read_memory_func = buffer_read_memory;
    /* Override the default generic_print_address so branch/call targets are
       printed without zero-padding, matching objdump (e.g. "j 0x1068"). */
    info.print_address_func = dis_print_address;

    int n = print_insn_tricore ((unsigned long) addr, &info);
    if (n <= 0) {
        free (ctx.buf);
        return n;
    }
    *text_out = ctx.buf ? ctx.buf : strdup ("");
    *n_consumed = (size_t) n;
    return n;
}
