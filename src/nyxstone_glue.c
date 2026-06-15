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
extern int  relax_segment (fragS *, segT, int);
extern void cons (int nbytes);           /* gas .byte/.word/... emitter */
extern char *input_line_pointer;         /* gas's current parse cursor */

/* TriCore's branch relax chains (md_relax_table).  Each rs_machine_dependent
   branch frag carries a subtype indexing this table; rlx_more links a subtype
   to the next *longer* encoding (0 = terminal).  The table is part of the
   pinned prebuilt gas (third_party/.../tc-tricore.o, 888 bytes / 24 = 37
   entries); regenerating binutils means revisiting MD_RELAX_TABLE_N. */
extern const relax_typeS md_relax_table[];
#define MD_RELAX_TABLE_N 37

extern int    print_insn_tricore (unsigned long, struct disassemble_info *);

static int g_initialized = 0;

/* ----- gas stderr capture ----------------------------------------------- */
/* gas reports every diagnostic through as_bad/as_warn, which print to the
   stderr FILE stream (messages.c).  A library must not spam the host
   process's stderr, and the caller needs the text to produce an actionable
   error.  While a capture is active we swap the `stderr` global for a
   fopencookie(3) stream that appends into a growable buffer (glibc-specific,
   like the rest of the Linux-only daemon machinery).  Single-threaded by
   contract (see nyxstone.h), so the global swap is safe. */

typedef struct { char *buf; size_t len; size_t cap; } growbuf_t;

static void growbuf_append (growbuf_t *g, const char *data, size_t n)
{
    if (g->len + n + 1 > g->cap) {
        size_t nc = g->cap ? g->cap : 256;
        while (nc < g->len + n + 1) nc *= 2;
        char *nb = (char *) realloc (g->buf, nc);
        if (!nb) return;
        g->buf = nb;
        g->cap = nc;
    }
    memcpy (g->buf + g->len, data, n);
    g->len += n;
    g->buf[g->len] = 0;
}

static growbuf_t g_caplog;
static FILE *g_cap_stream    = NULL;
static FILE *g_saved_stderr  = NULL;

static ssize_t cap_write (void *cookie, const char *data, size_t n)
{
    growbuf_append ((growbuf_t *) cookie, data, n);
    return (ssize_t) n;
}

void nyxstone_glue_begin_capture (void)
{
    g_caplog.len = 0;
    if (g_caplog.buf) g_caplog.buf[0] = 0;
    if (!g_cap_stream) {
        cookie_io_functions_t io = { NULL, cap_write, NULL, NULL };
        g_cap_stream = fopencookie (&g_caplog, "w", io);
        if (g_cap_stream) setvbuf (g_cap_stream, NULL, _IONBF, 0);
    }
    if (g_cap_stream && !g_saved_stderr) {
        g_saved_stderr = stderr;
        stderr = g_cap_stream;
    }
}

const char *nyxstone_glue_end_capture (void)
{
    if (g_saved_stderr) {
        fflush (g_cap_stream);
        stderr = g_saved_stderr;
        g_saved_stderr = NULL;
    }
    return g_caplog.buf ? g_caplog.buf : "";
}

/* Raise a gas error from the C++ driver (directive validation etc.) so all
   failures flow through one channel: the error counter + captured text. */
void nyxstone_glue_error (const char *msg)
{
    as_bad ("%s", msg);
}

/* Name of an unsupported relocation hit while encoding the last assemble's
   local branch displacements (NULL if all were handled).  Aliases bfd's static
   howto name string, valid until overwritten.  Read via
   nyxstone_glue_unsupported_reloc(); the C++ layer turns it into an error. */
static const char *g_unsupported_reloc = NULL;

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

/* Define `name` as an absolute symbol holding the full target address.
   Used for the caller's LabelDefinitions in the plain assemble path.

   Called AFTER the source has been parsed (deliberately): at parse time the
   name is an undefined symbol, so gas emits the longest, value-independent
   encoding for branches to it (and relax_branches keeps it there) -- the
   bytes don't change with the absolute `address` parameter.  Mutating the
   symbol here, before the fixup pass, is what makes the references resolve:
   the fixups hold pointers to the parse-time symbolS, so the existing
   object must be redefined in place (a fresh symbol_new would leave
   fx_addsy dangling at the undefined one).  apply_text_fixups then encodes
   PC-relative refs as value - (base + PC) and absolute refs as the value. */
void nyxstone_glue_define_abs (const char *name, uint64_t value)
{
    symbolS *sym = symbol_find (name);
    if (sym) {
        if (S_IS_DEFINED (sym)) {
            as_bad ("label '%s' is defined both in the source and as a "
                    "LabelDefinition", name);
            return;
        }
        S_SET_SEGMENT (sym, absolute_section);
        S_SET_VALUE (sym, (valueT) value);
    } else {
        sym = symbol_new (name, absolute_section,
                          &zero_address_frag, (valueT) value);
        symbol_table_insert (sym);
    }
}

/* `.equ name, expr` / `.set name, expr`: route through gas's own `equals`
   (read.c) so the full expression grammar works (constants, labels,
   arithmetic).  `equals` expects input_line_pointer to sit ON the `=`. */
void nyxstone_glue_set_sym (const char *name, const char *value_expr)
{
    size_t nlen = strlen (name);
    size_t vlen = strlen (value_expr);
    char *nbuf = (char *) malloc (nlen + 1);
    char *vbuf = (char *) malloc (vlen + 3);
    if (!nbuf || !vbuf) { free (nbuf); free (vbuf); return; }
    memcpy (nbuf, name, nlen + 1);
    vbuf[0] = '=';
    memcpy (vbuf + 1, value_expr, vlen);
    vbuf[vlen + 1] = '\n';
    vbuf[vlen + 2] = '\0';

    char *saved = input_line_pointer;
    input_line_pointer = vbuf;
    equals (nbuf, 1 /* allow reassignment, .set semantics */);
    input_line_pointer = saved;
    free (nbuf);
    free (vbuf);
}

/* Numeric local label definition (`1:`).  gas implements these as "fb"
   labels: each definition bumps an instance counter and defines a mangled
   per-instance symbol; `1b`/`1f` references inside operands are resolved
   by gas's expression parser against the same counter (expr.c). */
void nyxstone_glue_fb_label (unsigned int n)
{
    fb_label_instance_inc (n);
    colon (fb_label_name (n, 0));
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

/* Emit a `.byte/.hword/.word/.quad`-style data list whose operands may contain
   symbols or expressions (e.g. `.word label`, `.word end-start`), by handing
   the operand string to gas's `cons`.  gas parses the comma-separated
   expressions, emits `nbytes` little-endian bytes each, and creates fixups for
   symbolic terms -- which the normal fixup pass then resolves (local) or
   records as relocations (external).  Pure-integer lists are handled by the
   faster path in the C++ layer; this is only used when a symbol is present. */
void nyxstone_glue_emit_cons (const char *args, int nbytes)
{
    if (!args) return;
    size_t len = strlen (args);
    char *buf = (char *) malloc (len + 2);
    if (!buf) return;
    memcpy (buf, args, len);
    buf[len]     = '\n';     /* statement terminator cons() stops at */
    buf[len + 1] = '\0';

    char *saved = input_line_pointer;
    input_line_pointer = buf;
    cons (nbytes);           /* emits bytes + fixups into the current frag */
    input_line_pointer = saved;
    free (buf);
}

size_t nyxstone_glue_frag_now_fix (void)
{
    return (size_t) frag_now_fix ();
}

/* `.align p2[, fill[, max]]`: emit a real rs_align frag (gas frag_align) so
   the padding participates in relaxation.  Padding emitted at parse time
   would bake in pre-relax offsets: a preceding branch frag that later
   shrinks (or an offset measured against the wrong frag) silently
   misaligns everything after it -- this was the historical bug.  The frag
   is sized exactly in finalize_align_org() once all branch sizes are
   final. */
void nyxstone_glue_align (unsigned int p2, int fill, unsigned int max)
{
    frag_align ((int) p2, fill, (int) max);
}

/* `.org target[, fill]`: rs_org frag; target is the section-relative
   offset.  Sized in finalize_align_org(), which raises a gas error if the
   target lies behind the current offset (matching gas's "attempt to move
   .org backwards"). */
void nyxstone_glue_org (uint64_t target, int fill)
{
    char *p = frag_var (rs_org, 1, 1, (relax_substateT) 0,
                        (symbolS *) NULL, (offsetT) target, (char *) NULL);
    *p = (char) fill;
}

int nyxstone_glue_had_errors (void)
{
    return had_errors ();
}

/* ----- 4. Layout + relax + fixup pass ---------------------------------- */
/* Fixed-byte count of a frag: closed frags use fr_fix; the live tail frag
   uses frag_now_fix() (its fr_fix is not written until the frag closes). */
static size_t frag_fix_size (const frchainS *fc, const fragS *fr)
{
    return (fr == fc->frch_last && fr == frag_now)
           ? (size_t) frag_now_fix ()
           : (size_t) fr->fr_fix;
}

/* Fill-repeat byte count of a frag.  Plain frag_more frags are rs_fill with
   fr_var == 0; rs_align/rs_org frags are converted by finalize_align_org()
   into rs_fill with fr_var == 1 and fr_offset == pad count. */
static size_t frag_fill_size (const fragS *fr)
{
    if (fr->fr_type == rs_fill && fr->fr_var > 0 && fr->fr_offset > 0)
        return (size_t) fr->fr_offset * (size_t) fr->fr_var;
    return 0;
}

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
            addr += frag_fix_size (fc, fr) + frag_fill_size (fr);
        }
    }
}

/* Convert rs_align/rs_align_code/rs_org frags into plain rs_fill frags with
   an exact pad count.  Must run AFTER md_convert_frag has finalized every
   branch frag's fr_fix (relax only *chooses* sizes; the displacement bytes
   themselves are encoded later in apply_text_fixups, so converting first is
   safe) -- the pad of an alignment frag depends only on the exact sizes of
   everything before it, which a single forward pass accumulates.

   The 1-byte fill pattern was stored by frag_align/nyxstone_glue_org at
   fr_literal[fr_fix].  An rs_align frag's max-skip (gas: third .align
   operand) lives in fr_subtype: if the needed pad exceeds it, no padding is
   emitted, exactly like gas. */
static void finalize_align_org (segT seg)
{
    segment_info_type *si = seg_info (seg);
    if (!si) return;
    addressT addr = 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            fr->fr_address = addr;
            size_t fix = frag_fix_size (fc, fr);
            size_t pad = 0;
            if (fr->fr_type == rs_align || fr->fr_type == rs_align_code) {
                addressT off      = addr + fix;
                addressT boundary = (addressT) 1 << fr->fr_offset;
                pad = (size_t) ((boundary - (off & (boundary - 1)))
                                & (boundary - 1));
                if (fr->fr_subtype && pad > (size_t) fr->fr_subtype)
                    pad = 0;   /* max-skip exceeded: skip the alignment */
                fr->fr_type   = rs_fill;
                fr->fr_offset = (offsetT) pad;
                fr->fr_var    = 1;
            } else if (fr->fr_type == rs_org) {
                addressT off    = addr + fix;
                addressT target = (addressT) fr->fr_offset
                                + (fr->fr_symbol
                                   ? (addressT) S_GET_VALUE (fr->fr_symbol)
                                   : 0);
                if (target < off)
                    as_bad ("attempt to move .org backwards");
                else
                    pad = (size_t) (target - off);
                fr->fr_type   = rs_fill;
                fr->fr_offset = (offsetT) pad;
                fr->fr_var    = 1;
            } else {
                pad = frag_fill_size (fr);
            }
            addr += fix + pad;
        }
}

/* Walk md_relax_table backwards from `sub` to the head (shortest encoding) of
   its relax chain.  rlx_more only ever points to a *longer* state, so the head
   is the subtype that no other entry's rlx_more refers to.  Index 0 is gas's
   reserved "no relax" sentinel and is excluded from the predecessor search. */
static relax_substateT chain_head (relax_substateT sub)
{
    for (int guard = 0; guard < MD_RELAX_TABLE_N; ++guard) {
        relax_substateT pred = 0;
        for (int i = 1; i < MD_RELAX_TABLE_N; ++i)
            if (md_relax_table[i].rlx_more == sub) { pred = (relax_substateT) i; break; }
        if (!pred) break;
        sub = pred;
    }
    return sub;
}

/* Walk to the *longest* encoding in `sub`'s relax chain (rlx_more == 0). */
static relax_substateT chain_terminal (relax_substateT sub)
{
    for (int guard = 0; guard < MD_RELAX_TABLE_N; ++guard) {
        relax_substateT next = md_relax_table[sub].rlx_more;
        if (!next) break;
        sub = next;
    }
    return sub;
}

/* Relax branch frags to their shortest reaching encoding.

   gas's md_assemble seeds every TriCore branch frag in its *longest* encoding,
   and the generic relaxer (relax_segment -> relax_frag) only ever grows a
   subtype, never shrinks it -- so left alone the branch stays long.  To match
   standalone gas+objdump we first reset each branch whose target is defined in
   *this* section back to the head (shortest) of its relax chain; relax_segment
   then grows it only as far as the displacement requires.

   The reset is deliberately gated on "defined in this section":

     - locally-defined, in-range branches shrink to their short encoding
       (e.g. `start: j start` -> 2-byte 0x3c form);
     - undefined externals, reloc targets, and .equ-injected absolute labels
       are left in the seeded long form, preserving the 4-byte encoding and the
       maximum displacement range the linker may later need.

   This only chooses sizes/subtypes; byte emission (md_convert_frag) happens
   afterwards, once symbol values are resolved at their final addresses --
   converting earlier would bake in a zero displacement.  relax_segment
   converges for a single section in one call; we loop defensively until it
   reports no further size change (bounded for safety). */
static void relax_branches (segT seg)
{
    segment_info_type *si = seg_info (seg);
    if (!si || !si->frchainP) return;

    /* Prepare the frag chain for relax_segment the way gas's subsegs_finish
       would, without the assembler globals (input scrub, obstack bookkeeping)
       the embedded glue intentionally never sets up:
         - the open tail frag (frch_last == frag_now) gets its true fixed size
           written into fr_fix -- the same value layout()/extract() already
           trust via frag_now_fix() -- leaving the obstack untouched;
         - any frag still carrying the zero-initialized fr_type (e.g. the empty
           frag the last frag_var left dangling) is settled to rs_fill, else
           relax_segment aborts with "Case value 0 unexpected". */
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            if (fr == fc->frch_last && fr == frag_now)
                fr->fr_fix = (unsigned long) frag_now_fix ();
            if (fr->fr_type == 0) {
                fr->fr_type = rs_fill;
                fr->fr_var  = 0;
            }
        }

    /* Seed cumulative fr_address so relax_segment can compute distances, then
       choose each branch's starting form:
         - target defined in this section: reset to the shortest form so the
           relaxer can pick the smallest encoding that reaches;
         - target outside this section (external/absolute label, or undefined
           reloc target): force the longest form.  These keep maximum range and
           a size that doesn't depend on the symbol's absolute value (so e.g.
           assembling the same source at different addresses is invariant). */
    layout (seg);
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next)
            if (fr->fr_type == rs_machine_dependent && fr->fr_symbol)
                fr->fr_subtype = (S_GET_SEGMENT (fr->fr_symbol) == seg)
                               ? chain_head (fr->fr_subtype)
                               : chain_terminal (fr->fr_subtype);

    for (int pass = 0;
         relax_segment (si->frchainP->frch_root, seg, pass) && pass < 32;
         ++pass)
        ;
}

size_t nyxstone_glue_extract_text_bytes (uint8_t *out, size_t cap);

/* Read/write a little-endian word of `n` (1/2/4) bytes at buf[at]. */
static uint32_t rd_le (const uint8_t *buf, size_t at, unsigned n)
{
    uint32_t w = 0;
    for (unsigned i = 0; i < n; ++i) w |= (uint32_t) buf[at + i] << (8u * i);
    return w;
}
static void wr_le (uint8_t *buf, size_t at, unsigned n, uint32_t w)
{
    for (unsigned i = 0; i < n; ++i) buf[at + i] = (uint8_t) (w >> (8u * i));
}

/* Encode a resolved symbol reference into the instruction/data field in place.

   We do this ourselves because md_apply_fix in this gas port records the addend
   but never writes a fixup whose symbol is still attached (it defers to
   write_object_file, which Nyxstone skips); the relocation's BFD special
   function likewise returns bfd_reloc_outofrange in this no-output-bfd context.
   So for every *defined* symbol reference -- local branches/data and, in the
   plain path, every LabelDefinition -- the bytes are written here.

   This is a faithful transcription of tc-tricore.c's md_apply_fix switch: each
   TriCore relocation form has its own bit layout, and several are NOT a simple
   shift-and-mask (the B-format 24-bit displacement is split across the word;
   16OFF/LO2 permute the value; HI/HIADJ take the high half).  Approximating
   them from howto->dst_mask silently produced wrong bytes, so every form is
   encoded explicitly, keyed on howto->name; gas's own value placement and range
   checks are mirrored exactly.

   `value` is the final field value computed by apply_text_fixups: a byte
   displacement (target - PC) for PC-relative forms, or the absolute target
   address for the rest.  Returns 1 if encoded, 0 if the relocation form is not
   one we encode (caller raises a loud "unsupported relocation" error rather
   than emit wrong bytes), and -1 if the value is illegal/out of range for the
   form (caller raises an error). */
static int encode_pcrel_field (uint8_t *buf, size_t total, size_t at,
                               reloc_howto_type *howto, long value)
{
    if (at >= total || !howto->name) return 0;
    /* Names are "R_TRICORE_<FORM>"; compare on the <FORM> suffix. */
    const char *r = howto->name;
    if (strncmp (r, "R_TRICORE_", 10) == 0) r += 10;

#define R_IS(s) (strcmp (r, (s)) == 0)

    /* ---- data relocations: plain little-endian field, truncated as gas does
       (md_number_to_chars).  Value is absolute for *ABS, a displacement for
       *REL.  Also covers the generic BFD_RELOC_8/16/32 howtos by mask. ---- */
    if (R_IS ("32ABS") || R_IS ("32REL")
        || howto->dst_mask == 0xffffffffu) {
        if (at + 4 > total) return 0;
        wr_le (buf, at, 4, (uint32_t) value);
        return 1;
    }
    if (R_IS ("16ABS") || (howto->dst_mask == 0xffffu && howto->bitpos == 0)) {
        if (at + 2 > total) return 0;
        wr_le (buf, at, 2, (uint32_t) value & 0xffffu);
        return 1;
    }
    if (R_IS ("8ABS") || (howto->dst_mask == 0xffu && howto->bitpos == 0)) {
        buf[at] = (uint8_t) value;
        return 1;
    }

    /* ---- instruction relocations: read the opcode word (its width is encoded
       in the low bit of the first byte, TriCore's 16/32-bit marker), apply the
       form-specific placement, write it back. ---- */
    unsigned len = (buf[at] & 1u) ? 4u : 2u;
    if (at + len > total) return 0;
    uint32_t op  = rd_le (buf, at, len);
    long     val = value;

    if (R_IS ("24REL")) {
        if (val & 1) return -1;
        if (val < -16777216L || val > 16777214L) return -1;
        val >>= 1;
        op &= ~(((uint32_t) 0xffff << 16) | (uint32_t) 0xff00);
        op |= ((uint32_t) (val & 0xffff) << 16);
        op |= ((uint32_t) (val & 0xff0000) >> 8);
    } else if (R_IS ("24ABS")) {
        if ((unsigned long) val & 0x0fe00001UL) return -1;
        val >>= 1;
        val |= ((val & 0x78000000L) >> 7);
        op &= ~(((uint32_t) 0xffff << 16) | (uint32_t) 0xff00);
        op |= ((uint32_t) (val & 0xffff) << 16);
        op |= ((uint32_t) (val & 0xff0000) >> 8);
    } else if (R_IS ("18ABS")) {
        if ((unsigned long) val & 0x0fffc000UL) return -1;
        op &= ~0xf3fff000u;
        op |= ((uint32_t) (val & 0x3f) << 16);
        op |= ((uint32_t) (val & 0x3c0) << 22);
        op |= ((uint32_t) (val & 0x3c00) << 12);
        op |= ((uint32_t) ((unsigned long) val & 0xf0000000UL) >> 16);
    } else if (R_IS ("18ABS_14")) {
        if ((unsigned long) val & 0x00003fffUL) return -1;
        val = (long) ((unsigned long) val >> 14);
        op &= ~0xf3fff000u;
        op |= ((uint32_t) (val & 0x3f) << 16);
        op |= ((uint32_t) (val & 0x3c0) << 22);
        op |= ((uint32_t) (val & 0x3c00) << 12);
        op |= ((uint32_t) (val & 0x3c000) >> 2);
    } else if (R_IS ("HI")) {
        op &= ~((uint32_t) 0xffff << 12);
        op |= (((uint32_t) (val >> 16) & 0xffff) << 12);
    } else if (R_IS ("HIADJ")) {
        op &= ~((uint32_t) 0xffff << 12);
        op |= (((uint32_t) ((val + 0x8000) >> 16) & 0xffff) << 12);
    } else if (R_IS ("LO") || R_IS ("16CONST")) {
        if (R_IS ("16CONST") && (val < -32768L || val > 32767L)) return -1;
        op &= ~((uint32_t) 0xffff << 12);
        op |= (((uint32_t) val & 0xffff) << 12);
    } else if (R_IS ("LO2") || R_IS ("16OFF")) {
        if (R_IS ("16OFF") && (val < -32768L || val > 32767L)) return -1;
        op &= ~(((uint32_t) 0x3f << 16) | ((uint32_t) 0x3c0 << 22)
                | ((uint32_t) 0xfc00 << 12));
        op |= ((uint32_t) (val & 0x3f) << 16);
        op |= ((uint32_t) (val & 0x3c0) << 22);
        op |= ((uint32_t) (val & 0xfc00) << 12);
    } else if (R_IS ("10OFF")) {
        if (val < -512L || val > 511L) return -1;
        op &= ~(((uint32_t) 0x3f << 16) | ((uint32_t) 0x3c0 << 22));
        op |= ((uint32_t) (val & 0x3f) << 16);
        op |= ((uint32_t) (val & 0x3c0) << 22);
    } else if (R_IS ("15REL")) {
        if (val & 1) return -1;
        if (val < -32768L || val > 32766L) return -1;
        op &= ~((uint32_t) 0x7fff << 16);
        op |= (((uint32_t) (val >> 1) & 0x7fff) << 16);
    } else if (R_IS ("8REL")) {
        if (val & 1) return -1;
        if (val < -256L || val > 254L) return -1;
        val >>= 1;
        op &= ~((uint32_t) 0xff << 8);
        op |= (((uint32_t) val & 0xff) << 8);
    } else if (R_IS ("4REL")) {
        if (val & 1) return -1;
        if (val < 0L || val > 30L) return -1;
        val >>= 1;
        op &= ~((uint32_t) 0xf << 8);
        op |= (((uint32_t) val & 0xf) << 8);
    } else if (R_IS ("4REL2")) {
        if (val & 1) return -1;
        if (val < -32L || val > -2L) return -1;
        val >>= 1;
        op &= ~((uint32_t) 0xf << 8);
        op |= (((uint32_t) val & 0xf) << 8);
    } else if (R_IS ("5REL")) {
        if (val & 1) return -1;
        if (val < 0L || val > 62L) return -1;
        val >>= 1;
        op &= ~(((uint32_t) 0xf << 8) | ((uint32_t) 0x10 << 3));
        op |= (((uint32_t) val & 0xf) << 8);
        op |= (((uint32_t) val & 0x10) << 3);
    } else if (R_IS ("9SCONST")) {
        if (val < -256L || val > 255L) return -1;
        op &= ~((uint32_t) 0x1ff << 12);
        op |= (((uint32_t) val & 0x1ff) << 12);
    } else if (R_IS ("9ZCONST")) {
        if ((unsigned long) val & ~511UL) return -1;
        op &= ~((uint32_t) 0x1ff << 12);
        op |= ((uint32_t) val << 12);
    } else if (R_IS ("8CONST")) {
        if ((unsigned long) val & ~255UL) return -1;
        op &= ~((uint32_t) 0xff << 8);
        op |= ((uint32_t) val << 8);
    } else if (R_IS ("16SM") || R_IS ("10SM") || R_IS ("16SM2")) {
        return -1;   /* sm: prefix illegal for a resolved constant offset */
    } else {
        return 0;    /* form we don't encode: fail loudly, never guess */
    }

    wr_le (buf, at, len, op);
    return 1;
#undef R_IS
}

/* Apply fixups and report the count still unresolved (left as relocations).

   `base` is the address the section is being assembled at (used for absolute
   references; PC-relative ones are base-independent).

   md_apply_fix in this gas port does NOT write a fixup whose symbol is still
   attached -- it only records the addend and defers to gas's write_object_file,
   which Nyxstone skips.  That affects *every* reference to a defined symbol
   (branches AND data like `.word label`), so we encode them ourselves:

     - defined symbol (in this section or absolute via `.equ`): encode the value
       into the field via encode_pcrel_field.  PC-relative refs use the
       displacement `target - PC`; absolute refs (data, `movh hi:`, ...) use the
       absolute address `base + value`.
     - undefined / external symbol: hand to md_apply_fix, which records the
       addend so nyxstone_glue_collect_relocs can emit a relocation entry.

   Defined-symbol encoding runs on a contiguous image of the section (so offsets
   are real) after md_apply_fix has recorded the external addends; the image is
   then copied back into the frags. */
static int apply_text_fixups (segT seg, uint64_t base)
{
    segment_info_type *si = seg_info (seg);
    if (!si) return 0;

    g_unsupported_reloc = NULL;
    int unresolved = 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fixS *fx = fc->fix_root; fx; fx = fx->fx_next) {
            if (!fx->fx_addsy) continue;
            resolve_symbol_value (fx->fx_addsy);
            if (S_IS_DEFINED (fx->fx_addsy)) continue;   /* encoded below */

            valueT val = S_GET_VALUE (fx->fx_addsy) + fx->fx_offset;
            if (fx->fx_pcrel) val -= md_pcrel_from_section (fx, seg);
            md_apply_fix (fx, &val, seg);
            if (fx->fx_addsy && !fx->fx_done) ++unresolved;
        }

    size_t total = nyxstone_glue_extract_text_bytes (NULL, 0);
    uint8_t *buf = total ? (uint8_t *) malloc (total) : NULL;
    if (!buf) return unresolved;
    nyxstone_glue_extract_text_bytes (buf, total);

    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fixS *fx = fc->fix_root; fx; fx = fx->fx_next) {
            if (!fx->fx_addsy || !S_IS_DEFINED (fx->fx_addsy)) continue;
            reloc_howto_type *howto = bfd_reloc_type_lookup (stdoutput, fx->fx_r_type);
            if (!howto) { g_unsupported_reloc = "<no howto>"; continue; }

            /* Absolute address of the target.  Symbols defined in this section
               are laid out from 0, so add the assemble base; external symbols
               (defined absolute via nyxstone_glue_define_abs) already hold their
               full address. */
            uint64_t sym_abs = (S_GET_SEGMENT (fx->fx_addsy) == seg)
                             ? base + (uint64_t) S_GET_VALUE (fx->fx_addsy)
                             : (uint64_t) S_GET_VALUE (fx->fx_addsy);
            long value;
            if (howto->pc_relative)        /* displacement: both sides absolute */
                value = (long) (sym_abs + fx->fx_offset)
                      - (long) (base + md_pcrel_from_section (fx, seg));
            else                            /* absolute reference */
                value = (long) (sym_abs + fx->fx_offset);

            int rc = encode_pcrel_field (buf, total,
                                         (size_t) (fx->fx_frag->fr_address + (offsetT) fx->fx_where),
                                         howto, value);
            if (rc == 0)
                g_unsupported_reloc = howto->name ? howto->name : "<unknown>";
            else if (rc < 0)
                /* Out of range for the chosen form; raise a gas error so
                   do_assemble surfaces it instead of emitting truncated bytes. */
                as_bad ("displacement to '%s' out of range for %s",
                        S_GET_NAME (fx->fx_addsy),
                        howto->name ? howto->name : "branch");
        }

    /* Copy the encoded image back into the frag literals.  Fixups only ever
       sit in the fixed part of a frag, so fill bytes are skipped (advanced
       over) but never copied back. */
    addressT addr = 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            size_t n = frag_fix_size (fc, fr);
            if (n && addr + n <= total)
                memcpy (fr->fr_literal, buf + addr, n);
            addr += n + frag_fill_size (fr);
        }
    free (buf);
    return unresolved;
}

int nyxstone_glue_resolve_text_fixups (uint64_t base)
{
    /* Phase order for this gas port, minimised for one section:
         1. relax            -- choose each branch frag's encoding (subtype).
         2. md_convert_frag  -- emit the chosen opcode bytes; fr_fix becomes
                                final.  Displacement *values* are NOT baked
                                here: md_convert_frag emits fixups instead
                                (see tc-tricore.c), so converting before the
                                final layout is safe.
         3. finalize_align_org -- size rs_align/rs_org frags exactly, now
                                that every fr_fix is final.
         4. layout           -- final cumulative addresses (fill-aware).
         5. apply fixups     -- re-resolve symbols at final addresses and
                                encode every displacement/absolute field.
       Symbol re-resolution is exact because gas only caches resolved
       values once `finalize_syms` is set, which we never do. */
    relax_branches (text_section);

    segment_info_type *si = seg_info (text_section);
    if (!si) return 0;

    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next)
            if (fr->fr_type == rs_machine_dependent && fr->fr_symbol)
                resolve_symbol_value (fr->fr_symbol);
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next)
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next)
            if (fr->fr_type == rs_machine_dependent)
                md_convert_frag (stdoutput, text_section, fr);
    finalize_align_org (text_section);
    layout (text_section);

    return apply_text_fixups (text_section, base);
}

/* Name of a relocation form the local-branch encoder could not handle during
   the most recent nyxstone_glue_resolve_text_fixups, or NULL if all were
   handled.  Lets the caller fail loudly instead of emitting a wrong branch. */
const char *nyxstone_glue_unsupported_reloc (void)
{
    return g_unsupported_reloc;
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
   the frag).  Fill frags produced by finalize_align_org additionally emit
   fr_offset repetitions of the fr_var-byte pattern at fr_literal[fr_fix]. */
size_t nyxstone_glue_extract_text_bytes (uint8_t *out, size_t cap)
{
    size_t total = 0;
    segment_info_type *si = seg_info (text_section);
    if (!si) return 0;
    for (frchainS *fc = si->frchainP; fc; fc = fc->frch_next) {
        for (fragS *fr = fc->frch_root; fr; fr = fr->fr_next) {
            size_t n = frag_fix_size (fc, fr);
            if (n) {
                if (out && total + n <= cap)
                    memcpy (out + total, fr->fr_literal, n);
                total += n;
            }
            size_t fill = frag_fill_size (fr);
            if (fill) {
                size_t var = (size_t) fr->fr_var;
                if (out && total + fill <= cap)
                    for (size_t i = 0; i < fill; i += var)
                        memcpy (out + total + i,
                                fr->fr_literal + fr->fr_fix, var);
                total += fill;
            }
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
   we emit "j 0x1068" instead of "j 0x00001068".  TriCore is a 32-bit
   target, so like objdump we mask the value to the architecture's address
   width -- a backward branch near address 0 prints as 0xfffffffe, not as
   the sign-extended 0xfffffffffffffffe.  Routes through the same
   fprintf_func so the text lands in the same output buffer. */
static void dis_print_address (bfd_vma addr, struct disassemble_info *info) {
    (*info->fprintf_func) (info->stream, "0x%" PRIx32, (uint32_t) addr);
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
