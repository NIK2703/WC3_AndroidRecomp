/* liftcore.h - shared state for the compiled x86-32 -> C lifter.
 *
 * This is the C port of tools/lift/lift32_cpu.py from the pcrecomp checkout.
 * The Python lifter is the reference: the C one must produce byte-identical
 * text for the same input, because the corpus already lifted by the Python
 * version and the corpus lifted by this one are linked into the same host.
 *
 * Decoding is delegated to capstone.dll (the same library the Python lifter
 * used), so instruction boundaries, operand sizes and register names cannot
 * drift between the two implementations.
 */
#ifndef WC3_LIFTCORE_H
#define WC3_LIFTCORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pe.h"
#include "csglue.h"
#include "liftflags.h"

#define LF_MAX_JT     64
#define LF_MAX_JT_ENT 256

typedef struct {
    uint32_t ea;
    int n;
    uint32_t t[LF_MAX_JT_ENT];
} LfJumpTable;

/* Per-function flag summary used by the global call/ret boundary model. */
typedef struct {
    uint32_t addr;
    uint8_t entry_reads;      /* flags read at entry before any write */
    uint8_t exit_may_writes;  /* flags written on some path to any exit */
    uint8_t exit_must_writes; /* flags written on every path to every exit */
    uint8_t exit_flags_needed; /* callers need these after the ret */
    uint8_t computed;          /* entry/may/must are real, not a placeholder */
    uint8_t conservative;      /* masks are the unknown/indirect fallback */
    uint8_t queued;            /* addr is in the summary worklist */
} LfFlagSummary;

typedef struct {
    uint32_t addr;
    uint32_t size;
} LfFuncSize;

typedef struct {
    Pe *pe;
    csh h;
    uint32_t image_lo, image_hi;
    uint32_t func_start, func_end;
    LfJumpTable jt[LF_MAX_JT];
    int njt;
    uint32_t *reloc;
    size_t nreloc;
    int block_mode;             /* WC3_LIFT_BLOCK=1 */
    int x87_top;                /* compile-time virtual x87 top, 0..7 */
    int x87_lazy;               /* WC3_LIFT_X87_LAZY: skip clean internal sync */
    int x87_lazy_safe;          /* per-function: top==0 at internal boundaries */
    int x87_dirty;              /* emission-time: local x87 state changed */
    int flag_boundary;          /* WC3_LIFT_FLAG_BOUNDARY: global call/ret model */
    LfFlagSummary summary;      /* current function's flag summary */
    LfFlagSummary *summary_table; /* hash table of summaries */
    size_t summary_table_cap;
    size_t summary_table_n;
    uint32_t *summary_queue;    /* exit_flags_needed worklist */
    size_t summary_qhead, summary_qtail, summary_qcap;
    int summary_precomputed;    /* lf_precompute_summaries completed */
    int summary_incomplete;     /* a summary step failed: every ret N is ALL */
    LfFuncSize *func_map;       /* sorted function size map from funcs.txt */
    size_t func_n;
    int flag_fusion;            /* WC3_LIFT_FLAG_FUSION != "0" */
    int flagless;               /* WC3_LIFT_FLAGLESS != "0" */
    int gpr_cache;              /* WC3_LIFT_GPR_CACHE != "0" */
    const void *flag_ins;       /* InsnCopy array of the current extent */
    int flag_current_index;     /* index of the instruction being translated */
    const uint8_t *flag_role;   /* per instruction: 0 normal, 1 producer, 2 end */
    const LfFlagFuse *flag_fuse;/* per instruction, valid for roles 1 and 2 */
    uint8_t *flag_live_out;     /* per instruction: flags live after it */
} Lifter;

/* A translated instruction: one or more lines of C. */
#define LF_MAX_EMIT 300

typedef struct {
    const char *v[LF_MAX_EMIT];
    int n;
} Emit;

/* --- scratch strings (freed wholesale once per instruction) --- */
void   lf_arena_reset(void);
char  *lf_afmt(const char *fmt, ...);

/* Global call/ret flag summaries. */
void   lf_summary_init(Lifter *L, size_t cap);
void   lf_precompute_summaries(Lifter *L);

/* --- emission helpers (liftemit.c) --- */
Emit lf_translate(Lifter *L, cs_insn *insn, const uint32_t *labels, int nlabels);
Emit lf_fpu(Lifter *L, cs_insn *insn);
Emit lf_mmx(Lifter *L, cs_insn *insn);
Emit lf_sse(Lifter *L, cs_insn *insn);
int  lf_is_mmx(Lifter *L, cs_insn *insn);
int  lf_is_sse(Lifter *L, cs_insn *insn);

/* --- operand rendering shared by every family --- */
const char *lf_reg_name(Lifter *L, unsigned reg);
char *lf_addr_expr(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_seg_off(Lifter *L, cs_insn *insn, cs_x86_op *op);
const char *lf_seg_name(Lifter *L, cs_x86_op *op);
char *lf_rd(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_wr(Lifter *L, cs_insn *insn, cs_x86_op *op, const char *val);
char *lf_src(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_dst_write(Lifter *L, cs_insn *insn, cs_x86_op *op, const char *val);
char *lf_read_dst(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_mm_read(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_mm_write(Lifter *L, cs_insn *insn, cs_x86_op *op, const char *val);
char *lf_mm_count(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_xm(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_ss(Lifter *L, cs_insn *insn, cs_x86_op *op);
char *lf_sd(Lifter *L, cs_insn *insn, cs_x86_op *op);
int  lf_xmm_index(Lifter *L, cs_x86_op *op);
int  lf_mm_index(Lifter *L, cs_x86_op *op);
const char *lf_cond(const char *m);
char *lf_todo(uint32_t va, const char *what);
int  lf_in_image(Lifter *L, uint32_t a);
int  lf_disp_is_addr(Lifter *L, cs_insn *insn, uint32_t d);
int  lf_is_xmm(Lifter *L, cs_x86_op *op);
int  lf_is_mmx_op(Lifter *L, cs_x86_op *op);

/* Emit helpers */
static inline Emit lf_emit(void) { Emit e; e.n = 0; return e; }
static inline void lf_push(Emit *e, const char *s) { if (e->n < LF_MAX_EMIT) e->v[e->n++] = s; }
static inline void lf_pushf(Emit *e, const char *fmt, ...);

#include <stdarg.h>
static inline void lf_pushf(Emit *e, const char *fmt, ...)
{
    va_list ap;
    char buf[4096];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    e->v[e->n++] = lf_afmt("%s", buf);
}

#endif /* WC3_LIFTCORE_H */
