/* liftflags.h - flag-use analysis and direct-condition emission.
 *
 * This is the A.2/A.3 half of the C lifter: it decides when a flag-producing
 * instruction can be folded into a following jcc/setcc/cmov and builds the
 * direct C condition.  It deliberately works on a small per-instruction
 * summary so the liveness part can be tested without capstone or a PE.
 *
 * Flag bits match the six fields of CPU.  A producer never reports AF as
 * written for liveness: helpers may skip AF under WC3_FLAGS_FAST_PF_AF, and
 * keeping AF live is the conservative choice.  The planner adds AF to the
 * materialisation mask separately, because the original helper would write it
 * on a taken path where it is observed.
 */
#ifndef WC3_LIFTFLAGS_H
#define WC3_LIFTFLAGS_H

#include <stddef.h>
#include <stdint.h>

enum {
    LF_FLAG_CF = 1u << 0,
    LF_FLAG_ZF = 1u << 1,
    LF_FLAG_SF = 1u << 2,
    LF_FLAG_OF = 1u << 3,
    LF_FLAG_AF = 1u << 4,
    LF_FLAG_PF = 1u << 5,
    LF_FLAG_ALL = LF_FLAG_CF | LF_FLAG_ZF | LF_FLAG_SF |
                  LF_FLAG_OF | LF_FLAG_AF | LF_FLAG_PF
};

typedef enum {
    LF_FLAG_NONE = 0,
    LF_FLAG_CMP,
    LF_FLAG_TEST,
    LF_FLAG_ADD,
    LF_FLAG_SUB,
    LF_FLAG_AND,
    LF_FLAG_OR,
    LF_FLAG_XOR,
    LF_FLAG_INC,
    LF_FLAG_DEC,
    LF_FLAG_NEG,
    LF_FLAG_ADC,
    LF_FLAG_SBB,
    LF_FLAG_FP_CMP
} LfFlagKind;

typedef enum {
    LF_CONS_NONE = 0,
    LF_CONS_JCC,
    LF_CONS_SETCC,
    LF_CONS_CMOV,
    LF_CONS_LAHF
} LfFlagConsumer;

/* One instruction as seen by the flag planner.  succ1/succ2 are instruction
 * indices, -1 for "leaves this lifted extent" and -2 for "no successor". */
typedef struct {
    uint8_t reads;
    uint8_t writes;
    uint8_t consumer;
    uint8_t cond_mask;
    uint8_t producer;
    uint8_t size;
    uint8_t is_label;
    uint8_t is_control;
    int succ1;
    int succ2;
} LfFlagInsn;

/* A selected producer/consumer pair.  mat_taken/mat_fall ask the emitter to
 * run the original flags_* helper on that edge only; the other edge continues
 * with the direct condition and no flag store. */
typedef struct {
    int producer;
    int consumer;
    int kind;
    int size;
    int mat_taken;
    int mat_fall;
    uint32_t producer_addr;
} LfFlagFuse;

#define LF_FLAG_MAX_SPAN 8

/* Map a condition-code suffix ("e", "ne", "b", "ae", ...) to the flags it
 * reads.  Returns 0 for an unknown suffix. */
uint8_t lf_cond_mask(const char *cc);

/* Build a direct C condition for a fused producer.  fa/fb/fr/ffull/fcin are
 * C expressions (normally the generated temporary names).  ffull and fcin are
 * only used by ADC/SBB and may be NULL otherwise.  Returns 0 on success and
 * writes a NUL-terminated expression to buf. */
int lf_flag_cond(char *buf, size_t cap, const char *cc, LfFlagKind kind,
                 int sz, const char *fa, const char *fb, const char *fr,
                 const char *ffull, const char *fcin);

/* True when the producer kind writes AF (the original helper does). */
int lf_flag_producer_af(int kind);

/* Plan a function-sized straight-line/CFG region.  Returns the number of
 * fused pairs, or -1 when the flag liveness did not converge (the caller must
 * then leave the function unchanged).  When live_out is non-NULL it receives
 * the live-out mask of every instruction, which the lifter uses to emit
 * flagless arithmetic when no written flag is observable. */
int lf_flag_plan_ex(const LfFlagInsn *ins, int n, LfFlagFuse *out, int max_out,
                    uint8_t *live_out);

/* Wrapper without liveness export. */
int lf_flag_plan(const LfFlagInsn *ins, int n, LfFlagFuse *out, int max_out);

#endif /* WC3_LIFTFLAGS_H */
