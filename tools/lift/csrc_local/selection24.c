/* selection24.c - 24-unit selection / adaptive 6x2 <-> 8x3 panel LiftPass.
 *
 * The pass edits the generated corpus through address-comment anchors.  It is
 * idempotent, fail-closed on a template mismatch, and publishes a per-kind
 * counts object plus a line-level change set.  Per-chunk runs use a subset of
 * the sites; the configure step sums every chunk JSON through
 * --verify-coverage and compares the union with the fixed totals below.
 *
 *   selection24 --lift IN --out OUT [--json REPORT] [--profile seed|closed]
 *               [--quiet]
 *   selection24 --verify-coverage @manifest
 *   selection24 --self-test
 *
 * Product integration is described in docs/research/SELECTION_24_ANALYSIS.md.
 * No source file under build/corpus-full is modified in place.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define PASS "selection24"
#define PASS_REV 45

typedef enum {
    K_GROUP_ALLOC = 0,
    K_INIT_GROUP,
    K_SELECT,
    K_CAP_OUTER,
    K_CAP_INNER,
    K_FILL_BLOCK,
    K_FILL_GRID,
    K_NOTE_SELECTION,
    K_HIDE_ALL,
    K_ADDUNIT_CAP,
    K_CAP_LOCAL_LIST,
    K_CAP_COPY_LIST,
    K_CAP_SYNC_ADD,
    K_CAP_NET_RECV,
    K_CAP_NET_APPLY,
    K_CAP_NET_SEND,
    K_CAP_SELECT_ALL,
    K_ORDER_FRAME,
    K_ORDER_SORT,
    K_ORDER_LOOP,
    K_ORDER_CALLBACK,
    K_ORDER_CALLBACK_B,
    K_TRACE_EVT2,
    K_TRACE_LOOKUP,
    K_CAP_GROUP_STORE,
    K_CAP_ADD_MERGE,
    K_GUARD_NULL,
    K_GUARD_A60,
    K_GUARD_ABE0,
    K_TRACE_ORDER_UNIT_B,
    K_TRACE_ORDER_UNIT_C,
    K_TRACE_ORDER_LOOP_B,
    K_TRACE_ORDER_LOOP_C,
    K_GUARD_620,
    K_TRACE_ORDER_APPLY,
    K_GUARD_1F20,
    K_TRACE_ORDER_EXEC,
    K_GUARD_SEND_NULL,
    K_GUARD_FDD0,
    K_GUARD_E60,
    K_TRACE_REMOVE,
    K_TRACE_EVBIRTH,
    K_GUARD_E60B,
    K_TRACE_APPLY,
    K_GUARD_TAG,
    K_CAP_SLOT,
    K_GUARD_SLOT,
    K_GUARD_EVENT,
    K_GUARD_FC0,
    K_GUARD_E60_ENTRY,
    K_TRACE_TASKNULL,
    K_TRACE_EVINIT,
    K_TRACE_ORDERDROP,
    K_ORDER_OBJ_CONTAINER,
    K_TRACE_ORDER_SLOTS,
    K_FREELIST_HEAD,
    K_ORDER_GROUP_FULL,
    K_TRACE_EVRELEASE,
    K_SLOT_UNLINK,
    K_GROUP24,
    K_TRACE_FORMATION,
    K_COUNT
} Kind;

static const char *const kind_name[K_COUNT] = {
    "group_alloc",
    "init_group",
    "select",
    "cap_outer",
    "cap_inner",
    "fill_block",
    "fill_grid",
    "note_selection",
    "hide_all",
    "addunit_cap",
    "cap_local_list",
    "cap_copy_list",
    "cap_sync_add",
    "cap_net_recv",
    "cap_net_apply",
    "cap_net_send",
    "cap_select_all",
    "order_frame",
    "order_sort",
    "order_loop",
    "order_callback",
    "order_callback_b",
    "trace_evt2",
    "trace_lookup",
    "cap_group_store",
    "cap_add_merge",
    "guard_null",
    "guard_a60",
    "guard_abe0",
    "trace_order_unit_b",
    "trace_order_unit_c",
    "trace_order_loop_b",
    "trace_order_loop_c",
    "guard_620",
    "trace_order_apply",
    "guard_1f20",
    "trace_order_exec",
    "guard_send_null",
    "guard_fdd0",
    "guard_e60",
    "trace_remove",
    "trace_evbirth",
    "guard_e60b",
    "trace_apply",
    "guard_tag",
    "cap_slot",
    "guard_slot",
    "guard_event",
    "guard_fc0",
    "guard_e60_entry",
    "trace_tasknull",
    "trace_evinit",
    "trace_orderdrop",
    "order_obj_container",
    "trace_order_slots",
    "freelist_head",
    "order_group_full",
    "trace_evrelease",
    "slot_unlink",
    "group24",
    "trace_formation"
};

/* Fixed union totals for the whole relifted game corpus. */
static const unsigned expected_total[K_COUNT] = {
    1, /* group_alloc   */
    2, /* init_group    */
    2, /* select        */
    2, /* cap_outer     */
    2, /* cap_inner     */
    2, /* fill_block    */
    1, /* fill_grid     */
    1, /* note_selection*/
    1, /* hide_all      */
    1, /* addunit_cap   */
    5, /* cap_local_list*/
    3, /* cap_copy_list */
    1, /* cap_sync_add  */
    1, /* cap_net_recv  */
    5, /* cap_net_apply */
    2, /* cap_net_send  */
    1, /* cap_select_all*/
    2, /* order_frame   */
    4, /* order_sort    */
    4, /* order_loop    */
    8, /* order_callback*/
    4, /* order_callback_b */
    1, /* trace_evt2    */
    2, /* trace_lookup  */
    3, /* cap_group_store */
    3, /* cap_add_merge */
    1, /* guard_null    */
    1, /* guard_a60     */
    1, /* guard_abe0    */
    1, /* trace_order_unit_b */
    1, /* trace_order_unit_c */
    2, /* trace_order_loop_b */
    2, /* trace_order_loop_c */
    1, /* guard_620        */
    2, /* trace_order_apply */
    4, /* guard_1f20       */
    2, /* trace_order_exec  */
    3, /* guard_send_null   */
    2, /* guard_fdd0        */
    1, /* guard_e60         */
    4, /* trace_remove      */
    1, /* trace_evbirth     */
    1, /* guard_e60b        */
    2, /* trace_apply       */
    2, /* guard_tag         */
    10, /* cap_slot          */
    3, /* guard_slot        */
    1, /* guard_event       */
    1, /* guard_fc0         */
    1, /* guard_e60_entry   */
    1, /* trace_tasknull    */
    1, /* trace_evinit      */
    1, /* trace_orderdrop   */
    35, /* order_obj_container */
    3, /* trace_order_slots */
    1, /* freelist_head */
    2, /* order_group_full */
    1, /* trace_evrelease */
    1, /* slot_unlink */
    206, /* group24 */
    9 /* trace_formation */
};
/* Whole-file expectations.  seed.c carries both order prologues. */
static const unsigned expected_seed[K_COUNT] = {
    0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 2, 1, 2, 0, 0, 0,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 3, 0, 0, 0, 0, 44, 2
};
/* seed: builders B/C.  closed: listadd x2, cluster try/insert, formation. */

/* closed.c carries the split fragments and the UI allocation/fill code. */
static const unsigned expected_closed[K_COUNT] = {
    1, 2, 1, 1, 1, 2, 1, 0, 0, 1, 5, 3, 1, 0, 5, 2, 1, 0, 4, 4, 8, 4, 1, 0, 2, 1, 1, 1, 1,
    0, 0, 2, 2, 1, 2, 4, 2, 3, 2, 1, 4, 1, 1, 2, 2, 10, 3, 1, 1, 1, 1, 1, 1,
    31, 0, 1, 2, 1, 1, 162, 7
};

typedef struct {
    unsigned n[K_COUNT];
} Counts;

typedef struct {
    Kind kind;
    uint32_t va;
    const char *action;
    int line;
    char before[65];
    char after[65];
} Site;

typedef struct {
    Site *v;
    size_t n;
    size_t cap;
} Sites;

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

static int counts_match(const Counts *c, const unsigned *exp)
{
    int i;

    for (i = 0; i < K_COUNT; ++i)
        if (c->n[i] != exp[i])
            return 0;
    return 1;
}

static unsigned counts_sum(const Counts *c)
{
    unsigned total = 0;
    int i;

    for (i = 0; i < K_COUNT; ++i)
        total += c->n[i];
    return total;
}

/* ------------------------------------------------------------------ */
/* Small buffers and line helpers                                      */
/* ------------------------------------------------------------------ */

static int buf_append(Buf *b, const char *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *np;
        while (cap < b->n + n + 1)
            cap *= 2;
        np = (char *)realloc(b->p, cap);
        if (!np)
            return -1;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 0;
}

static int buf_puts(Buf *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

static void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static const char *line_end(const char *p, const char *end)
{
    const char *q = (const char *)memchr(p, '\n', (size_t)(end - p));

    return q ? q + 1 : end;
}

static size_t term_len(const char *p, const char *le)
{
    if (le > p && le[-1] == '\n') {
        if (le - p >= 2 && le[-2] == '\r')
            return 2;
        return 1;
    }
    return 0;
}

static const char *find_sub(const char *p, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    size_t i;

    if (m == 0)
        return p;
    if (n < m)
        return NULL;
    for (i = 0; i + m <= n; ++i)
        if (memcmp(p + i, needle, m) == 0)
            return p + i;
    return NULL;
}

static int contains_sub(const char *p, size_t n, const char *needle)
{
    return find_sub(p, n, needle) != NULL;
}

static int is_space_ch(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Compare the executable part of one generated line with a template.  The
 * trailing address comment and indentation are not part of the template, so
 * later passes that reflow the comment do not invalidate the anchor.
 */
static int code_equals(const char *p, const char *e, const char *want)
{
    const char *code = p;
    const char *ce = e;
    const char *comment;
    size_t wlen = strlen(want);
    size_t clen;

    while (code < ce && (*code == ' ' || *code == '\t'))
        ++code;
    comment = (const char *)find_sub(code, (size_t)(ce - code), "/*");
    if (comment)
        ce = comment;
    while (ce > code && is_space_ch((unsigned char)ce[-1]))
        --ce;
    clen = (size_t)(ce - code);
    return clen == wlen && memcmp(code, want, wlen) == 0;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Parse the last 8-hex-digit VA in the trailing comment. */
static int parse_va(const char *p, const char *e, uint32_t *out)
{
    const char *comment = NULL;
    const char *q;
    const char *digits;
    uint32_t v = 0;
    int i;

    for (q = p; q + 2 <= e; ++q)
        if (q[0] == '/' && q[1] == '*')
            comment = q;
    if (!comment)
        return 0;
    digits = comment + 2;
    while (digits < e && (*digits == ' ' || *digits == '\t'))
        ++digits;
    if ((size_t)(e - digits) < 9 || digits[8] != ':')
        return 0;
    for (i = 0; i < 8; ++i) {
        int h = hex_digit((unsigned char)digits[i]);
        if (h < 0)
            return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

static void hash_hex(const char *p, size_t n, char out[65])
{
    unsigned char digest[32];
    sha256_ctx s;

    sha256_init(&s);
    sha256_update(&s, p, n);
    sha256_final(&s, digest);
    sha256_hex(digest, out);
}

/*
 * Replace exactly one occurrence of old_s with new_s.  Returns 1 on success,
 * 0 when the substring is absent and -1 when it occurs more than once.
 */
static int replace_once(char *hay, size_t n, const char *old_s,
                        const char *new_s, char **outp, size_t *outn)
{
    const char *hit = find_sub(hay, n, old_s);
    size_t olen = strlen(old_s);
    size_t nlen = strlen(new_s);
    char *res;

    if (!hit)
        return 0;
    if (find_sub(hit + olen, n - (size_t)(hit + olen - hay), old_s))
        return -1;
    res = (char *)malloc(n + nlen + 1);
    if (!res)
        return -1;
    memcpy(res, hay, (size_t)(hit - hay));
    memcpy(res + (size_t)(hit - hay), new_s, nlen);
    memcpy(res + (size_t)(hit - hay) + nlen, hit + olen,
           n - (size_t)(hit + olen - hay));
    *outp = res;
    *outn = n - olen + nlen;
    return 1;
}

static void sites_add(Sites *s, Kind k, uint32_t va, const char *action,
                      int line, const char *before, const char *after)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        Site *nv = (Site *)realloc(s->v, cap * sizeof(*nv));
        if (!nv)
            return;
        s->v = nv;
        s->cap = cap;
    }
    s->v[s->n].kind = k;
    s->v[s->n].va = va;
    s->v[s->n].action = action;
    s->v[s->n].line = line;
    snprintf(s->v[s->n].before, sizeof(s->v[s->n].before), "%s", before);
    snprintf(s->v[s->n].after, sizeof(s->v[s->n].after), "%s", after);
    ++s->n;
}

/* ------------------------------------------------------------------ */
/* Site templates                                                      */
/* ------------------------------------------------------------------ */

#define VA_GROUP_ALLOC      0x6F39E7DEu
#define VA_INIT_GROUP       0x6F39EDA4u
#define VA_SELECT           0x6F3C7DBBu
#define VA_CAP_OUTER        0x6F3C7D71u
#define VA_CAP_INNER        0x6F3C7DD0u
#define VA_FILL_BLOCK       0x6F3C7E78u
#define VA_FILL_GRID        0x6F3DA47Fu
#define VA_NOTE_SELECTION   0x6F3C7D35u
#define VA_HIDE_ALL         0x6F3ACCA3u
#define VA_ADDUNIT_CAP      0x6F28DC30u
#define VA_CAP_LOCAL_LIST   0x6F28EB97u
#define VA_CAP_COPY_LIST    0x6F391AD2u
#define VA_CAP_SYNC_ADD     0x6F28D667u
#define VA_CAP_NET_RECV     0x6F6BC82Au
#define VA_CAP_NET_APPLY    0x6F6BCAC9u
#define VA_CAP_NET_SEND     0x6F6BCF1Du
/* Client-side builder of message 0x17 (Ctrl+digit store): L_6F6BCD30.
 * It clamps the copied unit list the same way L_6F6BCEF0 does for recall
 * (0x16), with an identical `mov ebx, 0xc` / `cmp` / `cmovb` clamp, so the
 * store message never carried more than twelve units even after the receive
 * path was widened. */
#define VA_CAP_NET_SEND_B   0x6F6BCD5Du
#define VA_CAP_SELECT_ALL   0x6F390293u

/* Remaining 12-unit constants of the selection / group cluster.  Sweeping the
 * 45 functions reachable from every existing selection and group site turns up
 * exactly five 0xCu lines: the four limits below and one stack frame
 * (`sub esp, 0xc` at 0x6F146D33), which must stay untouched.  A second hop over
 * the direct callees of those functions adds the same free-slot computation in
 * L_6F3968B0 (0x6F396916): it reads CSelectionWar3+0x1F8 through L_6F290270
 * and passes `12 - count` as the limit to the list builder L_6F391390.
 *
 *   cap_group_store: caller-side loop bound used when the live selection is
 *     copied into a group slot (L_6F292FF0 / L_6F293050).  The loop itself
 *     stops at min(count, bound).
 *   cap_add_merge:   how many more units fit when a stored group is added to
 *     the live selection (L_6F6BC0F0) or when a unit list is merged into it
 *     (L_6F3968B0).  The value is compared with the object's own occupancy, so
 *     12 must become 24 together with the store bound; otherwise
 *     `12 - occupancy` underflows once occupancy exceeds 12.
 *
 * Every limit below is emitted as wc3_sel24_cap(), never as a literal.  The
 * helper returns 24 for the product default and 12 when the launch-settings
 * switch is off.  The corpus is therefore built once and the choice is made at
 * run time.  Structural changes (group allocation 0x134 -> 0x144, order
 * frames/arrays) stay unconditional: they are safe for both values because
 * the loop bounds come from the live selection count.
 */
#define VA_CAP_GROUP_STORE_A 0x6F293020u
#define VA_CAP_GROUP_STORE_B 0x6F293080u
/* Ctrl+digit (message type 0x17) entry L_6F6B90D0.  Before the slot is
 * written it compares the request's unit count with 12 and jumps straight to
 * the cleanup block when the selection is larger.  The store loop below was
 * already widened, but this caller-side guard skipped the whole store, so a
 * 24-unit selection was never bound at all and the previous contents of the
 * slot stayed in place. */
#define VA_CAP_GROUP_STORE_C 0x6F6B915Au
#define VA_CAP_ADD_MERGE_A  0x6F6BC1AFu
#define VA_CAP_ADD_MERGE_B  0x6F6BC1BCu
#define VA_CAP_ADD_MERGE_C  0x6F396916u

/* Command batch (CSelectionWar3::IssueOrder path).  The original frame has a
 * 12-entry 0x24-byte local array ending exactly at the security cookie.  The
 * 24-unit path sorts that array with the live selection count, so the array is
 * moved below the frame and the frame is extended to hold 24 records.
 *
 * There are two independent order builders, both with a 12-entry inline
 * array of 0x24-byte records:
 *
 *   B: entry 0x6F6B98F0, frame 0x250, object [ebp-0x204],
 *      count object+0x40, array object+0x44, writer callback 0x6F6BA590.
 *   C: entry 0x6F6B9F70, frame 0x248, object [ebp-0x1FC],
 *      count object+0x38, array object+0x3C, writer callback 0x6F6BA800.
 *
 * In both builders only the element storage moves.  The object and count stay
 * in place.  Both arrays go to [ebp-0x960]; the frames grow to 0x9A0 (B) and
 * 0x9A8 (C), preserving each original frame size modulo 16 (0x250 % 16 == 0,
 * 0x248 % 16 == 8).  B's direct element displacement is +0x44 -> -0x75C
 * relative to edi; C's is +0x3C -> -0x764.  The field +0x48 is element+0x04
 * in B and element+0x0C in C; both become -0x758.  Each writer's element
 * pointer temp is rebased by -0x7A0, keeping all existing [ptr+0x3C..0x64]
 * field offsets valid. */
#define VA_ORDER_FRAME_B    0x6F6B9901u
#define VA_ORDER_FRAME_C    0x6F6B9F81u
#define VA_ORDER_SORT1      0x6F6B9E05u
#define VA_ORDER_SORT2      0x6F6BA444u
#define VA_ORDER_LOOP1      0x6F6B9E34u
#define VA_ORDER_LOOP2      0x6F6BA473u
#define VA_ORDER_CB_PTR     0x6F6BA76Eu
#define VA_ORDER_CB_BASE    0x6F6BA771u
#define VA_ORDER_CB_FIELD1C 0x6F6BA775u
#define VA_ORDER_CB_FIELD04 0x6F6BA78Cu
#define VA_ORDER_CB_B_PTR   0x6F6BAA00u
#define VA_ORDER_CB_B_FLD0C 0x6F6BAA73u

/*
 * Point-order container expansion (rev 31).
 *
 * The order handlers for types 0x15 (0x6F6BAAA0) and 0x14 (0x6F6BB050) keep
 * the order container as one 0x1D0-byte ebp-relative local:
 *
 *     +0x00  order descriptor (0x24 bytes, copied by 0x6F6A2C20)
 *     +0x40  +0x44  +0x48  pointer fields
 *     +0x4C  record count
 *     +0x50  12 records of 0x20 bytes
 *
 * 0x6F6B8720 initialises the capacity in [+0x4C] and memsets capacity*0x20
 * bytes, so the 12 is a single constant.  The appenders 0x6F6BB5F0 and
 * 0x6F6BB7A0 write record index [+0x4C] with no bounds check, therefore the
 * container itself must grow to 0x50 + 24*0x20 = 0x350 bytes.  The local is
 * relocated 0x3C4 bytes further down the frame (-0x1e4 -> -0x5a8 for type
 * 0x15, -0x1e0 -> -0x5a4 for type 0x14), which puts the whole container below
 * the other locals at [-0x24c, -0x1e4) and keeps its 0x350-byte span clear of
 * the cookie.
 *
 * The frame size is the container base plus the 0x10 bytes of caller-saved
 * registers the prologue pushes below esp (ebx/esi/edi/eax): 0x5a8 + 0x10 =
 * 0x5b8 for type 0x15 and 0x5a4 + 0x10 = 0x5b4 for type 0x14.  Sizing the frame
 * at the container base alone made those pushes overwrite the container's first
 * three dwords, and the container write then destroyed the saved edi.
 *
 * Every ebp reference inside the old container range shifts by the same
 * amount, so the rewrite is a range shift, not a per-offset map.  The two
 * ranges do not overlap the shifted ranges, which makes the pass idempotent.
 */
#define ORDER_CONTAINER_DELTA 0x3C4u
#define ORDER_CONTAINER_A_LO  0xFFFFFE1Cu  /* [ebp - 0x1e4] = container + 0 */
#define ORDER_CONTAINER_A_HI  0xFFFFFFE8u  /* [ebp - 0x18]  = container+0x1CC */
#define ORDER_CONTAINER_B_LO  0xFFFFFE20u  /* [ebp - 0x1e0] = container + 0 */
#define ORDER_CONTAINER_B_HI  0xFFFFFFECu  /* [ebp - 0x14]  = container+0x1CC */
#define ORDER_HANDLER_A_LO    0x6F6BAAA0u
#define ORDER_HANDLER_A_HI    0x6F6BB050u
#define ORDER_HANDLER_B_LO    0x6F6BB050u
#define ORDER_HANDLER_B_HI    0x6F6BB5F0u
#define VA_ORDER_A_FRAME      0x6F6BAAB1u
#define VA_ORDER_B_FRAME      0x6F6BB061u
#define VA_ORDER_CTOR_CAP     0x6F6B879Au
#define ORDER_B_OLD_FRAME   0x250u
#define ORDER_B_NEW_FRAME   0x9A0u
#define ORDER_C_OLD_FRAME   0x248u
#define ORDER_C_NEW_FRAME   0x9A8u
#define ORDER_OLD_BASE      0xFFFFFE40u /* -0x1c0 */
#define ORDER_NEW_BASE      0xFFFFF6A0u /* -0x960 */
#define ORDER_NEW_LOOP1     0xFFFFF6B8u /* -0x948 */
#define ORDER_NEW_LOOP2     0xFFFFF6BCu /* -0x944 */

/* ------------------------------------------------------------------ */
/* Selection-event diagnostics                                         */
/* ------------------------------------------------------------------ */
/*
 * The second order issued to a group of more than twelve units crashed at
 * 0x6F057C41 ([ecx + 0x54] with ecx == 0).  The path is
 *
 *   0x6F154B60 -> [interface + 0x44] -> 0x6F146C10 -> 0x6F146D30
 *   -> 0x6F15B830 -> [0x6FD53A48 + 0x254] -> 0x6F04D9C0
 *
 * and the handle lookup 0x6F054530 returned zero, which 0x6F04D9C0 passes
 * to 0x6F057C30 without a check.  The lookup is given {handle, serial} from
 * the interface object (+0x14/+0x18); zero means the index is out of range,
 * the slot is free, or the live object's serial no longer matches.
 *
 * The traces below record that pair, the event argument and which lookup
 * check failed.  They are debug-only: release compiles them out.
 *
 * The trace of the reproduced crash shows an out-of-range handle on the
 * order path (handle 0x8308, serial 0xE15F, table count 32020); the lookup
 * returns zero and 0x6F04D9C0 still calls 0x6F057C30 with ecx=0.  The
 * original 0x6F057C30 has no null check because the pair is expected to be
 * live.  Guard the callee once: a null object means there is no field to
 * update, and 0x6F04D9C0 continues with its existing null-safe cleanup.
 */
#define VA_TRACE_EVT2        0x6F146D33u
#define VA_TRACE_LOOKUP_ARG  0x6F04D9E7u
#define VA_TRACE_LOOKUP_RES  0x6F04D9F3u
#define VA_GUARD_NULL        0x6F057C31u
#define VA_GUARD_A60         0x6F689A61u
#define VA_GUARD_ABE0        0x6F67ABE1u

static const char *const INSERT_TRACE_EVT2[] = {
    "    /* selection24 evt2 trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_evt2(c, c->ecx, rd32((c->ebp + 0x00000008u)));",
    "#endif"
};

static const char *const INSERT_TRACE_LOOKUP_ARG[] = {
    "    /* selection24 lookup trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_lookup(c, 0u, c->ecx, c->edx);",
    "#endif"
};

static const char *const INSERT_TRACE_LOOKUP_RES[] = {
    "    /* selection24 lookup result */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_lookup(c, 1u, c->edi, 0u);",
    "#endif"
};

/* 0x6F057C30(ecx=object, arg=value) clears object+0x54 when value is zero;
 * with a failed lookup ecx is also zero.  Returning before the field access
 * keeps the original epilogue (ret 4) and does nothing for a null object. */
static const char *const INSERT_GUARD_NULL[] = {
    "    /* selection24 null-object guard */",
    "    if (c->ecx == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_guard(c, rd32((c->ebp + 0x00000008u)));",
    "#endif",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 8;",
    "        return;",
    "    }"
};

/* 0x6F689A60(ecx=object, 8 stack arguments) is the synchronous event
 * consumer reached from 0x6F69BD80.  The caller tests the result of
 * 0x6F154B60 for [result+0x54]; when that field is zero it still calls this
 * function with esi/ecx=0, and 0x6F689820 writes [ecx+0x24].  The callee has
 * no null path, so return before it builds its frame.  The original tail
 * 0x6F689AB2 ends with `ret 0x20`, so drop the saved ebp, return address and
 * eight arguments (4 + 0x20 bytes).  The caller already has a null-safe
 * cleanup at 0x6F69BE4D and returns eax=esi (zero). */
static const char *const INSERT_GUARD_A60[] = {
    "    /* selection24 event null-object guard */",
    "    if (c->ecx == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_guard(c, rd32((c->ebp + 0x00000008u)));",
    "#endif",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 36;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/* 0x6F67ABE0(ecx=this, [ebp+8]=object) is the generic frame method reached
 * from 0x6F693597 when 0x6F69BD80 returns a null event object.  The null is
 * consumed by 0x6F6BA48A -> 0x6F6B93A0 -> 0x6F6B951E -> 0x6F680320 ->
 * 0x6F693490, and 0x6F67ABE0 then writes [edi+0x20] with edi=0.  The callee
 * has no null path, so return before it builds its frame.  The original tail
 * 0x6F67ACE2 ends with `ret 8`, so drop the saved ebp and two arguments. */
static const char *const INSERT_GUARD_ABE0[] = {
    "    /* selection24 event null-object guard */",
    "    if (rd32((c->ebp + 0x00000008u)) == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_guard(c, rd32((c->ebp + 0x00000008u)));",
    "#endif",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 8;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * Per-unit order trace.  Both batch builders walk an array of nine-dword
 * records and call 0x6F69BD80 / 0x6F69BC80 per record; a zero event object
 * makes the unit fall out of the order without an error.  The unit trace sits
 * at the writer callback entry, the loop trace after the event lookup, where
 * eax is the event object and ecx the target unit.  Both are debug-only: the
 * whole block disappears from a release build.
 *
 * Builder B (entry 0x6F6B98F0) registers writer 0x6F6BA590 and walks its
 * records with the cursor at record+0x18; builder C (entry 0x6F6B9F70)
 * registers writer 0x6F6BA800 and uses the cursor at record+0x1C.  The
 * offsets travel as arguments so the runtime helper never has to guess which
 * frame it is looking at, and the log prints them back as cu=/pair=.
 */
#define VA_TRACE_ORDER_UNIT_B 0x6F6BA59Du
#define VA_TRACE_ORDER_UNIT_C 0x6F6BA80Du
#define VA_TRACE_ORDER_LOOP_B 0x6F6B9E8Du
#define VA_TRACE_ORDER_LOOP_C 0x6F6BA4C2u

static const char *const INSERT_TRACE_ORDER_UNIT_B[] = {
    "    /* selection24 order unit trace B */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_unit(c, 0x38u, c->edi, c->ebx);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_UNIT_C[] = {
    "    /* selection24 order unit trace C */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_unit(c, 0x30u, c->edi, c->ebx);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_LOOP_B[] = {
    "    /* selection24 order loop trace B */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_loop(c, 0x18u, c->esi, c->ecx, c->edx);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_LOOP_C[] = {
    "    /* selection24 order loop trace C */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_loop(c, 0x1Cu, c->esi, c->ecx, c->edx);",
    "#endif"
};

/*
 * The three per-unit assignment lists of the order path.  Two writers append
 * to the class A container (`+0x4C` count, `+0x50` array, stride 0x20):
 * 0x6F6BB5F0 and 0x6F6BB8F0.  The third writer 0x6F6BBEB0 appends to a
 * different container that kept the original capacity: `+0x28` count,
 * `+0x2C` array, also stride 0x20, twelve slots.  The count write is
 * unbounded in all three, so the trace prints the object, the element
 * address, the index derived from it, the counts of both layouts and the
 * unit dword that was just stored.  A list that stays at twelve while the
 * batch holds fourteen points at the container that still has to grow.
 */
#define VA_TRACE_SLOTS_A1 0x6F6BB731u
#define VA_TRACE_SLOTS_A2 0x6F6BB90Cu
#define VA_TRACE_SLOTS_C  0x6F6BBFCCu

static const char *const INSERT_TRACE_SLOTS_A1[] = {
    "    /* selection24 order slots trace A1 */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_slots(c, 1u, c->ebx, c->edi);",
    "#endif"
};

static const char *const INSERT_TRACE_SLOTS_A2[] = {
    "    /* selection24 order slots trace A2 */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_slots(c, 2u, c->ebx, c->edi);",
    "#endif"
};

static const char *const INSERT_TRACE_SLOTS_C[] = {
    "    /* selection24 order slots trace C */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_slots(c, 3u, c->ebx, c->esi);",
    "#endif"
};

/*
 * 0x6F056620(ecx=this, [ebp+8]=object) resolves two {handle, serial} pairs
 * that live at +0x0C/+0x10 of the two interface objects.  It is the fourth
 * consumer of the event target that 0x6F154B60 returns at +0x54, and it has
 * no null path: the first instruction after the prologue reads [ecx+0x10].
 * 0x6F691F20 calls it with esi=0 when the event target is null (0x6F691FB0),
 * which faults at address 0x10.  The callee has no null path, so return
 * before it builds its frame.  The original tail 0x6F05666F ends with
 * `ret 4`, so drop the saved ebp, the return address and the one argument
 * (4 + 4 bytes).  The caller overwrites eax right after the call, so the
 * zero return value is not observable.
 */
#define VA_GUARD_620 0x6F056621u

static const char *const INSERT_GUARD_620[] = {
    "    /* selection24 lookup null-object guard */",
    "    if (c->ecx == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_guard(c, rd32((c->ebp + 0x00000008u)));",
    "#endif",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 8;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * 0x6F691F20 delivers an event to the target object it finds at +0x54 of the
 * event source.  The prologue zeroes esi and only the success path assigns
 * esi = edi, so a missing target reaches the delivery block with esi == 0 and
 * the original code writes [esi+0x28] / [esi+0x24] / [esi+0x30] and then calls
 * 0x6F691E60(0); the `test esi, esi` two instructions later is too late.  The
 * callee guard at 0x6F056620 covers the lookup call but not the writes, so the
 * block is skipped as a whole.  The skipped call is 0x6F056620(ebx), pushed at
 * 0x6F691FAF, and the callee ends with `ret 4`, i.e. it pops its own argument.
 * Leaving the frame without that call therefore has to drop the pushed dword
 * explicitly: the epilogue pops ecx/edi/esi/ebx *before* `mov esp, ebp`, so a
 * stray push shifts all four register saves by one slot (run18: ebx/esi/edi
 * came back as the pushed ebx / saved eax / saved edi and the caller crashed on
 * `[ebx+0x178]`).  The return value is unused at every call site.
 */
#define VA_GUARD_1F20 0x6F691FB0u
#define VA_GUARD_2120 0x6F6921C8u
#define VA_GUARD_2380 0x6F69240Fu
#define VA_GUARD_3FE0 0x6F6740A0u

static const char *const INSERT_GUARD_1F20[] = {
    "    /* selection24 null-target guard */",
    "    if (c->esi == 0u) { c->esp += 4; goto L_6F691FEB; }"
};

static const char *const INSERT_GUARD_2120[] = {
    "    /* selection24 null-target guard 2 */",
    "    if (c->esi == 0u) goto L_6F692244;"
};

static const char *const INSERT_GUARD_2380[] = {
    "    /* selection24 null-target guard 3 */",
    "    if (c->esi == 0u) goto L_6F692451;"
};

/*
 * 0x6F673FE0 is the caller side of the same defect: it takes the event target
 * at 0x6F674079, jumps into the send block when it is null, calls the helper
 * 0x6F689C40 with ecx = esi and then reads [esi+0x30].  The helper guard
 * protects the call but not the read, so the block is left through the same
 * exit the `edi == 0` path uses.  The two arguments pushed for the skipped
 * call are dropped first; `edi` is already zero here, which is exactly the
 * state that exit expects.
 */
static const char *const INSERT_GUARD_3FE0[] = {
    "    /* selection24 null-target guard 4 */",
    "    if (c->esi == 0u) { c->esp += 8; WC3_DIRECT_TAIL(L_6F67413D, c);",
    "        return; }"
};

/*
 * The three send helpers 0x6F689C40 / 0x6F689D40 / 0x6F689F10 take the target
 * object in ecx, store it in esi and then write [esi+0x28].  A null argument
 * faults there in exactly the same way, so they get the same treatment as the
 * delivery block: return before the frame is built.  Each keeps its own stack
 * contract (ret 8 / ret 0x20 / ret 8).
 */
#define VA_SEND_NULL_C40 0x6F689C41u
#define VA_SEND_NULL_D40 0x6F689D41u
#define VA_SEND_NULL_F10 0x6F689F11u

static const char *const INSERT_SEND_NULL_C40[] = {
    "    /* selection24 send null-object guard */",
    "    if (c->ecx == 0u) {",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 12;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

static const char *const INSERT_SEND_NULL_D40[] = {
    "    /* selection24 send null-object guard */",
    "    if (c->ecx == 0u) {",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 36;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

static const char *const INSERT_SEND_NULL_F10[] = {
    "    /* selection24 send null-object guard */",
    "    if (c->ecx == 0u) {",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 12;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * 0x6F66FDD0 is called from 48 places with the object in ecx.  It reads the
 * vtable out of that object and calls through it, so a null object faults at
 * address 0x0 and an object with a cleared vtable faults at 0xEC.  The crash
 * in the order path was 0x6F5FD4DA calling it with ecx = ebx = 0, which is a
 * dead object that the caller did not check.
 *
 * Both guards return zero, which is the failure value of the function: 47 of
 * the 48 call sites test eax and take their failure branch when it is zero,
 * and the remaining one only uses the value.  The stack contract of the
 * function is three arguments plus the return address (its own epilogue skips
 * 0x10 bytes), so the guards drop exactly the same bytes and restore the
 * callee-saved registers the prologue had already pushed.
 */
#define VA_GUARD_FDD0_OBJ 0x6F66FDD1u
#define VA_GUARD_FDD0_VTBL 0x6F66FDE7u

static const char *const INSERT_GUARD_FDD0_OBJ[] = {
    "    /* selection24 dead-object guard */",
    "    if (c->ecx == 0u) {",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 16;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

static const char *const INSERT_GUARD_FDD0_VTBL[] = {
    "    /* selection24 dead-vtable guard */",
    "    if (c->eax == 0u) {",
    "        c->esi = (uint32_t)(pop32(c));",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 16;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * 0x6F691E60 resolves the target of an event: it reads [this+0x178] and
 * [this+0x174] and then writes the resolved {handle, serial} pair through the
 * second argument.  The caller at 0x6F69283D passes the object it got from
 * 0x6F686010, which returns zero when the event source has no target
 * ([this+0x54] == 0, the same dead-object case as the other delivery
 * helpers), so the function is entered with a null object and faults at
 * address 0x178.
 *
 * The guard is placed after the prologue, before the saved esi/edi are pushed,
 * so the stack is unwound exactly like the epilogue does: the saved ebp is
 * popped and the return address plus the one argument are skipped.  The
 * result is zero, which is the honest answer for "no target" - the delivery
 * then reports that nobody received the event instead of dereferencing a
 * dead object.
 */
#define VA_GUARD_E60 0x6F691E61u

static const char *const INSERT_GUARD_E60[] = {
    "    /* selection24 dead-target guard */",
    "    if (c->ecx == 0u) {",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 8;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * 0x6F28EEF0 removes one unit from the selection group: it walks the group's
 * unit list and, when the entry is found, decrements the counter at +0x1F8
 * and unlinks the entry.  A unit that dies but is never removed stays in the
 * selection, keeps its slot in the order batch and later receives an order
 * whose target is a freed object.
 *
 * phase 0 marks the entry of the removal (the group is the argument), phase 1
 * the removal itself, with the group, the removed unit and the counter after
 * the decrement.  A removal that enters the function but never reaches
 * phase 1 means the unit was not found in the group's list.
 */
#define VA_TRACE_REMOVE_ENTRY 0x6F28EF18u
#define VA_TRACE_REMOVE_UNIT 0x6F28EFBBu

static const char *const INSERT_TRACE_REMOVE_ENTRY[] = {
    "    /* selection24 remove entry trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_remove(c, 0u, c->ebx, 0u);",
    "#endif"
};

static const char *const INSERT_TRACE_REMOVE_UNIT[] = {
    "    /* selection24 remove unit trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_remove(c, 1u, c->eax, c->edi);",
    "#endif"
};

/*
 * 0x6F154B60 creates a task object through the virtual factory at
 * [[0x6FD53A48]+0x10] and receives it in eax at 0x6F154B76.  The object's
 * +0x10 class tag, +0x14 handle and +0x18 serial are what the event chain
 * resolves later (L_6F146D30 -> L_6F04D9C0 -> L_6F054530).  The delivery log
 * shows members whose handle is stale (0, 0x5C000) or released (0xFFFFFFFE)
 * while their serial is freshly assigned, so the trace records the object at
 * birth: a stale handle here means the registration never assigned one, a
 * valid handle here means the chain recycles the object later.
 */
#define VA_TRACE_EVBIRTH 0x6F154B76u

static const char *const INSERT_TRACE_EVBIRTH[] = {
    "    /* selection24 event birth trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_evbirth(c, c->eax, c->ebx);",
    "#endif"
};

/*
 * 0x6F154B92 is the join after both initialisation slots of the new task
 * object: the +0x0C slot (0x6F145C10) and the +0x44 slot (0x6F146C10).  edi is
 * the object and ebx the descriptor built by 0x6F055D00.  The +0x54 payload
 * that 0x6F6926B0 later reads is filled somewhere inside this call, so the
 * record pairs the birth state with the state after the slots ran.
 */
#define VA_TRACE_EVINIT 0x6F154B92u

static const char *const INSERT_TRACE_EVINIT[] = {
    "    /* selection24 event init trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_evinit(c, c->edi, c->ebx);",
    "#endif"
};

/*
 * 0x6F691E60 resolves the target pair of an event object.  Its first path
 * (the object's +0x174/+0x178 pair is still -1) checks the out-parameter and
 * jumps to the failure path when that parameter is null.  The continuation
 * chunk L_6F691EC3 is taken when the object already holds a resolved pair;
 * it writes the new pair into the out-parameter at +0x24/+0x28 before the
 * null check runs, so a null out-parameter faults at address 0x24.
 *
 * The caller L_6F692120 passes its reference structure as the out-parameter
 * and treats a null structure as a valid case (it checks it right after the
 * call), so the guard skips the two writes and returns what the failure path
 * returns (eax = esi = 0) through the same epilogue.
 */
#define VA_GUARD_E60B 0x6F691ECAu

/*
 * 0x6F04CDD0 applies the order to one unit and returns 0 when the unit did
 * not take the order.  L_6F69BD80 then keeps esi = 0, so the order record
 * for that unit carries no event and the delivery dereferences a null
 * pointer (SIGSEGV at 0x0 in L_6F6857E0).  With more than twelve selected
 * units the apply fails for the units past the limit, which is the observed
 * "units beyond twelve obey at random" plus the crash.
 *
 * The two trace sites show which branch rejects the unit:
 *   0x6F04CE17 - the order object and its class tag (0x2B616761 expected);
 *   0x6F04CE4C - the result object of the apply and its +0x78 field, which
 *                 has to point back at the order object for success.
 */
#define VA_TRACE_ORDEREV_RES 0x6F69BE1Bu
#define VA_TRACE_ORDEREV_RET 0x6F69BE5Cu

static const char *const INSERT_TRACE_ORDEREV_RES[] = {
    "    /* selection24 orderev result trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_apply(c, 0u, c->eax, c->edi);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDEREV_RET[] = {
    "    /* selection24 orderev event trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_apply(c, 1u, c->esi, c->edi);",
    "#endif"
};

static const char *const INSERT_GUARD_E60B[] = {
    "    /* selection24 null-reference guard */",
    "    if (c->esi == 0u) {",
    "        c->edi = (uint32_t)(pop32(c));",
    "        c->esi = (uint32_t)(pop32(c));",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 8;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * 0x6F48CC03 is the selection object's handler for the message 0x41777270.
 * It resolves the object's weak (handle, serial) pair and hands the result to
 * the order/UI path, which reads [target+0x34] and calls through the target's
 * vtable (fault inside L_6F48CC56 when the slot was recycled).
 *
 * The pair resolves successfully - handle and serial still match a live table
 * entry - but the entry's +0x54 no longer names the class the caller expects.
 * The engine's own object invariant is checked here before the pointer is used:
 * a game object carries its vtable in the first dword, inside the image range.
 * A pointer that fails the test is replaced with zero, and the existing null
 * branch at 0x6F48CC2B (flags_logicz on edi) then takes the "no target" exit.
 */
#define VA_GUARD_TAG 0x6F48CC29u

static const char *const INSERT_GUARD_TAG[] = {
    "    /* selection24 weak-pair target check */",
    "    {",
    "        uint32_t _sel24_vt = 0u;",
    "        uint32_t _sel24_ok = 0u;",
    "        if (c->edi >= 0x40000000u && c->edi < 0x70000000u) {",
    "            _sel24_vt = rd32((c->edi));",
    "            _sel24_ok = 1u;",
    "        }",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_stale(c, 1u, c->edi, _sel24_vt, _sel24_ok);",
    "        if (_sel24_ok != 0u)",
    "            wc3_sel24_trace_stale(c, 4u, rd32((c->edi + 0x00000034u)), rd32((c->edi + 0x00000020u)), rd32((c->edi + 0x00000018u)));",
    "#endif",
    "        if (c->edi < 0x40000000u || c->edi >= 0x70000000u ||",
    "            _sel24_vt < 0x6F000000u || _sel24_vt >= 0x71000000u)",
    "            c->edi = 0u;",
    "    }"
};

/*
 * The remaining twelve-slots constants.  These loops walk either the bit mask
 * of selected panel slots (`inc counter; rol bitmask,1; cmp counter, 0xc`) or
 * the group's unit list, and one computes the number of free slots as
 * 12 - selected.  Every one of them truncates the selection at twelve, so units
 * past the twelfth never reach the panel, the order loop or the slot update -
 * which is the observed "units beyond twelve obey at random".
 */
#define VA_CAP_SLOT_MASK_A 0x6F26B522u
#define VA_CAP_SLOT_MASK_B 0x6F3DEBEEu
#define VA_CAP_SLOT_MASK_C 0x6F66BEFAu
#define VA_CAP_SLOT_MASK_D 0x6F69D217u
#define VA_CAP_SLOT_MASK_E 0x6F3DB037u
#define VA_CAP_SLOT_UNIT_LIST 0x6F3DBE14u
#define VA_CAP_SLOT_PANEL_A 0x6F68C0CBu
#define VA_CAP_SLOT_PANEL_B 0x6F3C2E33u
#define VA_CAP_SLOT_FREE 0x6F39737Bu

/*
 * 0x6F05458B and 0x6F05457D are the two tails of the handle lookup: each reads
 * the slot object `[array + index*8 + 4]` and immediately compares its serial at
 * +0x18.  A slot whose object was released keeps the live tag in [slot] while
 * [slot+4] is zero, so the compare faults at address 0x18; a later call through
 * the same stale reference jumps into unmapped memory.  The guard returns the
 * function's own "not found" answer - eax = 0 through the same epilogue.
 */
#define VA_GUARD_SLOT_A 0x6F054590u
#define VA_GUARD_SLOT_B 0x6F05457Du

static const char *const INSERT_GUARD_SLOT[] = {
    "    /* selection24 released-slot guard */",
    "    if (c->ecx == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_stale(c, 2u, c->eax, c->ebx, c->ecx);",
    "#endif",
    "        c->ebx = (uint32_t)(pop32(c));",
    "        c->eax = 0u;",
    "        c->esp += 4;",
    "        return;",
    "    }"
};

/*
 * 0x6F6857E0 delivers an order event to a unit.  It reads its argument
 * straight away (`mov eax, [ecx]`), so a missing event faults at address 0x0.
 * The event is missing when the order builder could not create one for that
 * unit - which happens exactly for the units past the engine's own limit.
 * The guard returns the function's failure answer (eax = 0) through the same
 * epilogue; the caller checks for zero and skips the unit.
 */
#define VA_GUARD_EVENT 0x6F6857E9u

static const char *const INSERT_GUARD_EVENT[] = {
    "    /* selection24 missing order event guard */",
    "    if (c->ecx == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_stale(c, 3u, c->ecx, c->eax, c->edi);",
    "#endif",
    "        c->eax = 0u;",
    "        c->edi = (uint32_t)(pop32(c));",
    "        c->esi = (uint32_t)(pop32(c));",
    "        c->ebx = (uint32_t)(pop32(c));",
    "        c->esp = c->ebp;",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 8; return;",
    "    }"
};

/*
 * 0x6F6B93A0 hands the order event to the unit through a virtual call on the
 * event object: `event->vtbl[0x1C](event)` at 0x6F6B93E7.  The handler is the
 * code that decides what the unit does with the order, so the unit-side
 * decision is visible only here.  Stage 0 prints the unit, the event, its
 * vtable and the handler address; stage 1 prints the handler result, which
 * 0x6F04CDD0 then resolves (the `apply` trace prints that result).
 */
#define VA_TRACE_ORDER_EXEC_1 0x6F6B93E5u
#define VA_TRACE_ORDER_EXEC_2 0x6F6B93EAu

static const char *const INSERT_TRACE_ORDER_EXEC_1[] = {
    "    /* selection24 order exec trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_exec(c, 0u, rd32((c->ebp + 0xFFFFFFECu)),",
    "                               c->ecx, c->edx, rd32((c->edx + 0x0000001Cu)),",
    "                               0u);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_EXEC_2[] = {
    "    /* selection24 order exec result trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_exec(c, 1u, rd32((c->ebp + 0xFFFFFFECu)),",
    "                               c->ebx, 0u, 0u, c->eax);",
    "#endif"
};

/*
 * Point-order / formation tracing (PASS_REV 44).
 *
 * The request traces sit after `mov ebx, ecx` in the two point-order handlers:
 * 0x6F6B991E is message 0x12 (builder B) and 0x6F6B9F9E is message 0x13
 * (builder C).  The list trace sits before the PrClGen pointer write at
 * 0x6F169670, on the `cmovne eax, esi` line at 0x6F16966D, because the write
 * line itself is a group24 shift site and cannot also be an insertion anchor.
 * The solver trace sits after `mov edi, ecx` at 0x6F16B306, before the
 * index/pair workspace is formed.  The cluster growth prepare and capacity
 * trace share the entry of the member-vector insertion at 0x6F16C077; the
 * prepare activates the existing grow path for the 24-unit profile, then the
 * trace records the capacity the original comparison is about to use.  The
 * result trace sits after the 0x6F16C060 call at 0x6F16B7ED, where eax is the
 * value the original code ignores.
 */
#define VA_TRACE_ORDER_BEGIN_B 0x6F6B991Eu
#define VA_TRACE_ORDER_BEGIN_C 0x6F6B9F9Eu
#define VA_TRACE_LISTADD       0x6F16966Du
#define VA_TRACE_FORMATION     0x6F16B306u
#define VA_TRACE_CLUSTER_TRY   0x6F16C077u
#define VA_TRACE_CLUSTER_INS   0x6F16B7EDu
#define VA_TRACE_A5B9          0x6F16A5B9u
#define VA_TRACE_CF94_BASE     0x6F16D00Fu

static const char *const INSERT_TRACE_ORDER_BEGIN_B[] = {
    "    /* selection24 point-order begin trace B */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_begin(c, c->ebx, 0u);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_BEGIN_C[] = {
    "    /* selection24 point-order begin trace C */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_begin(c, c->ebx, 1u);",
    "#endif"
};

static const char *const INSERT_TRACE_LISTADD[] = {
    "    /* selection24 PrClGen list-add trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_listadd(c, c->edi, c->edx, c->eax,",
    "                            rd32((c->ebp + 0x00000008u)),",
    "                            rd32((c->ebp + 0x0000000Cu)));",
    "#endif"
};

static const char *const INSERT_TRACE_FORMATION[] = {
    "    /* selection24 formation entry trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_formation_begin(c, c->ecx);",
    "#endif"
};

static const char *const INSERT_TRACE_CLUSTER_TRY[] = {
    "    /* selection24 cluster growth prepare */",
    "    wc3_sel24_cluster_prepare(c->esi);",
    "    /* selection24 cluster capacity trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_cluster_try(c, c->esi, c->edi);",
    "#endif"
};

static const char *const INSERT_TRACE_CLUSTER_INS[] = {
    "    /* selection24 cluster insertion trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_cluster_insert(c,",
    "        rd32((c->ebp + 0x00000008u)), c->edi, c->ebx, c->esi, c->eax);",
    "#endif"
};

static const char *const INSERT_TRACE_A5B9[] = {
    "    /* selection24 group builder entry trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_base(c, c->ecx, 0u, 0u, 0u);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_BASE[] = {
    "    /* selection24 ground-order base trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_base(c, rd32((c->ebp + 0xFFFFFFFCu)),",
    "                                rd32((c->ebp + 0x00000008u)),",
    "                                rd32((c->ebx)), c->esi);",
    "#endif"
};

/*
 * 0x6F6B93A0(unit, event) delivers the per-unit order event in two phases.
 * Each phase calls 0x6F04CDD0 and skips the unit entirely when the result is
 * zero, which is the only silent per-unit drop in the order path.  The trace
 * is inserted right after those calls, so the log states for every unit
 * whether the order was accepted.  The unit is the first argument and the
 * event object the second; the frame still holds them at [ebp-0x14] and
 * [ebp-0x10].
 */
#define VA_TRACE_ORDER_APPLY   0x6F6B93EEu
#define VA_TRACE_ORDER_APPLY_2 0x6F6B9426u

static const char *const INSERT_TRACE_ORDER_APPLY[] = {
    "    /* selection24 order apply trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_apply(c, rd32((c->ebp + 0xFFFFFFECu)),",
    "                               rd32((c->ebp + 0xFFFFFFF0u)), c->eax, 0u);",
    "#endif"
};

static const char *const INSERT_TRACE_ORDER_APPLY_2[] = {
    "    /* selection24 order apply trace 2 */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_order_apply(c, rd32((c->ebp + 0xFFFFFFECu)),",
    "                               rd32((c->ebp + 0xFFFFFFF0u)), c->eax, 1u);",
    "#endif"
};

/*
 * P0-6 return-address elision replaces the return-address push with the same
 * stack adjustment, so the call line keeps its meaning but loses the
 * `push32(c, 0x...u);` prefix.  The trace inserted below reads [ebp-*] and
 * eax only, never the return slot, so both forms anchor the same event.
 */
static const char *const TRACE_ORDER_APPLY_ELIDED =
    "c->esp -= 4; WC3_DIRECT_CALL(L_6F04CDD0, c);";

/*
 * 0x6F689FC0 is the event-target filler called by 0x6F6926B0 with the target
 * that the task factory returned at [result+0x54].  When that field is zero
 * the caller still calls this function with ecx = 0, and the first write is
 * [ecx+0x28] - the "signal 11 at 0x28" of the 24-unit order path.  The
 * function has no null path: every consumer of the target assumes it exists.
 * Return through the original epilogue (`ret 0x14` plus the return address)
 * so the caller continues with its own null-safe cleanup.
 */
#define VA_GUARD_FC0 0x6F689FC1u

static const char *const INSERT_GUARD_FC0[] = {
    "    /* selection24 null-event guard */",
    "    if (c->ecx == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_guard(c, rd32((c->ebp + 0x00000008u)));",
    "#endif",
    "        c->ebp = (uint32_t)(pop32(c));",
    "        c->esp += 24;",
    "        c->eax = 0u;",
    "        return;",
    "    }"
};

/*
 * 0x6F691E60(manager, target) is the next consumer of the same target.  It
 * loads [ebp+8] and writes [target+0x28]/[target+0x24] before its own
 * `test esi, esi`; the null branch it reaches two instructions later (the
 * shared exit 0x6F691EFE) is the path the engine wants.  Jump there directly
 * when the target is null, so the guard only diverts the crashing case.
 */
#define VA_GUARD_E60_ENTRY 0x6F691E7Cu

static const char *const INSERT_GUARD_E60_ENTRY[] = {
    "    /* selection24 null-task guard */",
    "    if (c->esi == 0u) {",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "        wc3_sel24_trace_guard(c, 0u);",
    "#endif",
    "        WC3_DIRECT_TAIL(L_6F691EFE, c);",
    "        return;",
    "    }"
};

/*
 * 0x6F1CBF40 is the handle-table slot allocator.  Its free-list head is -1
 * when the list is empty and a slot index otherwise; a slot that is in use
 * carries the reserved marker 0xFFFFFFFE ("no link").  When the head points
 * at such a slot the pop leaves 0xFFFFFFFE in the head, and the next
 * allocation computes base + (-2)*8 -- an out-of-bounds write sixteen bytes
 * before the slot array -- and returns 0xFFFFFFFE as the handle itself.
 * The order event stored that value in [event+0x14]; the handle lookup then
 * left [event+0x54] null and the order path faulted on the null payload.
 * The marker is not a valid index, so treat any negative head as an empty
 * list and let the append path hand out a fresh slot.
 */
#define VA_FREELIST_HEAD 0x6F1CBF4Du

static const char *const INSERT_FREELIST_HEAD[] = {
    "    /* selection24 freelist head normalize */",
    "    if (c->esi != 0xFFFFFFFFu && (c->esi & 0x80000000u) != 0u) {",
    "        c->esi = 0xFFFFFFFFu;",
    "        flags_sub(c, c->esi, 0xFFFFFFFFu, 4);",
    "    }"
};

/*
 * 0x6F67DDF1 asks the selection group for its unit count and turns
 * `count >= 0xC` into a boolean that 0x6F680630 forwards to 0x6F528110,
 * which uses it to pick the group handling branch.  With a 24-unit
 * selection the threshold is the selection capacity itself, not 12, so a
 * 13..23 unit group must keep the ordinary branch instead of being treated
 * as a full group that the 12-slot path knows how to drain.
 */
#define VA_ORDER_GROUP_FULL 0x6F67DDF1u

static const char *const INSERT_ORDER_GROUP_FULL[] = {
    "    flags_sub(c, c->eax, 0x18u, 4); /* selection24 group-full threshold */"
};

/*
 * 0x6F1CBFB0 is the release wrapper of the handle table: a negative handle
 * indexes the negative slot vector at +0x20 and 0x6F1CBFF0 pushes the index
 * onto the free list at +0x44 (`entry[index].value = *head; *head = index`).
 * Releasing one index twice links it to itself, the next pop sets the head to
 * 0xFFFFFFFE, and the allocator then writes one eight-byte record before the
 * slot array and returns 0xFFFFFFFE as a handle -- exactly the value seen in
 * the crash of the 22-unit order.  The record prints the slot value so the
 * second release of a live index is visible.
 */
#define VA_TRACE_EVRELEASE 0x6F1CBFB3u

static const char *const INSERT_TRACE_EVRELEASE[] = {
    "    /* selection24 event release trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_evrelease(c, c->ecx, c->edx);",
    "#endif"
};

/*
 * 0x6F1CBE60 binds an event to an explicit slot index.  Both callers pass the
 * free-list head of their table as the fourth argument (0x6F1CBEF2 pushes
 * `table+0x40`, 0x6F1CBEC0 pushes `table+0x44`), but the lifted body never
 * reads it: the slot is marked 0xFFFFFFFE (occupied) while it stays linked in
 * the free list.  The next pop of that table then follows the chain into an
 * occupied slot, takes its 0xFFFFFFFE as the next head, and the allocator
 * finally writes one eight-byte record before the slot array and returns
 * 0xFFFFFFFE as a handle.
 *
 * The insertion walks the free list before the slot is marked and unlinks the
 * index, which is the operation the missing argument implies: a slot that is
 * handed out must not stay free.  `ecx` already holds the slot base and `esi`
 * the index at this point; the head pointer is the fourth argument.
 */
#define VA_SLOT_UNLINK 0x6F1CBE99u

static const char *const INSERT_SLOT_UNLINK[] = {
    "    /* selection24 free-list unlink */",
    "    {",
    "        uint32_t sel24_headp = rd32((c->ebp + 0x00000010u));",
    "        uint32_t sel24_prev = 0xFFFFFFFFu;",
    "        uint32_t sel24_cur = sel24_headp != 0u ? rd32(sel24_headp) : 0xFFFFFFFFu;",
    "        unsigned sel24_hops = 0u;",
    "        while (sel24_cur != 0xFFFFFFFFu && sel24_hops++ < 4096u) {",
    "            if (sel24_cur == c->esi) {",
    "                uint32_t sel24_next = rd32((c->ecx + c->esi * 8u));",
    "                if (sel24_prev == 0xFFFFFFFFu)",
    "                    wr32(sel24_headp, sel24_next);",
    "                else",
    "                    wr32((c->ecx + sel24_prev * 8u), sel24_next);",
    "                break;",
    "            }",
    "            sel24_prev = sel24_cur;",
    "            sel24_cur = rd32((c->ecx + sel24_cur * 8u));",
    "        }",
    "    }"
};

/*
 * 0x6F6926B0 builds the order task and reads its payload at [factory+0x54].
 * A zero payload silently drops the order for this unit: the `je` right after
 * the test jumps to the shared epilogue, so the trace has to sit before that
 * branch.  edi is the payload, eax still holds the wrapper, and the descriptor
 * lives at [ebp-0x3C].
 */
#define VA_TRACE_TASKNULL 0x6F692727u

static const char *const INSERT_TRACE_TASKNULL[] = {
    "    /* selection24 task null trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    wc3_sel24_trace_tasknull(c, c->eax);",
    "#endif"
};

/*
 * 0x6F69273F is the second silent drop of 0x6F6926B0: the class-instance check
 * 0x6F04CDD0 returned zero and the function jumps to the shared epilogue
 * without touching the unit.  edi is the task wrapper, [ebp-0x48] the class tag
 * that was resolved for it, and [ebp+8]/[ebp+0xC] the caller's arguments.
 */
#define VA_TRACE_ORDERDROP 0x6F69273Fu

static const char *const INSERT_TRACE_ORDERDROP[] = {
    "    /* selection24 order drop trace */",
    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
    "    if (c->eax == 0u) wc3_sel24_trace_orderdrop(",
    "        c, c->edi, rd32((c->ebp + 0xFFFFFFB8u)), c->eax);",
    "#endif"
};

static const char *const INIT_GROUP_DISPATCH =
    "push32(c, 0x6F39EDA9u); dispatch(c, 0x6F3B91B0u);";
static const char *const INIT_GROUP_DIRECT =
    "push32(c, 0x6F39EDA9u); WC3_DIRECT_CALL(L_6F3B91B0, c);";

static const char *const INSERT_INIT_LINES[] = {
    "    wc3_sel24_init_group(c, c->edi); "
    "/* 6F39EDA4: selection24 grid24 init */"
};

static const char *const INSERT_SELECT_LINES[] = {
    "    wc3_sel24_select(c, rd32((c->ebp + 0xFFFFFFE0u)), "
    "rd32((c->ebp + 0xFFFFFFDCu))); "
    "/* 6F3C7DBB: selection24 mode */"
};

static const char *const INSERT_NOTE_LINES[] = {
    "    wc3_sel24_note_selection(c, rd32((c->ebp + 0xFFFFFFE0u)), c->esi); "
    "/* 6F3C7D35: selection24 total */"
};

static const char *const INSERT_HIDE[] = {
    /* The insertion is emitted after `mov ebx, ecx`, so ebx is the group
     * `this` pointer.  Keep that argument explicit: the helper signature is
     * wc3_sel24_hide_buttons(c, group), not a raw caller register. */
    "    wc3_sel24_hide_buttons(c, c->ebx); "
    "/* 6F3ACCA3: selection24 active grid */",
    "    c->edi = (uint32_t)(pop32(c));                               "
    "/* 6F3ACCF2: pop edi */",
    "    c->esi = (uint32_t)(pop32(c));                               "
    "/* 6F3ACCF3: pop esi */",
    "    c->ebx = (uint32_t)(pop32(c));                               "
    "/* 6F3ACCF4: pop ebx */",
    "    c->esp += 4; return;                                         "
    "/* 6F3ACCF5: ret  */"
};

static const char INSERT_FILL[] =
    "    wc3_sel24_fill(c, rd32((c->ebp + 0xFFFFFFE0u)), c->ebx, c->esi, "
    "rd32((c->ebp + 0xFFFFFFE4u))); /* 6F3C7E78: selection24 fill */";

static const char *const FILL_CALL_DIRECT =
    "push32(c, 0x6F3C7E99u); WC3_DIRECT_CALL(L_6F3DA470, c);";

static const char *const FILL_CODE[13] = {
    "push32(c, rd32((c->ebp + 0xFFFFFFE4u)));",
    "c->eax = 0xAAAAAAABu;",
    "{ uint64_t _p=(uint64_t)c->eax*(uint32_t)(c->ebx); "
    "c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); }",
    "push32(c, c->esi);",
    "c->edx = op_shr_nf(c->edx, 0x2u, 4);",
    "c->eax = c->ebx;",
    "push32(c, c->edx);",
    "c->ecx = (uint32_t)((c->edx + c->edx*2));",
    "c->ecx = add32(c->ecx, c->ecx, 4);",
    "c->eax = flags_sub(c, c->eax, c->ecx, 4);",
    "c->ecx = rd32((c->ebp + 0xFFFFFFE0u));",
    "push32(c, c->eax);",
    "push32(c, 0x6F3C7E99u); dispatch(c, 0x6F3DA470u);"
};

static const uint32_t FILL_VA[13] = {
    0x6F3C7E78u, 0x6F3C7E7Bu, 0x6F3C7E80u, 0x6F3C7E82u,
    0x6F3C7E83u, 0x6F3C7E86u, 0x6F3C7E88u, 0x6F3C7E89u,
    0x6F3C7E8Cu, 0x6F3C7E8Eu, 0x6F3C7E90u, 0x6F3C7E93u,
    0x6F3C7E94u
};

static int is_fill_va(uint32_t va)
{
    int i;

    for (i = 0; i < 13; ++i)
        if (FILL_VA[i] == va)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Site handlers                                                       */
/* ------------------------------------------------------------------ */

static int handle_subst(Buf *out, const char *p, const char *e,
                        const char *old_code, const char *new_code,
                        const char *old_comment, const char *new_comment,
                        Kind k, uint32_t va, int line, Counts *counts,
                        Sites *sites, int *handled, char *err, size_t errsz)
{
    int has_old = contains_sub(p, (size_t)(e - p), old_code);
    int has_new = contains_sub(p, (size_t)(e - p), new_code);
    char before[65];
    char after[65];
    Buf tmp = {0};
    const char *action;

    hash_hex(p, (size_t)(e - p), before);

    if (has_old && has_new) {
        snprintf(err, errsz,
                 "site 0x%08X line %d contains both '%s' and '%s'",
                 va, line, old_code, new_code);
        return -1;
    }
    if (!has_old && !has_new) {
        snprintf(err, errsz,
                 "site 0x%08X line %d does not match '%s'", va, line, old_code);
        return -1;
    }

    if (buf_append(&tmp, p, (size_t)(e - p)) != 0) {
        buf_free(&tmp);
        return -1;
    }

    if (has_old) {
        char *cur = NULL;
        size_t curlen = 0;
        char *next = NULL;
        size_t nextlen = 0;
        int r;

        r = replace_once((char *)p, (size_t)(e - p), old_code, new_code,
                         &cur, &curlen);
        if (r != 1) {
            snprintf(err, errsz,
                     "site 0x%08X line %d: ambiguous substitution", va, line);
            free(cur);
            buf_free(&tmp);
            return -1;
        }
        free(tmp.p);
        tmp.p = cur;
        tmp.n = curlen;
        tmp.cap = curlen + 1;

        if (old_comment && new_comment &&
            contains_sub(tmp.p, tmp.n, old_comment)) {
            r = replace_once(tmp.p, tmp.n, old_comment, new_comment,
                             &next, &nextlen);
            if (r != 1) {
                snprintf(err, errsz,
                         "site 0x%08X line %d: ambiguous comment edit",
                         va, line);
                free(next);
                buf_free(&tmp);
                return -1;
            }
            free(tmp.p);
            tmp.p = next;
            tmp.n = nextlen;
            tmp.cap = nextlen + 1;
        }
        action = "replace";
    } else {
        /* Already patched.  A stale comment is repaired, otherwise the line
         * is copied byte-for-byte. */
        action = "already";
        if (old_comment && new_comment &&
            contains_sub(tmp.p, tmp.n, old_comment)) {
            char *next = NULL;
            size_t nextlen = 0;
            int r = replace_once(tmp.p, tmp.n, old_comment, new_comment,
                                 &next, &nextlen);
            if (r != 1) {
                snprintf(err, errsz,
                         "site 0x%08X line %d: ambiguous comment edit",
                         va, line);
                free(next);
                buf_free(&tmp);
                return -1;
            }
            free(tmp.p);
            tmp.p = next;
            tmp.n = nextlen;
            tmp.cap = nextlen + 1;
        }
    }

    hash_hex(tmp.p, tmp.n, after);
    if (buf_append(out, tmp.p, tmp.n) != 0) {
        buf_free(&tmp);
        return -1;
    }
    buf_free(&tmp);
    counts->n[k]++;
    sites_add(sites, k, va, action, line, before, after);
    *handled = 1;
    return 0;
}

static int handle_insert(Buf *out, const char **pp, const char *end,
                         const char *anchor, const char *anchor_alt,
                         const char *marker,
                         const char *const *ins, int ins_count,
                         Kind k, uint32_t va, int line, Counts *counts,
                         Sites *sites, int *handled, char *err, size_t errsz)
{
    const char *p = *pp;
    const char *le = line_end(p, end);
    const char *ns;
    const char *ne;
    size_t tlen;
    char before[65];
    char after[65];
    char insbuf[1024];
    size_t inslen = 0;
    int i;
    int already;
    const char *term;

    if (!code_equals(p, le, anchor) &&
        (!anchor_alt || !code_equals(p, le, anchor_alt))) {
        snprintf(err, errsz,
                 "site 0x%08X line %d does not match anchor '%s'",
                 va, line, anchor);
        return -1;
    }
    hash_hex(p, (size_t)(le - p), before);

    ns = le;
    ne = line_end(ns, end);
    already = contains_sub(ns, (size_t)(ne - ns), marker);

    if (buf_append(out, p, (size_t)(le - p)) != 0)
        return -1;

    tlen = term_len(p, le);
    term = (tlen == 2) ? "\r\n" : "\n";
    if (tlen == 0)
        term = "\n";

    if (already) {
        if (buf_append(out, ns, (size_t)(ne - ns)) != 0)
            return -1;
        *pp = ne;
        hash_hex(ns, (size_t)(ne - ns), after);
        sites_add(sites, k, va, "already", line, before, after);
    } else {
        for (i = 0; i < ins_count; ++i) {
            size_t l = strlen(ins[i]);

            if (inslen + l + 3 > sizeof(insbuf)) {
                snprintf(err, errsz, "selection24 insertion text too large");
                return -1;
            }
            memcpy(insbuf + inslen, ins[i], l);
            inslen += l;
            memcpy(insbuf + inslen, term, strlen(term));
            inslen += strlen(term);
        }
        hash_hex(insbuf, inslen, after);
        if (buf_append(out, insbuf, inslen) != 0)
            return -1;
        *pp = le;
        sites_add(sites, k, va, "insert", line, before, after);
    }

    counts->n[k]++;
    *handled = 1;
    return 0;
}

static int handle_fill_block(Buf *out, const char **pp, const char *end,
                             int line, Counts *counts, Sites *sites,
                             int *handled, char *err, size_t errsz)
{
    const char *p = *pp;
    const char *le = line_end(p, end);
    const char *q;
    const char *term;
    size_t tlen;
    char before[65];
    char after[65];
    int i;

    if (contains_sub(p, (size_t)(le - p), "wc3_sel24_fill")) {
        const char *ns = le;
        const char *ne = line_end(ns, end);
        uint32_t nv;

        if (parse_va(ns, ne, &nv) && is_fill_va(nv) &&
            code_equals(ns, ne, FILL_CODE[0])) {
            snprintf(err, errsz,
                     "site 0x6F3C7E78 line %d: helper line followed by the "
                     "original fill block", line);
            return -1;
        }
        hash_hex(p, (size_t)(le - p), before);
        if (buf_append(out, p, (size_t)(le - p)) != 0)
            return -1;
        sites_add(sites, K_FILL_BLOCK, VA_FILL_BLOCK, "already", line,
                  before, before);
        counts->n[K_FILL_BLOCK]++;
        *pp = le;
        *handled = 1;
        return 0;
    }

    if (!code_equals(p, le, FILL_CODE[0])) {
        snprintf(err, errsz,
                 "site 0x6F3C7E78 line %d does not match the fill block",
                 line);
        return -1;
    }

    hash_hex(p, (size_t)(le - p), before);
    q = p;
    for (i = 0; i < 13; ++i) {
        const char *qe = line_end(q, end);
        uint32_t v = 0;
        int match = code_equals(q, qe, FILL_CODE[i]);

        if (!match && i == 12)
            match = code_equals(q, qe, FILL_CALL_DIRECT);
        if (!parse_va(q, qe, &v) || v != FILL_VA[i] || !match) {
            snprintf(err, errsz,
                     "fill block line %d (0x%08X) does not match template",
                     i + 1, FILL_VA[i]);
            return -1;
        }
        q = qe;
    }

    tlen = term_len(p, le);
    term = (tlen == 2) ? "\r\n" : "\n";
    if (tlen == 0)
        term = "\n";
    if (buf_append(out, INSERT_FILL, strlen(INSERT_FILL)) != 0)
        return -1;
    if (buf_append(out, term, strlen(term)) != 0)
        return -1;
    hash_hex(INSERT_FILL, strlen(INSERT_FILL), after);
    sites_add(sites, K_FILL_BLOCK, VA_FILL_BLOCK, "block", line,
              before, after);
    counts->n[K_FILL_BLOCK]++;
    *pp = q;
    *handled = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Buffer processing                                                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Point-order container relocation (rev 31)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;
    uint32_t lo;    /* lowest old displacement (container + 0)          */
    uint32_t hi;    /* highest old displacement (container + 0x1CC)     */
    uint32_t delta;
} ShiftSpan;

static int parse_hex8(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < 8; ++i) {
        int d = hex_digit((unsigned char)s[i]);

        if (d < 0)
            return 0;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 1;
}

/* Recognize "void L_XXXXXXXX(CPU *restrict c)" block definitions. */
static int parse_block_va(const char *p, const char *e, uint32_t *out)
{
    static const char def[] = "void L_";
    static const char tail[] = "CPU *restrict c)";
    const char *q;
    uint32_t v;

    if ((size_t)(e - p) <
        (sizeof(def) - 1) + 8 + 1 + (sizeof(tail) - 1))
        return 0;
    if (memcmp(p, def, sizeof(def) - 1) != 0)
        return 0;
    q = p + sizeof(def) - 1;
    if (!parse_hex8(q, &v) || q[8] != '(')
        return 0;
    if (memcmp(q + 9, tail, sizeof(tail) - 1) != 0)
        return 0;
    *out = v;
    return 1;
}

/*
 * Rewrite every reference to the relocated container inside one line: the
 * 32-bit ebp displacement in the lifted code and the matching "[ebp - 0x..]"
 * address comment.  Values already inside the shifted range are counted as
 * "already" so a second pass reports the same site count.
 */
static int shift_container_line(const char *p, const char *e,
                                const ShiftSpan *sp, Buf *out,
                                int *replaced, int *already)
{
    static const char code_pat[] = "c->ebp + 0x";
    static const char cmt_pat[] = "[ebp - 0x";
    const size_t code_len = sizeof(code_pat) - 1;
    const size_t cmt_len = sizeof(cmt_pat) - 1;
    const char *q = p;
    char num[16];

    *replaced = 0;
    *already = 0;

    while (q < e) {
        const char *code = find_sub(q, (size_t)(e - q), code_pat);
        const char *cmt = find_sub(q, (size_t)(e - q), cmt_pat);
        const char *hit;
        int is_code;

        if (!code && !cmt)
            break;
        is_code = (code && (!cmt || code < cmt));
        hit = is_code ? code : cmt;

        if (is_code) {
            const char *d = hit + code_len;
            const char *tok = d;
            uint32_t v = 0;
            int in_old = 0;
            int in_new = 0;

            if (d + 9 <= e && d[8] == 'u' && parse_hex8(d, &v)) {
                tok = d + 8;
                in_old = (v >= sp->lo && v <= sp->hi);
                in_new = (v >= sp->lo - sp->delta &&
                          v <= sp->hi - sp->delta);
            }
            if (in_old) {
                if (buf_append(out, q, (size_t)(d - q)) != 0)
                    return -1;
                snprintf(num, sizeof(num), "%08X",
                         (unsigned)(v - sp->delta));
                if (buf_puts(out, num) != 0)
                    return -1;
                ++*replaced;
            } else {
                if (in_new)
                    ++*already;
                if (buf_append(out, q, (size_t)(tok - q)) != 0)
                    return -1;
            }
            q = tok;
            continue;
        }

        {
            const char *d = hit + cmt_len;
            const char *digits = d;
            uint32_t v = 0;
            size_t used = 0;
            int in_old = 0;
            int in_new = 0;

            while (digits < e && hex_digit((unsigned char)*digits) >= 0) {
                v = (v << 4) | (uint32_t)hex_digit((unsigned char)*digits);
                ++digits;
            }
            if (digits > d && digits - d <= 8 && digits < e &&
                *digits == ']') {
                uint32_t disp = 0u - v;

                used = (size_t)(digits - d) + 1;
                in_old = (disp >= sp->lo && disp <= sp->hi);
                in_new = (disp >= sp->lo - sp->delta &&
                          disp <= sp->hi - sp->delta);
            }
            if (in_old) {
                if (buf_append(out, q, (size_t)(d - q)) != 0)
                    return -1;
                snprintf(num, sizeof(num), "%x",
                         (unsigned)(v + sp->delta));
                if (buf_puts(out, num) != 0)
                    return -1;
                if (buf_append(out, digits, 1) != 0)
                    return -1;
                q = digits + 1;
                ++*replaced;
                continue;
            }
            if (in_new)
                ++*already;
            if (used) {
                if (buf_append(out, q, (size_t)(d - q) + used) != 0)
                    return -1;
                q = d + used;
            } else {
                if (buf_append(out, q, (size_t)(d - q)) != 0)
                    return -1;
                q = d;
            }
        }
    }
    if (buf_append(out, q, (size_t)(e - q)) != 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PrClGen 12 -> 24 layout shift                                       */
/*                                                                     */
/* The order pipeline's formation/cluster generator keeps its unit     */
/* candidates in a fixed container with 12 slots at +0x1C (stride      */
/* 0xC), 12 unit pointers at +0xAC (stride 4) and the target/point     */
/* block at +0xDC.  The 13th candidate runs past the pointer array and */
/* overwrites the object's own handle at +0x14.  The whole layout is   */
/* shifted to 24 slots: the slots keep +0x1C and stride 0xC, the       */
/* pointers move to +0x13C, the tail moves by +0xC0 and the element    */
/* size grows from 0x108 to 0x1C8.  The pool element is a 4-byte       */
/* free-list link followed by the object, so the object itself grows   */
/* from 0x104 to 0x1C4 and the flags land at +0x1C0, inside the new    */
/* element.  Only literal displacements change;                        */
/* the lifted control flow is identical.  wc3_sel24_cap() keeps the    */
/* old 12 when WC3_SELECTION_24 is not set, so the default build keeps */
/* the original 12-unit limit.                                         */
/*                                                                     */
/* Rev 42 adds the second root: the CPrCluster member-vector capacity  */
/* (0x6F14FCAF) and the 12-entry solver workspace L_6F16B2F0.  The      */
/* solver frame and index/pair arrays are widened unconditionally to   */
/* 24 entries; with the feature off the behaviour for <=12 units is    */
/* unchanged, but the frame/offset layout differs from the pre-pass     */
/* corpus.  The original 12-entry sort L_6F16DFD0 stays in place for   */
/* reference; both live callers now use the 24-entry helpers.           */
/*                                                                     */
/* Rev 45 widens the remaining 12-entry selection-group path: the      */
/* 16-element array in L_6F16A5F8/L_6F16A6E0, its frame/local          */
/* displacements, the group fill (L_6F16CB80), the tail writer         */
/* (L_6F16D270) and the duplicated stride block (L_6F16D2D3/D2DA/      */
/* D37D).  The group entry becomes count +0, index[24] +4, pair[24]    */
/* +0x64, tail +0x124; the element size grows 0xA4 -> 0x134 and the    */
/* frame 0xA54 -> 0x1354.  L_6F16A6E0 now calls                       */
/* wc3_sel24_group_sort() instead of the 12-entry L_6F16DFD0, so a    */
/* 14-unit selection can no longer write pair 12/13 over the tail at    */
/* +0x94/+0x9C.  The debug-only group snapshot wrappers log the entry   */
/* around init/sort under WC3_DEBUG_SELECT24.                          */
/*                                                                     */
/* Rev 59 replaces the formation-table reads in L_6F16D270/D286/D2D3   */
/* with wc3_sel24_formation_columns().  The 12-entry table at           */
/* 0x6FA91D20/0x6FA91D88 is followed by a vtable, so count 13 read      */
/* B[0]/C[0] = 0 and the order loop never advanced its step.  The       */
/* helper returns the original word for count <= 12 and a defined 13..24 */
/* row length otherwise (compact 3, grid ceil(count/3)).               */
/*                                                                     */
/* Every row is one anchor VA from                                    */
/* docs/research/SELECTION_24_SITES.tsv plus the exact operand text    */
/* the lift emits.  All copies of a duplicated block are patched, so   */
/* the coverage totals count lines, not addresses.                     */
typedef struct {
    uint32_t va;
    const char *old_code;
    const char *new_code;
} Group24Site;

static const Group24Site GROUP24_SITES[] = {
    /* CPrCluster member-vector capacity: 12 -> runtime cap. */
    { 0x6F14FCAFu, "0xCu", "wc3_sel24_cap()" },
    { 0x6F157A27u, "0x108u", "0x1C8u" },
    { 0x6F169231u, "0xBu", "wc3_sel24_cap() - 1" },
    { 0x6F169282u, "0x000000E4u", "0x000001A4u" },
    { 0x6F16928Cu, "0x000000E0u", "0x000001A0u" },
    { 0x6F169296u, "0x000000DCu", "0x0000019Cu" },
    { 0x6F1692A0u, "0x000000E4u", "0x000001A4u" },
    { 0x6F1692AFu, "0x000000E8u", "0x000001A8u" },
    { 0x6F1692BAu, "0x000000ECu", "0x000001ACu" },
    { 0x6F1692C0u, "0x000000F0u", "0x000001B0u" },
    { 0x6F1692CAu, "0x000000F4u", "0x000001B4u" },
    { 0x6F1692D4u, "0x000000F8u", "0x000001B8u" },
    { 0x6F1692E3u, "0x000000FCu", "0x000001BCu" },
    { 0x6F1692EBu, "0x00000100u", "0x000001C0u" },
    { 0x6F1695F5u, "0x00000100u", "0x000001C0u" },
    { 0x6F169670u, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16991Eu, "0x000000E0u", "0x000001A0u" },
    { 0x6F16992Au, "0x000000DCu", "0x0000019Cu" },
    { 0x6F169939u, "0x000000E8u", "0x000001A8u" },
    { 0x6F16993Fu, "0x000000ECu", "0x000001ACu" },
    { 0x6F169945u, "0x000000F0u", "0x000001B0u" },
    { 0x6F16994Fu, "0x000000F4u", "0x000001B4u" },
    { 0x6F169959u, "0x000000F8u", "0x000001B8u" },
    { 0x6F169968u, "0x000000FCu", "0x000001BCu" },
    { 0x6F16996Eu, "0x00000100u", "0x000001C0u" },
    { 0x6F169C68u, "0xCu", "wc3_sel24_cap()" },
    { 0x6F169C70u, "0x000000ACu", "0x0000013Cu" },
    { 0x6F169D71u, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16A07Cu, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16A08Fu, "0x000000E0u", "0x000001A0u" },
    { 0x6F16A095u, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16A0A9u, "0x000000ECu", "0x000001ACu" },
    { 0x6F16A0BEu, "0x000000E8u", "0x000001A8u" },
    { 0x6F16A0DDu, "0x000000FCu", "0x000001BCu" },
    { 0x6F16A0FBu, "0x00000100u", "0x000001C0u" },
    { 0x6F16A13Au, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16A1FAu, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16A251u, "0x000000E0u", "0x000001A0u" },
    { 0x6F16A2A8u, "0x000000E8u", "0x000001A8u" },
    { 0x6F16A2FFu, "0x000000ECu", "0x000001ACu" },
    { 0x6F16A356u, "0x000000FCu", "0x000001BCu" },
    { 0x6F16A3ADu, "0x00000100u", "0x000001C0u" },
    /* Selection-group workspace L_6F16A5F8/L_6F16A6E0: 0xA4 -> 0x134. */
    { 0x6F16A5B3u, "0xA54u", "0x1354u" },
    { 0x6F16A5CDu, "0xFFFFF5B4u", "0xFFFFECB4u" },
    { 0x6F16A5F8u, "0xFFFFF5B8u", "0xFFFFECB8u" },
    { 0x6F16A5FEu, "0xFFFFF5BCu", "0xFFFFECBCu" },
    { 0x6F16A604u, "0xFFFFF5B0u", "0xFFFFECB0u" },
    { 0x6F16A612u,
      "c->esp -= 4; WC3_DIRECT_CALL(L_6F169300, c);",
      "wc3_sel24_group_init((uint32_t)(c->esi + 0x64u));" },
    { 0x6F16A617u, "0xA4u", "0x134u" },
    { 0x6F16A620u, "0xFFFFF5BCu", "0xFFFFECBCu" },
    { 0x6F16A62Fu, "0xFFFFF5B8u", "0xFFFFECB8u" },
    { 0x6F16A638u, "0xFFFFF5BCu", "0xFFFFECBCu" },
    { 0x6F16A644u, "0xFFFFF5B8u", "0xFFFFECB8u" },
    { 0x6F16A64Au, "0xFFFFF5BCu", "0xFFFFECBCu" },
    /* Rev 62: dump the sixteen counts at the sort-loop entry. */
    { 0x6F16A650u,
      "c->ebx = 0x10u;",
      "wc3_sel24_trace_counts(c->edi); c->ebx = 0x10u;" },
    { 0x6F16A65Au, "0xFFFFF5B0u", "0xFFFFECB0u" },
    { 0x6F16A662u, "0xFFFFF5B4u", "0xFFFFECB4u" },
    /* 6F16A672 reloads the same selection object after L_6F16A6E0.  It was
     * missed in Rev 57: the old slot -0xA4C now aliases index[10] of group 7
     * inside the widened 16 x 0x134 array and reads zero. */
    { 0x6F16A672u, "0xFFFFF5B4u", "0xFFFFECB4u" },
    { 0x6F16A678u, "0xFFFFF5B0u", "0xFFFFECB0u" },
    { 0x6F16A692u, "0xFFFFF5ACu", "0xFFFFECACu" },
    { 0x6F16A69Au, "0xFFFFF5B8u", "0xFFFFECB8u" },
    { 0x6F16A6A0u, "0xFFFFF5B8u", "0xFFFFECB8u" },
    { 0x6F16A6AEu, "0xFFFFF5B0u", "0xFFFFECB0u" },
    { 0x6F16A6B4u, "0xA4u", "0x134u" },
    { 0x6F16A6BDu, "0xFFFFF5B4u", "0xFFFFECB4u" },
    { 0x6F16A6EFu,
      "push32(c, 0x6F16A6F4u); WC3_DIRECT_CALL(L_6F16DFD0, c);",
      "wc3_sel24_group_sort(c->edi); c->esp += 4;" },
    { 0x6F16A6FBu, "0x00000094u", "0x00000124u" },
    { 0x6F16A72Eu, "0x0000009Cu", "0x0000012Cu" },
    { 0x6F16A75Fu, "0x0000009Cu", "0x0000012Cu" },
    { 0x6F16A765u, "0x00000094u", "0x00000124u" },
    { 0x6F16AB30u, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16AB52u, "0x000000E8u", "0x000001A8u" },
    { 0x6F16AB5Fu, "0x000000ECu", "0x000001ACu" },
    { 0x6F16AB6Cu, "0x000000FCu", "0x000001BCu" },
    { 0x6F16AB79u, "0x00000100u", "0x000001C0u" },
    { 0x6F16AB90u, "0x000000F4u", "0x000001B4u" },
    { 0x6F16AB9Bu, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16ABA1u, "0x000000F8u", "0x000001B8u" },
    { 0x6F16ABA7u, "0x000000F0u", "0x000001B0u" },
    { 0x6F16ABB7u, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16ADDEu, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16ADECu, "0x000000E0u", "0x000001A0u" },
    { 0x6F16ADF9u, "0x000000E4u", "0x000001A4u" },
    { 0x6F16AE06u, "0x000000E8u", "0x000001A8u" },
    { 0x6F16AE13u, "0x000000ECu", "0x000001ACu" },
    { 0x6F16AE20u, "0x000000FCu", "0x000001BCu" },
    { 0x6F16AE2Du, "0x00000100u", "0x000001C0u" },
    { 0x6F16AE3Au, "0x000000F4u", "0x000001B4u" },
    { 0x6F16AE53u, "0x000000F8u", "0x000001B8u" },
    { 0x6F16AE63u, "0xACu", "0x13Cu" },
    { 0x6F16AE6Cu, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16AFF0u, "0x000000F4u", "0x000001B4u" },
    { 0x6F16B000u, "0x000000F4u", "0x000001B4u" },
    /* Ground-order solver workspace L_6F16B2F0: 12 -> 24 entries. */
    { 0x6F16B2F3u, "0x108u", "0x240u" },
    { 0x6F16B308u, "0xFFFFFF58u", "0xFFFFFDC0u" },
    { 0x6F16B314u,
      "c->esp -= 4; WC3_DIRECT_CALL_CLEAN(L_6F169300, c);",
      "wc3_sel24_init24((uint32_t)(c->ebp + 0xFFFFFE24u));" },
    { 0x6F16B330u, "0xFFFFFF58u", "0xFFFFFDC0u" },
    { 0x6F16B47Cu, "0xFFFFFF5Cu", "0xFFFFFDC4u" },
    { 0x6F16B496u, "0xFFFFFF8Cu", "0xFFFFFE24u" },
    { 0x6F16B4B7u, "0xFFFFFF90u", "0xFFFFFE28u" },
    { 0x6F16B4BCu, "0xFFFFFF58u", "0xFFFFFDC0u" },
    { 0x6F16B4D4u, "0xFFFFFF58u", "0xFFFFFDC0u" },
    { 0x6F16B4DDu,
      "push32(c, 0x6F16B4E2u); WC3_DIRECT_CALL_LEAF(L_6F16DFD0, c);",
      "wc3_sel24_sort24(c->eax); c->esp += 4;" },
    { 0x6F16B4E2u, "0xFFFFFF58u", "0xFFFFFDC0u" },
    { 0x6F16B4F7u, "0xFFFFFF90u", "0xFFFFFE28u" },
    { 0x6F16B500u, "0xFFFFFF58u", "0xFFFFFDC0u" },
    { 0x6F16B551u, "0xFFFFFF8Cu", "0xFFFFFE24u" },
    { 0x6F16B558u, "0xFFFFFF5Cu", "0xFFFFFDC4u" },
    { 0x6F16B78Du, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16B7CCu, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16B7FEu, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16B809u, "0x00000100u", "0x000001C0u" },
    { 0x6F16B8F9u, "0x00000100u", "0x000001C0u" },
    { 0x6F16B924u, "0xACu", "0x13Cu" },
    { 0x6F16BA90u, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16BB2Cu, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16BD3Du, "0x000000E8u", "0x000001A8u" },
    { 0x6F16BD89u, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16BDBCu, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16BDE0u, "0x000000E0u", "0x000001A0u" },
    { 0x6F16BDE6u, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16BDF7u, "0x000000E8u", "0x000001A8u" },
    { 0x6F16BE03u, "0x000000F0u", "0x000001B0u" },
    { 0x6F16BE10u, "0x000000F4u", "0x000001B4u" },
    { 0x6F16BE1Du, "0x000000F8u", "0x000001B8u" },
    { 0x6F16BE2Au, "0x000000FCu", "0x000001BCu" },
    { 0x6F16BE35u, "0x00000100u", "0x000001C0u" },
    { 0x6F16BE70u, "0x000000E0u", "0x000001A0u" },
    { 0x6F16BE76u, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16BF83u, "0x00000100u", "0x000001C0u" },
    { 0x6F16BFF0u, "0x00000100u", "0x000001C0u" },
    { 0x6F16C8EEu, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16C8F3u, "0x000000ACu", "0x0000013Cu" },
    /* Selection-group fill L_6F16CB80 and tail writer L_6F16D270. */
    { 0x6F16CBC2u, "0x000000A4u", "0x00000134u" },
    /* Rev 62: dump the sixteen counts right after the fill's zero loop. */
    { 0x6F16CBCBu,
      "wr32((c->ebp + 0xFFFFFFFCu), c->ebx);",
      "wc3_sel24_trace_counts(c->edx); wr32((c->ebp + 0xFFFFFFFCu), c->ebx);" },
    { 0x6F16CBFBu, "0xA4u", "0x134u" },
    { 0x6F16CC8Cu, "0x29u", "0x4Du" },
    { 0x6F16CCCAu, "0x00000034u", "0x00000064u" },
    { 0x6F16CD02u, "0x00000038u", "0x00000068u" },
    /* Formation table 0x6FA91D20/0x6FA91D88 is 13 entries per block and is
     * followed immediately by the list-object vtable at 0x6FA91DF4, so it
     * cannot be widened in place.  The read moves to a host helper that
     * returns the original table word for count <= 12 and the defined 24-unit
     * row length for 13..24.  eax = 13 * flag + count, ecx = count. */
    { 0x6F16D2CAu,
      "c->edi = rd32((c->eax*4 + GVA(0x6FA91D20)));",
      "c->edi = wc3_sel24_formation_columns(c->eax, c->ecx, 0u);" },
    { 0x6F16D2D3u,
      "c->edi = rd32((c->eax*4 + GVA(0x6FA91D88)));",
      "c->edi = wc3_sel24_formation_columns(c->eax, c->ecx, 1u);" },
    { 0x6F16D35Fu, "0x00000098u", "0x00000128u" },
    { 0x6F16D368u, "0x000000A0u", "0x00000130u" },
    { 0x6F16D371u, "0x00000094u", "0x00000124u" },
    { 0x6F16D377u, "0x0000009Cu", "0x0000012Cu" },
    { 0x6F16D37Du, "0xA4u", "0x134u" },
    { 0x6F16D5D0u, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16D5D9u, "0xCu", "wc3_sel24_cap()" },
    { 0x6F16D634u, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16D818u, "0x000000FCu", "0x000001BCu" },
    { 0x6F16D877u, "0x000000ACu", "0x0000013Cu" },
    { 0x6F16D8B3u, "0x000000F8u", "0x000001B8u" },
    { 0x6F16D8CAu, "0x000000F8u", "0x000001B8u" },
    { 0x6F16DA1Du, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16DA26u, "0x000000E0u", "0x000001A0u" },
    { 0x6F16DA2Cu, "0x00000100u", "0x000001C0u" },
    { 0x6F16DA3Au, "0x000000E0u", "0x000001A0u" },
    { 0x6F16DA44u, "0x000000DCu", "0x0000019Cu" },
    { 0x6F16DA4Eu, "0x00000100u", "0x000001C0u" },
    { 0x6F16DAF6u, "0x000000F4u", "0x000001B4u" },
    { 0x6F16DB78u, "0x000000E8u", "0x000001A8u" },
    { 0x6F16DB81u, "0x000000ECu", "0x000001ACu" },
    { 0x6F16DC09u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC17u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC39u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC44u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC59u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC64u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC79u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC84u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DC99u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DCA4u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DCB9u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DCC4u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DD09u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DD17u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DDB9u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DDC7u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DDE9u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DDF7u, "0x00000100u", "0x000001C0u" },
    { 0x6F16DE26u, "0x000000F0u", "0x000001B0u" },
    /* Rev 60: the per-batch insertion sort L_6F16E090/L_6F16E0C0 is a
     * second 12-entry sort of the same group pair array.  Rev 45 moved the
     * pair array from +0x34/+0x38 to +0x64/+0x68 but missed this function,
     * so each order batch sorted index[12..] instead of the pairs.  With 13+
     * units the index array was overwritten by float key/value words and the
     * unit pointer `index * 0x2C + cluster_base` went out of bounds. */
    { 0x6F16E0AFu, "0x00000038u", "0x00000068u" },
    { 0x6F16E11Au, "0x00000034u", "0x00000064u" },
    { 0x6F16E127u, "0x00000038u", "0x00000068u" },
    { 0x6F16E137u, "0x00000034u", "0x00000064u" },
    { 0x6F16E13Eu, "0x00000038u", "0x00000068u" }
};

/* The call-macro spelling of the two helper call sites depends on the
 * earlier passes: a fresh w3l_compat chunk emits WC3_DIRECT_CALL, while an
 * older selection24 output carried WC3_DIRECT_CALL_CLEAN/_LEAF.  Both forms
 * are accepted so the pass stays idempotent across pass-order changes. */
static const Group24Site GROUP24_ALT_SITES[] = {
    { 0x6F16B314u,
      "c->esp -= 4; WC3_DIRECT_CALL(L_6F169300, c);",
      "wc3_sel24_init24((uint32_t)(c->ebp + 0xFFFFFE24u));" },
    { 0x6F16B4DDu,
      "push32(c, 0x6F16B4E2u); WC3_DIRECT_CALL(L_6F16DFD0, c);",
      "wc3_sel24_sort24(c->eax); c->esp += 4;" },
    { 0x6F16A612u,
      "c->esp -= 4; WC3_DIRECT_CALL_CLEAN(L_6F169300, c);",
      "wc3_sel24_group_init((uint32_t)(c->esi + 0x64u));" },
    { 0x6F16A6EFu,
      "push32(c, 0x6F16A6F4u); WC3_DIRECT_CALL_LEAF(L_6F16DFD0, c);",
      "wc3_sel24_group_sort(c->edi); c->esp += 4;" }
};

static const Group24Site *group24_alt_lookup(uint32_t va)
{
    size_t i;

    for (i = 0; i < sizeof(GROUP24_ALT_SITES) / sizeof(GROUP24_ALT_SITES[0]);
         ++i) {
        if (GROUP24_ALT_SITES[i].va == va)
            return &GROUP24_ALT_SITES[i];
    }
    return NULL;
}

static const Group24Site *group24_lookup(uint32_t va)
{
    int lo = 0;
    int hi = (int)(sizeof(GROUP24_SITES) / sizeof(GROUP24_SITES[0])) - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (GROUP24_SITES[mid].va == va)
            return &GROUP24_SITES[mid];
        if (GROUP24_SITES[mid].va < va)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* Replace every occurrence of old_s.  A single lifted instruction can
 * touch the same field twice (an or-memory pseudo-instruction becomes a
 * read plus a write), so a strictly single-shot substitution would give
 * up on those lines. */
static int replace_all(const char *hay, size_t n, const char *old_s,
                       const char *new_s, char **outp, size_t *outn)
{
    size_t olen = strlen(old_s);
    size_t nlen = strlen(new_s);
    size_t i = 0;
    size_t start = 0;
    Buf b = {0};

    while (i + olen <= n) {
        if (memcmp(hay + i, old_s, olen) == 0) {
            if (buf_append(&b, hay + start, i - start) != 0 ||
                buf_append(&b, new_s, nlen) != 0) {
                buf_free(&b);
                return -1;
            }
            i += olen;
            start = i;
        } else {
            ++i;
        }
    }
    if (buf_append(&b, hay + start, n - start) != 0) {
        buf_free(&b);
        return -1;
    }
    *outp = b.p;
    *outn = b.n;
    return 0;
}

static int handle_group24(Buf *out, const char *p, const char *e,
                          const Group24Site *g, int line, Counts *counts,
                          Sites *sites, int *handled, char *err, size_t errsz)
{
    size_t n = (size_t)(e - p);
    int has_old = contains_sub(p, n, g->old_code);
    int has_new = contains_sub(p, n, g->new_code);
    char before[65];
    char after[65];
    char *res = NULL;
    size_t resn = 0;

    if (has_old && has_new) {
        /* A replacement may legitimately contain the old text (a trace
         * prefix in front of the original statement).  Only an old
         * occurrence outside the new text is a real conflict. */
        const char *hit = find_sub(p, n, g->new_code);
        size_t nlen = strlen(g->new_code);
        int leftover = 1;

        if (hit) {
            leftover = find_sub(p, (size_t)(hit - p), g->old_code) != NULL ||
                       find_sub(hit + nlen,
                                n - (size_t)(hit + nlen - p),
                                g->old_code) != NULL;
        }
        if (leftover) {
            snprintf(err, errsz,
                     "site 0x%08X line %d contains both '%s' and '%s'",
                     g->va, line, g->old_code, g->new_code);
            return -1;
        }
        has_old = 0; /* already applied; the old text is inside the new */
    }
    if (!has_old && !has_new) {
        snprintf(err, errsz, "site 0x%08X line %d does not match '%s'",
                 g->va, line, g->old_code);
        return -1;
    }

    hash_hex(p, n, before);
    if (has_old) {
        if (replace_all(p, n, g->old_code, g->new_code, &res, &resn) != 0) {
            snprintf(err, errsz, "site 0x%08X line %d: out of memory",
                     g->va, line);
            return -1;
        }
    } else {
        res = (char *)malloc(n + 1);
        if (!res) {
            snprintf(err, errsz, "site 0x%08X line %d: out of memory",
                     g->va, line);
            return -1;
        }
        memcpy(res, p, n);
        res[n] = '\0';
        resn = n;
    }

    hash_hex(res, resn, after);
    if (buf_append(out, res, resn) != 0) {
        free(res);
        return -1;
    }
    free(res);
    counts->n[K_GROUP24]++;
    sites_add(sites, K_GROUP24, g->va, has_old ? "replace" : "already",
              line, before, after);
    *handled = 1;
    return 0;
}

static int process_buffer(const char *data, size_t n, const char *profile,
                          Buf *out, Counts *counts, Sites *sites,
                          char *err, size_t errsz)
{
    const char *p = data;
    const char *end = data + n;
    int line = 1;
    ShiftSpan span;
    uint32_t cur_block = 0;
    int block_site = 0;

    span.active = 0;
    span.lo = 0;
    span.hi = 0;
    span.delta = ORDER_CONTAINER_DELTA;

    if (err && errsz)
        err[0] = '\0';
    memset(counts, 0, sizeof(*counts));

    while (p < end) {
        const char *le = line_end(p, end);
        const char *next = le;
        uint32_t va = 0;
        uint32_t block_va = 0;
        int has_va = parse_va(p, le, &va);
        int handled = 0;

        if (parse_block_va(p, le, &block_va)) {
            cur_block = block_va;
            block_site = 0;
            span.active = 0;
            if (block_va >= ORDER_HANDLER_A_LO && block_va < ORDER_HANDLER_A_HI) {
                span.active = 1;
                span.lo = ORDER_CONTAINER_A_LO;
                span.hi = ORDER_CONTAINER_A_HI;
            } else if (block_va >= ORDER_HANDLER_B_LO &&
                       block_va < ORDER_HANDLER_B_HI) {
                span.active = 1;
                span.lo = ORDER_CONTAINER_B_LO;
                span.hi = ORDER_CONTAINER_B_HI;
            }
        } else if (le - p >= 7 && memcmp(p, "void L_", 7) == 0) {
            /* A definition the lift emitted with another signature: do not
             * carry the previous block's relocation state into it. */
            cur_block = 0;
            block_site = 0;
            span.active = 0;
        }

        if (has_va) {
            switch (va) {
            case VA_GROUP_ALLOC:
                if (handle_subst(out, p, le,
                                 "0x134u", "0x144u",
                                 "push 0x134 */", "push 0x144 */",
                                 K_GROUP_ALLOC, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_INIT_GROUP:
                next = p;
                if (handle_insert(out, &next, end,
                                  INIT_GROUP_DISPATCH, INIT_GROUP_DIRECT,
                                  "wc3_sel24_init_group", INSERT_INIT_LINES, 1,
                                  K_INIT_GROUP, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_SELECT:
                next = p;
                if (handle_insert(out, &next, end,
                                  "wr32((c->ebp + 0xFFFFFFF0u), c->edi);",
                                  NULL,
                                  "wc3_sel24_select", INSERT_SELECT_LINES, 1,
                                  K_SELECT, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_OUTER:
                if (handle_subst(out, p, le,
                                 "(uint32_t)(0xCu)",
                                 "(uint32_t)(wc3_sel24_cap())",
                                 NULL, NULL,
                                 K_CAP_OUTER, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_INNER:
                if (handle_subst(out, p, le,
                                 "0xCu", "wc3_sel24_cap()",
                                 NULL, NULL,
                                 K_CAP_INNER, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_FILL_BLOCK:
                next = p;
                if (handle_fill_block(out, &next, end, line, counts, sites,
                                      &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_FILL_GRID:
                if (handle_subst(out, p, le,
                                 "0x00000130u", "0x00000138u",
                                 "[ecx + 0x130] */", "[ecx + 0x138] */",
                                 K_FILL_GRID, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_NOTE_SELECTION:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = rd32((c->eax + 0x00000034u));",
                                  NULL,
                                  "wc3_sel24_note_selection", INSERT_NOTE_LINES, 1,
                                  K_NOTE_SELECTION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_HIDE_ALL:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebx = c->ecx;",
                                  NULL,
                                  "wc3_sel24_hide_buttons",
                                  INSERT_HIDE, 5,
                                  K_HIDE_ALL, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ADDUNIT_CAP: {
                /* The trace is inserted after the patched comparison and is
                 * compiled out unless WC3_DEBUG_SELECT24 is enabled.  It is
                 * part of the same site: the cap substitution remains the one
                 * counted change, and the insertion is idempotent because the
                 * marker line is already present on a second pass. */
                static const char *const TRACE_LINES[] = {
                    "#if WC3_DEBUG && WC3_DEBUG_SELECT24",
                    "    wc3_sel24_trace_addunit(c, c->ebx, c->edi, c->eax); "
                    "/* selection24 addunit trace */",
                    "#endif"
                };
                const char *term;
                size_t tlen;
                size_t ti;
                const char *scan;
                int trace_present = 0;
                int scan_i;

                if (handle_subst(out, p, le,
                                 "0xCu", "wc3_sel24_cap()",
                                 NULL, NULL,
                                 K_ADDUNIT_CAP, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                /* The trace block starts with #if and carries its marker on
                 * the helper line, so inspect the whole short block rather
                 * than only the immediately following line. */
                scan = le;
                for (scan_i = 0; scan_i < 4 && scan < end; ++scan_i) {
                    const char *se = line_end(scan, end);

                    if (contains_sub(scan, (size_t)(se - scan),
                                     "wc3_sel24_trace_addunit")) {
                        trace_present = 1;
                        break;
                    }
                    scan = se;
                }
                if (!trace_present) {
                    tlen = term_len(p, le);
                    term = (tlen == 2) ? "\r\n" : "\n";
                    if (tlen == 0)
                        term = "\n";
                    for (ti = 0;
                         ti < sizeof(TRACE_LINES) / sizeof(TRACE_LINES[0]);
                         ++ti) {
                        if (buf_append(out, TRACE_LINES[ti],
                                       strlen(TRACE_LINES[ti])) != 0 ||
                            buf_append(out, term, strlen(term)) != 0)
                            return -1;
                    }
                }
                break;
            }
            case VA_CAP_LOCAL_LIST:
                if (handle_subst(out, p, le,
                                 "push32(c, 0xCu);",
                                 "push32(c, wc3_sel24_cap());",
                                 NULL, NULL,
                                 K_CAP_LOCAL_LIST, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_COPY_LIST:
                if (handle_subst(out, p, le,
                                 "c->edx = 0xCu;",
                                 "c->edx = wc3_sel24_cap();",
                                 NULL, NULL,
                                 K_CAP_COPY_LIST, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_SYNC_ADD:
                if (handle_subst(out, p, le,
                                 "flags_sub(c, rd32((c->ebx + 0x00000010u)), "
                                 "0xCu, 4);",
                                 "flags_sub(c, rd32((c->ebx + 0x00000010u)), "
                                 "wc3_sel24_cap(), 4);",
                                 NULL, NULL,
                                 K_CAP_SYNC_ADD, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_NET_RECV:
                if (handle_subst(out, p, le,
                                 "flags_sub(c, c->eax, 0xCu, 4);",
                                 "flags_sub(c, c->eax, wc3_sel24_cap(), 4);",
                                 NULL, NULL,
                                 K_CAP_NET_RECV, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_NET_APPLY:
                if (handle_subst(out, p, le,
                                 "c->edx = 0xCu;",
                                 "c->edx = wc3_sel24_cap();",
                                 NULL, NULL,
                                 K_CAP_NET_APPLY, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_NET_SEND:
            case VA_CAP_NET_SEND_B:
                if (handle_subst(out, p, le,
                                 "c->ebx = 0xCu;",
                                 "c->ebx = wc3_sel24_cap();",
                                 NULL, NULL,
                                 K_CAP_NET_SEND, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_SELECT_ALL:
                if (handle_subst(out, p, le,
                                 "push32(c, 0xCu);",
                                 "push32(c, wc3_sel24_cap());",
                                 NULL, NULL,
                                 K_CAP_SELECT_ALL, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_GROUP_STORE_A:
            case VA_CAP_GROUP_STORE_B:
                if (handle_subst(out, p, le,
                                 "flags_sub(c, c->esi, 0xCu, 4);",
                                 "flags_sub(c, c->esi, wc3_sel24_cap(), 4);",
                                 NULL, NULL,
                                 K_CAP_GROUP_STORE, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_GROUP_STORE_C:
                if (handle_subst(out, p, le,
                                 "flags_sub(c, c->eax, 0xCu, 4);",
                                 "flags_sub(c, c->eax, wc3_sel24_cap(), 4);",
                                 NULL, NULL,
                                 K_CAP_GROUP_STORE, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_ADD_MERGE_A:
                if (handle_subst(out, p, le,
                                 "c->edx = 0xCu;",
                                 "c->edx = wc3_sel24_cap();",
                                 NULL, NULL,
                                 K_CAP_ADD_MERGE, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_ADD_MERGE_B:
                if (handle_subst(out, p, le,
                                 "wr32((c->ebp + 0xFFFFFFD8u), 0xCu);",
                                 "wr32((c->ebp + 0xFFFFFFD8u), "
                                 "wc3_sel24_cap());",
                                 NULL, NULL,
                                 K_CAP_ADD_MERGE, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_ADD_MERGE_C:
                if (handle_subst(out, p, le,
                                 "c->ecx = 0xCu;",
                                 "c->ecx = wc3_sel24_cap();",
                                 NULL, NULL,
                                 K_CAP_ADD_MERGE, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_A_FRAME:
                if (handle_subst(out, p, le,
                                 "0x24Cu", "0x5B8u",
                                 "sub esp, 0x24c */", "sub esp, 0x5b8 */",
                                 K_ORDER_OBJ_CONTAINER, va, line, counts,
                                 sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_B_FRAME:
                if (handle_subst(out, p, le,
                                 "0x248u", "0x5B4u",
                                 "sub esp, 0x248 */", "sub esp, 0x5b4 */",
                                 K_ORDER_OBJ_CONTAINER, va, line, counts,
                                 sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CTOR_CAP:
                if (handle_subst(out, p, le,
                                 "0xCu", "0x18u",
                                 "mov dword ptr [esi + 0x4c], 0xc */",
                                 "mov dword ptr [esi + 0x4c], 0x18 */",
                                 K_ORDER_OBJ_CONTAINER, va, line, counts,
                                 sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_FRAME_B:
                if (handle_subst(out, p, le,
                                 "0x250u", "0x9A0u",
                                 "sub esp, 0x250 */", "sub esp, 0x9a0 */",
                                 K_ORDER_FRAME, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_FRAME_C:
                if (handle_subst(out, p, le,
                                 "0x248u", "0x9A8u",
                                 "sub esp, 0x248 */", "sub esp, 0x9a8 */",
                                 K_ORDER_FRAME, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_SORT1:
            case VA_ORDER_SORT2:
                if (handle_subst(out, p, le,
                                 "0xFFFFFE40u", "0xFFFFF6A0u",
                                 "[ebp - 0x1c0] */", "[ebp - 0x960] */",
                                 K_ORDER_SORT, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_LOOP1:
                if (handle_subst(out, p, le,
                                 "0xFFFFFE58u", "0xFFFFF6B8u",
                                 "[ebp - 0x1a8] */", "[ebp - 0x948] */",
                                 K_ORDER_LOOP, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_LOOP2:
                if (handle_subst(out, p, le,
                                 "0xFFFFFE5Cu", "0xFFFFF6BCu",
                                 "[ebp - 0x1a4] */", "[ebp - 0x944] */",
                                 K_ORDER_LOOP, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CB_PTR:
                if (handle_subst(out, p, le,
                                 "c->ecx = (uint32_t)((c->edi + c->eax*4));",
                                 "c->ecx = (uint32_t)((c->edi + c->eax*4 + "
                                 "0xFFFFF860u));",
                                 NULL, NULL,
                                 K_ORDER_CALLBACK, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CB_BASE:
                if (handle_subst(out, p, le,
                                 "wr32((c->edi + c->eax*4 + 0x00000044u), "
                                 "c->ebx);",
                                 "wr32((c->edi + c->eax*4 + 0xFFFFF8A4u), "
                                 "c->ebx);",
                                 NULL, NULL,
                                 K_ORDER_CALLBACK, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CB_FIELD1C:
                if (handle_subst(out, p, le,
                                 "wr32((c->edi + c->eax*4 + 0x00000060u), "
                                 "c->edx);",
                                 "wr32((c->edi + c->eax*4 + 0xFFFFF8C0u), "
                                 "c->edx);",
                                 NULL, NULL,
                                 K_ORDER_CALLBACK, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CB_FIELD04:
                if (handle_subst(out, p, le,
                                 "wr32((c->edi + c->ecx*4 + 0x00000048u), "
                                 "c->eax);",
                                 "wr32((c->edi + c->ecx*4 + 0xFFFFF8A8u), "
                                 "c->eax);",
                                 NULL, NULL,
                                 K_ORDER_CALLBACK, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CB_B_PTR:
                if (handle_subst(out, p, le,
                                 "c->esi = (uint32_t)((c->edi + c->eax*4));",
                                 "c->esi = (uint32_t)((c->edi + c->eax*4 + "
                                 "0xFFFFF860u));",
                                 NULL, NULL,
                                 K_ORDER_CALLBACK_B, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_CB_B_FLD0C:
                if (handle_subst(out, p, le,
                                 "wr32((c->edi + c->ecx*4 + 0x00000048u), "
                                 "c->eax);",
                                 "wr32((c->edi + c->ecx*4 + 0xFFFFF8A8u), "
                                 "c->eax);",
                                 NULL, NULL,
                                 K_ORDER_CALLBACK_B, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_EVT2:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esp = flags_sub(c, c->esp, 0xCu, 4);",
                                  NULL,
                                  "selection24 evt2 trace",
                                  INSERT_TRACE_EVT2, 4,
                                  K_TRACE_EVT2, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_LOOKUP_ARG:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = rd32((c->eax));",
                                  NULL,
                                  "selection24 lookup trace",
                                  INSERT_TRACE_LOOKUP_ARG, 4,
                                  K_TRACE_LOOKUP, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_LOOKUP_RES:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edi = c->eax;",
                                  NULL,
                                  "selection24 lookup result",
                                  INSERT_TRACE_LOOKUP_RES, 4,
                                  K_TRACE_LOOKUP, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_NULL:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 null-object guard",
                                  INSERT_GUARD_NULL, 9,
                                  K_GUARD_NULL, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_A60:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 event null-object guard",
                                  INSERT_GUARD_A60, 10,
                                  K_GUARD_A60, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_ABE0:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 event null-object guard",
                                  INSERT_GUARD_ABE0, 10,
                                  K_GUARD_ABE0, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_620:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 lookup null-object guard",
                                  INSERT_GUARD_620, 10,
                                  K_GUARD_620, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_APPLY:
                next = p;
                if (handle_insert(out, &next, end,
                                  "push32(c, 0x6F6B93F3u); "
                                  "WC3_DIRECT_CALL(L_6F04CDD0, c);",
                                  TRACE_ORDER_APPLY_ELIDED,
                                  "selection24 order apply trace",
                                  INSERT_TRACE_ORDER_APPLY, 5,
                                  K_TRACE_ORDER_APPLY, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_APPLY_2:
                next = p;
                if (handle_insert(out, &next, end,
                                  "push32(c, 0x6F6B942Bu); "
                                  "WC3_DIRECT_CALL(L_6F04CDD0, c);",
                                  TRACE_ORDER_APPLY_ELIDED,
                                  "selection24 order apply trace 2",
                                  INSERT_TRACE_ORDER_APPLY_2, 5,
                                  K_TRACE_ORDER_APPLY, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_1F20:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = c->esi;",
                                  NULL,
                                  "selection24 null-target guard",
                                  INSERT_GUARD_1F20, 2,
                                  K_GUARD_1F20, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_2120:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = c->esi;",
                                  NULL,
                                  "selection24 null-target guard 2",
                                  INSERT_GUARD_2120, 2,
                                  K_GUARD_1F20, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_2380:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = c->esi;",
                                  NULL,
                                  "selection24 null-target guard 3",
                                  INSERT_GUARD_2380, 2,
                                  K_GUARD_1F20, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_3FE0:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = c->esi;",
                                  NULL,
                                  "selection24 null-target guard 4",
                                  INSERT_GUARD_3FE0, 3,
                                  K_GUARD_1F20, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_SEND_NULL_C40:
            case VA_SEND_NULL_D40:
            case VA_SEND_NULL_F10: {
                static const char *const *const INS[3] = {
                    INSERT_SEND_NULL_C40, INSERT_SEND_NULL_D40,
                    INSERT_SEND_NULL_F10
                };
                static const int INS_COUNT[3] = { 7, 7, 7 };
                int pick = (va == VA_SEND_NULL_C40) ? 0
                           : (va == VA_SEND_NULL_D40) ? 1 : 2;
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 send null-object guard",
                                  INS[pick], INS_COUNT[pick],
                                  K_GUARD_SEND_NULL, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            }
            case VA_GUARD_FDD0_OBJ:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 dead-object guard",
                                  INSERT_GUARD_FDD0_OBJ, 7,
                                  K_GUARD_FDD0, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_FDD0_VTBL:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->eax = rd32((c->ecx));",
                                  NULL,
                                  "selection24 dead-vtable guard",
                                  INSERT_GUARD_FDD0_VTBL, 8,
                                  K_GUARD_FDD0, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_E60:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 dead-target guard",
                                  INSERT_GUARD_E60, 7,
                                  K_GUARD_E60, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_REMOVE_ENTRY:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebx = c->ecx;",
                                  NULL,
                                  "selection24 remove entry trace",
                                  INSERT_TRACE_REMOVE_ENTRY, 4,
                                  K_TRACE_REMOVE, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_REMOVE_UNIT:
                next = p;
                if (handle_insert(out, &next, end,
                                  "wr32((c->eax + 0x000001F8u), "
                                  "dec32(rd32((c->eax + 0x000001F8u)), 4));",
                                  NULL,
                                  "selection24 remove unit trace",
                                  INSERT_TRACE_REMOVE_UNIT, 4,
                                  K_TRACE_REMOVE, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_EVBIRTH:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edi = c->eax;",
                                  NULL,
                                  "selection24 event birth trace",
                                  INSERT_TRACE_EVBIRTH, 4,
                                  K_TRACE_EVBIRTH, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_EVINIT:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->eax = c->edi;",
                                  NULL,
                                  "selection24 event init trace",
                                  INSERT_TRACE_EVINIT, 4,
                                  K_TRACE_EVINIT, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDEREV_RES:
                next = p;
                if (handle_insert(out, &next, end,
                                  "flags_logicz(c, c->eax & c->eax, 4);",
                                  NULL,
                                  "selection24 orderev result trace",
                                  INSERT_TRACE_ORDEREV_RES, 4,
                                  K_TRACE_APPLY, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDEREV_RET:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->eax = c->esi;",
                                  NULL,
                                  "selection24 orderev event trace",
                                  INSERT_TRACE_ORDEREV_RET, 4,
                                  K_TRACE_APPLY, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_E60B:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = rd32((c->ebp + 0x00000008u));",
                                  NULL,
                                  "selection24 null-reference guard",
                                  INSERT_GUARD_E60B, 9,
                                  K_GUARD_E60B, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_TAG:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edi = c->eax;",
                                  NULL,
                                  "selection24 weak-pair target check",
                                  INSERT_GUARD_TAG, 17,
                                  K_GUARD_TAG, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_CAP_SLOT_MASK_A:
            case VA_CAP_SLOT_MASK_B:
            case VA_CAP_SLOT_MASK_C:
            case VA_CAP_SLOT_MASK_D:
            case VA_CAP_SLOT_MASK_E:
            case VA_CAP_SLOT_UNIT_LIST:
            case VA_CAP_SLOT_PANEL_A:
            case VA_CAP_SLOT_PANEL_B:
            case VA_CAP_SLOT_FREE:
                if (handle_subst(out, p, le,
                                 "0xCu", "wc3_sel24_cap()",
                                 NULL, NULL,
                                 K_CAP_SLOT, va, line, counts, sites,
                                 &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_SLOT_A:
            case VA_GUARD_SLOT_B:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = rd32((c->eax + c->ecx*8 + 0x00000004u));",
                                  NULL,
                                  "selection24 released-slot guard",
                                  INSERT_GUARD_SLOT, 10,
                                  K_GUARD_SLOT, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_EVENT:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = rd32((c->ebp + 0x00000008u));",
                                  NULL,
                                  "selection24 missing order event guard",
                                  INSERT_GUARD_EVENT, 13,
                                  K_GUARD_EVENT, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_FC0:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebp = c->esp;",
                                  NULL,
                                  "selection24 null-event guard",
                                  INSERT_GUARD_FC0, 10,
                                  K_GUARD_FC0, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_GUARD_E60_ENTRY:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = rd32((c->ebp + 0x00000008u));",
                                  NULL,
                                  "selection24 null-task guard",
                                  INSERT_GUARD_E60_ENTRY, 8,
                                  K_GUARD_E60_ENTRY, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_TASKNULL:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edi = rd32((c->eax + 0x00000054u));",
                                  NULL,
                                  "selection24 task null trace",
                                  INSERT_TRACE_TASKNULL, 4,
                                  K_TRACE_TASKNULL, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDERDROP:
                next = p;
                if (handle_insert(out, &next, end,
                                  "flags_logicz(c, c->eax & c->eax, 4);",
                                  NULL,
                                  "selection24 order drop trace",
                                  INSERT_TRACE_ORDERDROP, 5,
                                  K_TRACE_ORDERDROP, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_EXEC_1:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = c->eax;",
                                  NULL,
                                  "selection24 order exec trace",
                                  INSERT_TRACE_ORDER_EXEC_1, 6,
                                  K_TRACE_ORDER_EXEC, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_EXEC_2:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edx = c->esi;",
                                  NULL,
                                  "selection24 order exec result trace",
                                  INSERT_TRACE_ORDER_EXEC_2, 5,
                                  K_TRACE_ORDER_EXEC, va, line, counts,
                                  sites, &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_BEGIN_B:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebx = c->ecx;",
                                  NULL,
                                  "selection24 point-order begin trace B",
                                  INSERT_TRACE_ORDER_BEGIN_B, 4,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_BEGIN_C:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ebx = c->ecx;",
                                  NULL,
                                  "selection24 point-order begin trace C",
                                  INSERT_TRACE_ORDER_BEGIN_C, 4,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_LISTADD:
                next = p;
                if (handle_insert(out, &next, end,
                                  "if (!c->zf) { c->eax = c->esi; }",
                                  NULL,
                                  "selection24 PrClGen list-add trace",
                                  INSERT_TRACE_LISTADD, 7,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_FORMATION:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edi = c->ecx;",
                                  NULL,
                                  "selection24 formation entry trace",
                                  INSERT_TRACE_FORMATION, 4,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_CLUSTER_TRY:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = rd32((c->esi + 0x0000001Cu));",
                                  NULL,
                                  "selection24 cluster growth prepare",
                                  INSERT_TRACE_CLUSTER_TRY, 6,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_CLUSTER_INS:
                next = p;
                if (handle_insert(out, &next, end,
                                  "push32(c, rd32((c->ebp + 0x00000008u)));",
                                  NULL,
                                  "selection24 cluster insertion trace",
                                  INSERT_TRACE_CLUSTER_INS, 5,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_A5B9:
                /* Anchor one instruction past the frame allocation.  The
                 * 0xA54 -> 0x1354 frame substitution on 6F16A5B3 is a
                 * GROUP24 site and must go through the default branch;
                 * keeping the trace on 6F16A5B3 would mark the line handled
                 * and silently skip the frame growth. */
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->eax = rd32((GVA(0x6FCE689C)));",
                                  NULL,
                                  "selection24 group builder entry trace",
                                  INSERT_TRACE_A5B9, 4,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_CF94_BASE:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->ecx = rd32((c->ebp + 0xFFFFFFFCu));",
                                  NULL,
                                  "selection24 ground-order base trace",
                                  INSERT_TRACE_ORDER_BASE, 6,
                                  K_TRACE_FORMATION, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_UNIT_B:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = rd32((c->edi));",
                                  NULL,
                                  "selection24 order unit trace B",
                                  INSERT_TRACE_ORDER_UNIT_B, 4,
                                  K_TRACE_ORDER_UNIT_B, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_UNIT_C:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->esi = rd32((c->edi));",
                                  NULL,
                                  "selection24 order unit trace C",
                                  INSERT_TRACE_ORDER_UNIT_C, 4,
                                  K_TRACE_ORDER_UNIT_C, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_LOOP_B:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edx = c->eax;",
                                  NULL,
                                  "selection24 order loop trace B",
                                  INSERT_TRACE_ORDER_LOOP_B, 4,
                                  K_TRACE_ORDER_LOOP_B, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_ORDER_LOOP_C:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edx = c->eax;",
                                  NULL,
                                  "selection24 order loop trace C",
                                  INSERT_TRACE_ORDER_LOOP_C, 4,
                                  K_TRACE_ORDER_LOOP_C, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_SLOTS_A1:
                next = p;
                if (handle_insert(out, &next, end,
                                  "wr32((c->edi + 0x00000050u), c->eax);",
                                  NULL,
                                  "selection24 order slots trace A1",
                                  INSERT_TRACE_SLOTS_A1, 4,
                                  K_TRACE_ORDER_SLOTS, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_SLOTS_A2:
                next = p;
                if (handle_insert(out, &next, end,
                                  "wr32((c->edi + 0x00000050u), c->eax);",
                                  NULL,
                                  "selection24 order slots trace A2",
                                  INSERT_TRACE_SLOTS_A2, 4,
                                  K_TRACE_ORDER_SLOTS, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_SLOTS_C:
                next = p;
                if (handle_insert(out, &next, end,
                                  "wr32((c->esi + 0x0000002Cu), c->eax);",
                                  NULL,
                                  "selection24 order slots trace C",
                                  INSERT_TRACE_SLOTS_C, 4,
                                  K_TRACE_ORDER_SLOTS, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_FREELIST_HEAD:
                next = p;
                if (handle_insert(out, &next, end,
                                  "flags_sub(c, c->esi, 0xFFFFFFFFu, 4);",
                                  NULL,
                                  "selection24 freelist head normalize",
                                  INSERT_FREELIST_HEAD, 5,
                                  K_FREELIST_HEAD, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_ORDER_GROUP_FULL:
                next = p;
                if (handle_insert(out, &next, end,
                                  "flags_sub(c, c->eax, 0xCu, 4);",
                                  NULL,
                                  "selection24 group-full threshold",
                                  INSERT_ORDER_GROUP_FULL, 1,
                                  K_ORDER_GROUP_FULL, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_TRACE_EVRELEASE:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edx = rd32((c->ebp + 0x00000008u));",
                                  NULL,
                                  "selection24 event release trace",
                                  INSERT_TRACE_EVRELEASE, 4,
                                  K_TRACE_EVRELEASE, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            case VA_SLOT_UNLINK:
                next = p;
                if (handle_insert(out, &next, end,
                                  "c->edi = (uint32_t)(pop32(c));",
                                  NULL,
                                  "selection24 free-list unlink",
                                  INSERT_SLOT_UNLINK, 19,
                                  K_SLOT_UNLINK, va, line, counts, sites,
                                  &handled, err, errsz) != 0)
                    return -1;
                break;
            default:
                break;
            }
        }

        if (!handled && has_va) {
            const Group24Site *g = group24_lookup(va);

            if (g && handle_group24(out, p, le, g, line, counts, sites,
                                    &handled, err, errsz) != 0) {
                const Group24Site *alt = group24_alt_lookup(va);

                if (!alt ||
                    handle_group24(out, p, le, alt, line, counts, sites,
                                   &handled, err, errsz) != 0)
                    return -1;
            }
        }

        if (!handled && span.active) {
            Buf shifted = {0};
            int replaced = 0;
            int already = 0;

            if (shift_container_line(p, le, &span, &shifted,
                                     &replaced, &already) != 0) {
                buf_free(&shifted);
                return -1;
            }
            if (replaced || already) {
                if (!block_site) {
                    char before[65];
                    char after[65];

                    hash_hex(p, (size_t)(le - p), before);
                    hash_hex(shifted.p, shifted.n, after);
                    counts->n[K_ORDER_OBJ_CONTAINER]++;
                    sites_add(sites, K_ORDER_OBJ_CONTAINER, cur_block,
                              replaced ? "replace" : "already", line,
                              before, after);
                    block_site = 1;
                }
                if (buf_append(out, shifted.p, shifted.n) != 0) {
                    buf_free(&shifted);
                    return -1;
                }
                buf_free(&shifted);
                handled = 1;
            } else {
                buf_free(&shifted);
            }
        }

        if (!handled && buf_append(out, p, (size_t)(le - p)) != 0)
            return -1;
        {
            const char *q;
            for (q = p; q < next; ++q)
                if (*q == '\n')
                    ++line;
        }
        p = next;
    }

    if (profile) {
        const unsigned *exp = NULL;

        if (strcmp(profile, "seed") == 0)
            exp = expected_seed;
        else if (strcmp(profile, "closed") == 0)
            exp = expected_closed;
        else {
            snprintf(err, errsz, "unknown profile '%s'", profile);
            return -1;
        }
        if (!counts_match(counts, exp)) {
            int i;

            for (i = 0; i < K_COUNT; ++i)
                if (counts->n[i] != exp[i])
                    snprintf(err + strlen(err),
                             errsz - strlen(err),
                             "%s%s=%u/%u", strlen(err) ? ", " : "",
                             kind_name[i], counts->n[i], exp[i]);
            snprintf(err + strlen(err), errsz - strlen(err),
                     ": profile %s counts do not match", profile);
            return -1;
        }
    }

    return 0;
}

static int write_json(const char *path, const char *input, const char *output,
                      const char *profile, const Counts *c, const Sites *s,
                      char *err, size_t errsz);

/* ------------------------------------------------------------------ */
/* File IO                                                             */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_n, char *err,
                       size_t errsz)
{
    FILE *f;
    char *buf;
    size_t cap = 1 << 20;
    size_t n = 0;

    f = fopen(path, "rb");
    if (!f) {
        if (err)
            snprintf(err, errsz, "cannot open '%s': %s",
                     path, strerror(errno));
        return NULL;
    }
    buf = (char *)malloc(cap + 1);
    if (!buf) {
        fclose(f);
        if (err)
            snprintf(err, errsz, "out of memory reading '%s'", path);
        return NULL;
    }
    for (;;) {
        size_t got;
        if (n == cap) {
            char *nb;
            cap *= 2;
            nb = (char *)realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                if (err)
                    snprintf(err, errsz, "out of memory reading '%s'", path);
                return NULL;
            }
            buf = nb;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                if (err)
                    snprintf(err, errsz, "read error on '%s': %s",
                             path, strerror(errno));
                return NULL;
            }
            break;
        }
    }
    fclose(f);
    buf[n] = '\0';
    *out_n = n;
    return buf;
}

static int files_equal(const char *path, const char *data, size_t n)
{
    size_t m = 0;
    char *old = read_file(path, &m, NULL, 0);
    int eq = 0;

    if (old) {
        eq = (m == n) && memcmp(old, data, n) == 0;
        free(old);
    }
    return eq;
}

/* A small portability shim so the tool never pulls in windows.h. */
static unsigned long temp_serial(void)
{
    static unsigned long serial;
    return (++serial * 2654435761u) +
           (unsigned long)(uintptr_t)&serial;
}

static int write_file_atomic(const char *path, const char *data, size_t n,
                             char *err, size_t errsz)
{
    char tmp[4096];
    FILE *f;
    int wrote = 0;

    if (files_equal(path, data, n))
        return 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp.%lu", path,
             (unsigned long)temp_serial());
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot create '%s': %s", tmp, strerror(errno));
        return -1;
    }
    wrote = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0)
        wrote = 0;
    if (!wrote) {
        remove(tmp);
        snprintf(err, errsz, "short write on '%s'", tmp);
        return -1;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errsz, "cannot replace '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int run_pass(const char *in_path, const char *out_path,
                    const char *json_path, const char *profile, int quiet,
                    char *err, size_t errsz)
{
    size_t n = 0;
    char *data;
    Buf out = {0};
    Counts counts;
    Sites sites = {0};
    int rc;

    data = read_file(in_path, &n, err, errsz);
    if (!data)
        return -1;
    rc = process_buffer(data, n, profile, &out, &counts, &sites,
                        err, errsz);
    free(data);
    if (rc != 0) {
        buf_free(&out);
        free(sites.v);
        return -1;
    }
    if (out_path && write_file_atomic(out_path, out.p, out.n, err, errsz) != 0) {
        buf_free(&out);
        free(sites.v);
        return -1;
    }
    if (json_path &&
        write_json(json_path, in_path, out_path, profile, &counts, &sites,
                   err, errsz) != 0) {
        buf_free(&out);
        free(sites.v);
        return -1;
    }
    if (!quiet) {
        printf("%s: %s -> %s (%u site%s", PASS, in_path,
               out_path ? out_path : "-", counts_sum(&counts),
               counts_sum(&counts) == 1 ? "" : "s");
        if (profile)
            printf(", profile %s", profile);
        printf(")\n");
    }
    buf_free(&out);
    free(sites.v);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

static void write_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
            break;
        }
    }
    fputc('"', f);
}

static int write_json(const char *path, const char *input, const char *output,
                      const char *profile, const Counts *c, const Sites *s,
                      char *err, size_t errsz)
{
    FILE *f;
    int i;
    size_t j;

    f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errsz, "cannot create '%s': %s", path, strerror(errno));
        return -1;
    }
    fputs("{\n", f);
    fputs("  \"pass\": \"" PASS "\",\n", f);
    fprintf(f, "  \"pass_rev\": %d,\n", PASS_REV);
    fputs("  \"input\": ", f);
    write_json_string(f, input);
    fputs(",\n  \"output\": ", f);
    write_json_string(f, output ? output : "");
    fputs(",\n  \"profile\": ", f);
    if (profile)
        write_json_string(f, profile);
    else
        fputs("null", f);
    fputs(",\n  \"counts\": {\n", f);
    for (i = 0; i < K_COUNT; ++i) {
        fputs("    \"", f);
        fputs(kind_name[i], f);
        fprintf(f, "\": %u", c->n[i]);
        fputs(i + 1 == K_COUNT ? "\n" : ",\n", f);
    }
    fputs("  },\n", f);
    fprintf(f, "  \"total\": %u,\n", counts_sum(c));
    fputs("  \"applied\": ", f);
    {
        int applied = 0;
        for (j = 0; j < s->n; ++j)
            if (strcmp(s->v[j].action, "already") != 0)
                applied = 1;
        fputs(applied ? "true" : "false", f);
    }
    fputs(",\n  \"sites\": [", f);
    if (s->n) {
        fputc('\n', f);
        for (j = 0; j < s->n; ++j) {
            const Site *st = &s->v[j];
            fputs("    {\n      \"kind\": ", f);
            write_json_string(f, kind_name[st->kind]);
            fprintf(f, ",\n      \"va\": \"0x%08X\"", st->va);
            fputs(",\n      \"action\": ", f);
            write_json_string(f, st->action);
            fprintf(f, ",\n      \"line\": %d", st->line);
            fputs(",\n      \"before_sha256\": ", f);
            write_json_string(f, st->before);
            fputs(",\n      \"after_sha256\": ", f);
            write_json_string(f, st->after);
            fputs("\n    }", f);
            fputs(j + 1 == s->n ? "\n" : ",\n", f);
        }
        fputs("  ]\n", f);
    } else {
        fputs("]\n", f);
    }
    fputs("}\n", f);
    if (fclose(f) != 0) {
        snprintf(err, errsz, "write error on '%s': %s",
                 path, strerror(errno));
        return -1;
    }
    return 0;
}

static int json_counts_parse(const char *text, Counts *c, char *err,
                             size_t errsz)
{
    const char *p = strstr(text, "\"counts\"");
    const char *q;
    unsigned seen[K_COUNT];
    int i;

    memset(c, 0, sizeof(*c));
    memset(seen, 0, sizeof(seen));
    if (!p) {
        snprintf(err, errsz, "no counts object");
        return -1;
    }
    p = strchr(p + 8, '{');
    if (!p) {
        snprintf(err, errsz, "malformed counts object");
        return -1;
    }
    ++p;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' ||
               *p == ',')
            ++p;
        if (*p == '}' || *p == '\0')
            break;
        if (*p != '"') {
            snprintf(err, errsz, "malformed counts key");
            return -1;
        }
        ++p;
        q = strchr(p, '"');
        if (!q) {
            snprintf(err, errsz, "unterminated counts key");
            return -1;
        }
        {
            char name[64];
            size_t len = (size_t)(q - p);
            int kind = -1;
            unsigned long ul;
            char *endp = NULL;

            if (len >= sizeof(name)) {
                snprintf(err, errsz, "counts key too long");
                return -1;
            }
            memcpy(name, p, len);
            name[len] = '\0';
            for (i = 0; i < K_COUNT; ++i)
                if (strcmp(name, kind_name[i]) == 0)
                    kind = i;
            if (kind < 0) {
                snprintf(err, errsz, "unknown counts key '%s'", name);
                return -1;
            }
            p = q + 1;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
                ++p;
            if (*p != ':') {
                snprintf(err, errsz, "missing ':' after counts key '%s'",
                         name);
                return -1;
            }
            ++p;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
                ++p;
            errno = 0;
            ul = strtoul(p, &endp, 10);
            if (errno != 0 || endp == p || ul > UINT_MAX) {
                snprintf(err, errsz, "bad value for counts key '%s'", name);
                return -1;
            }
            if (seen[kind]) {
                snprintf(err, errsz, "duplicate counts key '%s'", name);
                return -1;
            }
            seen[kind] = 1;
            c->n[kind] = (unsigned)ul;
            p = endp;
        }
    }
    return 0;
}

static int json_pass_ok(const char *text)
{
    return strstr(text, "\"" PASS "\"") != NULL;
}

static int verify_coverage_one(const char *path, Counts *sum, int *reports,
                               char *err, size_t errsz)
{
    char *text;
    size_t n = 0;
    Counts c;

    text = read_file(path, &n, err, errsz);
    if (!text)
        return -1;
    if (!json_pass_ok(text)) {
        free(text);
        snprintf(err, errsz, "'%s' is not a " PASS " report", path);
        return -1;
    }
    if (json_counts_parse(text, &c, err, errsz) != 0) {
        free(text);
        return -1;
    }
    free(text);
    {
        int i;
        for (i = 0; i < K_COUNT; ++i)
            sum->n[i] += c.n[i];
    }
    ++*reports;
    return 0;
}

static int verify_coverage(const char *arg, int quiet, char *err,
                           size_t errsz)
{
    const char *manifest;
    char *text;
    size_t n = 0;
    Counts sum;
    int reports = 0;
    int failed = 0;

    memset(&sum, 0, sizeof(sum));
    manifest = (arg[0] == '@') ? arg + 1 : NULL;
    if (manifest) {
        text = read_file(manifest, &n, err, errsz);
        if (!text)
            return -1;
        {
            char *p = text;
            char *end = text + n;
            while (p < end) {
                char *le = (char *)find_sub(p, (size_t)(end - p), "\n");
                size_t len = le ? (size_t)(le - p) : (size_t)(end - p);
                char path[4096];

                while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' ||
                               p[len - 1] == '\t'))
                    --len;
                if (len) {
                    if (len >= sizeof(path))
                        len = sizeof(path) - 1;
                    memcpy(path, p, len);
                    path[len] = '\0';
                    if (verify_coverage_one(path, &sum, &reports,
                                            err, errsz) != 0) {
                        failed = 1;
                        break;
                    }
                }
                if (!le)
                    break;
                p = le + 1;
            }
        }
        free(text);
    } else {
        if (verify_coverage_one(arg, &sum, &reports, err, errsz) != 0)
            failed = 1;
    }

    if (!failed && reports == 0) {
        snprintf(err, errsz, "coverage manifest is empty");
        return -1;
    }
    if (!failed && !counts_match(&sum, expected_total)) {
        int i;

        snprintf(err, errsz, "coverage mismatch:");
        for (i = 0; i < K_COUNT; ++i)
            if (sum.n[i] != expected_total[i])
                snprintf(err + strlen(err), errsz - strlen(err),
                         " %s=%u/%u", kind_name[i], sum.n[i],
                         expected_total[i]);
        failed = 1;
    }

    if (!quiet) {
        int i;
        printf("%s coverage:", PASS);
        for (i = 0; i < K_COUNT; ++i)
            printf(" %s=%u", kind_name[i], sum.n[i]);
        printf(" reports=%d %s\n", reports,
               failed ? "MISMATCH" : "OK");
    }
    return failed ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

static const char SELF_FIXTURE[] =
"/* selection24 self-test fixture */\n"
"void L_fixture(CPU *restrict c)\n"
"{\n"
"    push32(c, 0x134u);                                           /* 6F39E7DE: push 0x134 */\n"
"    push32(c, 0x6F39EDA9u); dispatch(c, 0x6F3B91B0u);            /* 6F39EDA4: call 0x6f3b91b0 */\n"
"    push32(c, 0x6F39EDA9u); WC3_DIRECT_CALL(L_6F3B91B0, c);            /* 6F39EDA4: call 0x6f3b91b0 */\n"
"    c->esi = rd32((c->eax + 0x00000034u));                       /* 6F3C7D35: mov esi, dword ptr [eax + 0x34] */\n"
"    wr32((c->ebp + 0xFFFFFFF0u), c->edi);                        /* 6F3C7DBB: mov dword ptr [ebp - 0x10], edi */\n"
"    wr32((c->ebp + 0xFFFFFFF0u), c->edi);                        /* 6F3C7DBB: mov dword ptr [ebp - 0x10], edi */\n"
"    { uint32_t _fa_6F3C7D71 = (uint32_t)(c->ebx); uint32_t _fb_6F3C7D71 = (uint32_t)(0xCu);  /* 6F3C7D71: cmp ebx, 0xc */\n"
"    { uint32_t _fa_6F3C7D71 = (uint32_t)(c->ebx); uint32_t _fb_6F3C7D71 = (uint32_t)(0xCu);  /* 6F3C7D71: cmp ebx, 0xc */\n"
"    flags_sub(c, c->ebx, 0xCu, 4);                               /* 6F3C7DD0: cmp ebx, 0xc */\n"
"    flags_sub(c, c->ebx, 0xCu, 4);                               /* 6F3C7DD0: cmp ebx, 0xc */\n"
"    push32(c, rd32((c->ebp + 0xFFFFFFE4u)));                     /* 6F3C7E78: push dword ptr [ebp - 0x1c] */\n"
"    c->eax = 0xAAAAAAABu;                                        /* 6F3C7E7B: mov eax, 0xaaaaaaab */\n"
"    { uint64_t _p=(uint64_t)c->eax*(uint32_t)(c->ebx); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); } /* 6F3C7E80: mul ebx */\n"
"    push32(c, c->esi);                                           /* 6F3C7E82: push esi */\n"
"    c->edx = op_shr_nf(c->edx, 0x2u, 4);                         /* 6F3C7E83: shr edx, 2 */\n"
"    c->eax = c->ebx;                                             /* 6F3C7E86: mov eax, ebx */\n"
"    push32(c, c->edx);                                           /* 6F3C7E88: push edx */\n"
"    c->ecx = (uint32_t)((c->edx + c->edx*2));                    /* 6F3C7E89: lea ecx, [edx + edx*2] */\n"
"    c->ecx = add32(c->ecx, c->ecx, 4);                           /* 6F3C7E8C: add ecx, ecx */\n"
"    c->eax = flags_sub(c, c->eax, c->ecx, 4);                    /* 6F3C7E8E: sub eax, ecx */\n"
"    c->ecx = rd32((c->ebp + 0xFFFFFFE0u));                       /* 6F3C7E90: mov ecx, dword ptr [ebp - 0x20] */\n"
"    push32(c, c->eax);                                           /* 6F3C7E93: push eax */\n"
"    push32(c, 0x6F3C7E99u); dispatch(c, 0x6F3DA470u);            /* 6F3C7E94: call 0x6f3da470 */\n"
"    push32(c, rd32((c->ebp + 0xFFFFFFE4u)));                     /* 6F3C7E78: push dword ptr [ebp - 0x1c] */\n"
"    c->eax = 0xAAAAAAABu;                                        /* 6F3C7E7B: mov eax, 0xaaaaaaab */\n"
"    { uint64_t _p=(uint64_t)c->eax*(uint32_t)(c->ebx); c->eax=(uint32_t)_p; c->edx=(uint32_t)(_p>>32); } /* 6F3C7E80: mul ebx */\n"
"    push32(c, c->esi);                                           /* 6F3C7E82: push esi */\n"
"    c->edx = op_shr_nf(c->edx, 0x2u, 4);                         /* 6F3C7E83: shr edx, 2 */\n"
"    c->eax = c->ebx;                                             /* 6F3C7E86: mov eax, ebx */\n"
"    push32(c, c->edx);                                           /* 6F3C7E88: push edx */\n"
"    c->ecx = (uint32_t)((c->edx + c->edx*2));                    /* 6F3C7E89: lea ecx, [edx + edx*2] */\n"
"    c->ecx = add32(c->ecx, c->ecx, 4);                           /* 6F3C7E8C: add ecx, ecx */\n"
"    c->eax = flags_sub(c, c->eax, c->ecx, 4);                    /* 6F3C7E8E: sub eax, ecx */\n"
"    c->ecx = rd32((c->ebp + 0xFFFFFFE0u));                       /* 6F3C7E90: mov ecx, dword ptr [ebp - 0x20] */\n"
"    push32(c, c->eax);                                           /* 6F3C7E93: push eax */\n"
"    push32(c, 0x6F3C7E99u); WC3_DIRECT_CALL(L_6F3DA470, c);            /* 6F3C7E94: call 0x6f3da470 */\n"
"    c->eax = rd32((c->ecx + 0x00000130u));                       /* 6F3DA47F: mov eax, dword ptr [ecx + 0x130] */\n"
"    flags_sub(c, c->eax, 0xCu, 4);                               /* 6F28DC30: cmp eax, 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F28EB97: push 0xc */\n"
"    push32(c, 0xCu);                                             /* 6F390293: push 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F391AD2: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F391AD2: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F391AD2: mov edx, 0xc */\n"
"    flags_sub(c, rd32((c->ebx + 0x00000010u)), 0xCu, 4);         /* 6F28D667: cmp dword ptr [ebx + 0x10], 0xc */\n"
"    flags_sub(c, c->eax, 0xCu, 4);                               /* 6F6BC82A: cmp eax, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BCAC9: mov edx, 0xc */\n"
"    c->ebx = 0xCu;                                               /* 6F6BCF1D: mov ebx, 0xc */\n"
"    c->ebx = 0xCu;                                               /* 6F6BCD5D: mov ebx, 0xc */\n"
"    c->ebx = c->ecx;                                             /* 6F3ACCA3: mov ebx, ecx */\n"
"    c->esp = sub32(c->esp, 0x250u, 4);                           /* 6F6B9901: sub esp, 0x250 */\n"
"    c->esp = sub32(c->esp, 0x248u, 4);                           /* 6F6B9F81: sub esp, 0x248 */\n"
"    c->esp = flags_sub(c, c->esp, 0x24Cu, 4);                    /* 6F6BAAB1: sub esp, 0x24c */\n"
"    c->esp = flags_sub(c, c->esp, 0x248u, 4);                    /* 6F6BB061: sub esp, 0x248 */\n"
"    wr32((c->esi + 0x0000004Cu), 0xCu);                          /* 6F6B879A: mov dword ptr [esi + 0x4c], 0xc */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6B9E05: lea ecx, [ebp - 0x1c0] */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6B9E05: lea ecx, [ebp - 0x1c0] */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6BA444: lea ecx, [ebp - 0x1c0] */\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE40u));                 /* 6F6BA444: lea ecx, [ebp - 0x1c0] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE58u));                 /* 6F6B9E34: lea esi, [ebp - 0x1a8] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE58u));                 /* 6F6B9E34: lea esi, [ebp - 0x1a8] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE5Cu));                 /* 6F6BA473: lea esi, [ebp - 0x1a4] */\n"
"    c->esi = (uint32_t)((c->ebp + 0xFFFFFE5Cu));                 /* 6F6BA473: lea esi, [ebp - 0x1a4] */\n"
"    c->ecx = (uint32_t)((c->edi + c->eax*4));                    /* 6F6BA76E: lea ecx, [edi + eax*4] */\n"
"    c->ecx = (uint32_t)((c->edi + c->eax*4));                    /* 6F6BA76E: lea ecx, [edi + eax*4] */\n"
"    wr32((c->edi + c->eax*4 + 0x00000044u), c->ebx);             /* 6F6BA771: mov dword ptr [edi + eax*4 + 0x44], ebx */\n"
"    wr32((c->edi + c->eax*4 + 0x00000044u), c->ebx);             /* 6F6BA771: mov dword ptr [edi + eax*4 + 0x44], ebx */\n"
"    wr32((c->edi + c->eax*4 + 0x00000060u), c->edx);             /* 6F6BA775: mov dword ptr [edi + eax*4 + 0x60], edx */\n"
"    wr32((c->edi + c->eax*4 + 0x00000060u), c->edx);             /* 6F6BA775: mov dword ptr [edi + eax*4 + 0x60], edx */\n"
"    wr32((c->edi + c->ecx*4 + 0x00000048u), c->eax);             /* 6F6BA78C: mov dword ptr [edi + ecx*4 + 0x48], eax */\n"
"    wr32((c->edi + c->ecx*4 + 0x00000048u), c->eax);             /* 6F6BA78C: mov dword ptr [edi + ecx*4 + 0x48], eax */\n"
"    c->esi = (uint32_t)((c->edi + c->eax*4));                    /* 6F6BAA00: lea esi, [edi + eax*4] */\n"
"    c->esi = (uint32_t)((c->edi + c->eax*4));                    /* 6F6BAA00: lea esi, [edi + eax*4] */\n"
"    wr32((c->edi + c->ecx*4 + 0x00000048u), c->eax);             /* 6F6BAA73: mov dword ptr [edi + ecx*4 + 0x48], eax */\n"
"    wr32((c->edi + c->ecx*4 + 0x00000048u), c->eax);             /* 6F6BAA73: mov dword ptr [edi + ecx*4 + 0x48], eax */\n"
"    c->ebp = c->esp;                                             /* 6F057C31: mov ebp, esp */\n"
"    c->ebp = c->esp;                                             /* 6F689A61: mov ebp, esp */\n"
"    c->ebp = c->esp;                                             /* 6F67ABE1: mov ebp, esp */\n"
"    c->esp = flags_sub(c, c->esp, 0xCu, 4);                      /* 6F146D33: sub esp, 0xc */\n"
"    c->ecx = rd32((c->eax));                                     /* 6F04D9E7: mov ecx, dword ptr [eax] */\n"
"    c->edi = c->eax;                                             /* 6F04D9F3: mov edi, eax */\n"
"    flags_sub(c, c->esi, 0xCu, 4);                               /* 6F293020: cmp esi, 0xc */\n"
"    flags_sub(c, c->esi, 0xCu, 4);                               /* 6F293080: cmp esi, 0xc */\n"
"    flags_sub(c, c->eax, 0xCu, 4);                               /* 6F6B915A: cmp eax, 0xc */\n"
"    c->edx = 0xCu;                                               /* 6F6BC1AF: mov edx, 0xc */\n"
"    wr32((c->ebp + 0xFFFFFFD8u), 0xCu);                          /* 6F6BC1BC: mov dword ptr [ebp - 0x28], 0xc */\n"
"    c->ecx = 0xCu;                                               /* 6F396916: mov ecx, 0xc */\n"
"    c->esi = rd32((c->edi));                                     /* 6F6BA59D: mov esi, dword ptr [edi] */\n"
"    c->esi = rd32((c->edi));                                     /* 6F6BA80D: mov esi, dword ptr [edi] */\n"
"    c->edx = c->eax;                                             /* 6F6BA4C2: mov edx, eax */\n"
"    c->edx = c->eax;                                             /* 6F6B9E8D: mov edx, eax */\n"
"    c->edx = c->eax;                                             /* 6F6BA4C2: mov edx, eax */\n"
"    c->edx = c->eax;                                             /* 6F6B9E8D: mov edx, eax */\n"
"    wr32((c->edi + 0x00000050u), c->eax);                        /* 6F6BB731: mov dword ptr [edi + 0x50], eax */\n"
"    wr32((c->edi + 0x00000050u), c->eax);                        /* 6F6BB90C: mov dword ptr [edi + 0x50], eax */\n"
"    wr32((c->esi + 0x0000002Cu), c->eax);                        /* 6F6BBFCC: mov dword ptr [esi + 0x2c], eax */\n"
"    flags_sub(c, c->esi, 0xFFFFFFFFu, 4);                        /* 6F1CBF4D: cmp esi, -1 */\n"
"    flags_sub(c, c->eax, 0xCu, 4);                               /* 6F67DDF1: cmp eax, 0xc */\n"
"    c->edx = rd32((c->ebp + 0x00000008u));                       /* 6F1CBFB3: mov edx, dword ptr [ebp + 8] */\n"
"    c->edi = (uint32_t)(pop32(c));                               /* 6F1CBE99: pop edi */\n"
"    c->ebp = c->esp;                                             /* 6F056621: mov ebp, esp */\n"
"    push32(c, 0x6F6B93F3u); WC3_DIRECT_CALL(L_6F04CDD0, c);      /* 6F6B93EE: call 0x6f04cdd0 */\n"
"    push32(c, 0x6F6B942Bu); WC3_DIRECT_CALL(L_6F04CDD0, c);      /* 6F6B9426: call 0x6f04cdd0 */\n"
"    c->ecx = c->esi;                                             /* 6F691FB0: mov ecx, esi */\n"
"    c->ecx = c->esi;                                             /* 6F6921C8: mov ecx, esi */\n"
"    c->ecx = c->esi;                                             /* 6F69240F: mov ecx, esi */\n"
"    c->ecx = c->esi;                                             /* 6F6740A0: mov ecx, esi */\n"
"L_6F691FEB:\n"
"L_6F692244:\n"
"L_6F692451:\n"
"    c->ebp = c->esp;                                             /* 6F689C41: mov ebp, esp */\n"
"    c->ebp = c->esp;                                             /* 6F689D41: mov ebp, esp */\n"
"    c->ebp = c->esp;                                             /* 6F689F11: mov ebp, esp */\n"
"    c->ebp = c->esp;                                             /* 6F66FDD1: mov ebp, esp */\n"
"    c->eax = rd32((c->ecx));                                     /* 6F66FDE7: mov eax, dword ptr [ecx] */\n"
"    c->ebp = c->esp;                                             /* 6F691E61: mov ebp, esp */\n"
"    c->ebx = c->ecx;                                             /* 6F28EF18: mov ebx, ecx */\n"
"    wr32((c->eax + 0x000001F8u), dec32(rd32((c->eax + 0x000001F8u)), 4)); /* 6F28EFBB: dec dword ptr [eax + 0x1f8] */\n"
"    wr32((c->eax + 0x000001F8u), dec32(rd32((c->eax + 0x000001F8u)), 4)); /* 6F28EFBB: dec dword ptr [eax + 0x1f8] */\n"
"    wr32((c->eax + 0x000001F8u), dec32(rd32((c->eax + 0x000001F8u)), 4)); /* 6F28EFBB: dec dword ptr [eax + 0x1f8] */\n"
"    c->edi = c->eax;                                             /* 6F154B76: mov edi, eax */\n"
"    c->eax = c->edi;                                             /* 6F154B92: mov eax, edi */\n"
"    c->esi = rd32((c->ebp + 0x00000008u));                       /* 6F691ECA: mov esi, dword ptr [ebp + 8] */\n"
"    flags_logicz(c, c->eax & c->eax, 4);                         /* 6F69BE1B: test eax, eax */\n"
"    c->eax = c->esi;                                             /* 6F69BE5C: mov eax, esi */\n"
"    c->esi = c->eax;                                             /* 6F6B93E5: mov esi, eax */\n"
"    c->edx = c->esi;                                             /* 6F6B93EA: mov edx, esi */\n"
"    c->edi = c->eax;                                             /* 6F48CC29: mov edi, eax */\n"
"    c->edi = c->eax;                                             /* 6F48CC29: mov edi, eax */\n"
"    flags_sub(c, c->edi, 0xCu, 4);                               /* 6F26B522: cmp edi, 0xc */\n"
"    flags_sub(c, c->edi, 0xCu, 4);                               /* 6F3DEBEE: cmp edi, 0xc */\n"
"    flags_sub(c, c->edi, 0xCu, 4);                               /* 6F66BEFA: cmp edi, 0xc */\n"
"    flags_sub(c, c->edi, 0xCu, 4);                               /* 6F69D217: cmp edi, 0xc */\n"
"    { uint32_t _fa_6F3DB037 = (uint32_t)(c->esi); uint32_t _fb_6F3DB037 = (uint32_t)(0xCu); /* 6F3DB037: cmp esi, 0xc */\n"
"    flags_sub(c, c->esi, 0xCu, 4);                               /* 6F3DBE14: cmp esi, 0xc */\n"
"    flags_sub(c, c->esi, 0xCu, 4);                               /* 6F3DBE14: cmp esi, 0xc */\n"
"    flags_sub(c, c->esi, 0xCu, 4);                               /* 6F68C0CB: cmp esi, 0xc */\n"
"    flags_sub(c, c->esi, 0xCu, 4);                               /* 6F3C2E33: cmp esi, 0xc */\n"
"    c->eax = 0xCu;                                               /* 6F39737B: mov eax, 0xc */\n"
"    c->ecx = rd32((c->eax + c->ecx*8 + 0x00000004u));           /* 6F054590: mov ecx, dword ptr [eax + ecx*8 + 4] */\n"
"    c->ecx = rd32((c->eax + c->ecx*8 + 0x00000004u));           /* 6F05457D: mov ecx, dword ptr [eax + ecx*8 + 4] */\n"
"    c->ecx = rd32((c->eax + c->ecx*8 + 0x00000004u));           /* 6F05457D: mov ecx, dword ptr [eax + ecx*8 + 4] */\n"
"    c->ecx = rd32((c->ebp + 0x00000008u));                      /* 6F6857E9: mov ecx, dword ptr [ebp + 8] */\n"
"    c->ebp = c->esp;                                             /* 6F689FC1: mov ebp, esp */\n"
"    c->esi = rd32((c->ebp + 0x00000008u));                       /* 6F691E7C: mov esi, dword ptr [ebp + 8] */\n"
"    if (c->zf) goto L_6F69274B;                                  /* 6F69272C: je 0x6f69274b */\n"
"    c->edi = rd32((c->eax + 0x00000054u));                        /* 6F692727: mov edi, dword ptr [eax + 0x54] */\n"
"    flags_logicz(c, c->eax & c->eax, 4);                         /* 6F69273F: test eax, eax */\n"
"    c->ecx = 0xCu;                                               /* 6F169C68: mov ecx, 0xc */\n"
"    c->edi = (uint32_t)((c->esi + 0x000000ACu));                 /* 6F169C70: lea edi, [esi + 0xac] */\n"
"    c->ebx = add32(c->ebx, 0xACu, 4);                            /* 6F16AE63: add ebx, 0xac */\n"
"    c->edi = add32(c->edi, 0xACu, 4);                            /* 6F16B924: add edi, 0xac */\n"
"    wr32((c->ecx + 0x000000DCu), 0xFFFFFFFFu);                   /* 6F169296: mov dword ptr [ecx + 0xdc], 0xffffffff */\n"
"    c->edx = 0xBu;                                               /* 6F169231: mov edx, 0xb */\n"
"    wr32((c->esi + 0x00000100u), flags_logicz(c, rd32((c->esi + 0x00000100u)) | 0x1000u, 4)); /* 6F16BF83: or dword ptr [esi + 0x100], 0x1000 */\n"
"    push32(c, 0x108u);                                           /* 6F157A27: push 0x108 */\n"
"}\n"
"void L_6F6BAAA0(CPU *restrict c)\n"
"{\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE1Cu));                 /* 6F6BAAD6: lea ecx, [ebp - 0x1e4] */\n"
"    wr32((c->ebp + 0xFFFFFE68u), c->eax);                        /* 6F6BAAF5: mov dword ptr [ebp - 0x198], eax */\n"
"}\n"
"void L_6F6BB050(CPU *restrict c)\n"
"{\n"
"    c->ecx = (uint32_t)((c->ebp + 0xFFFFFE20u));                 /* 6F6BB086: lea ecx, [ebp - 0x1e0] */\n"
"    c->eax = rd32((c->ebp + 0xFFFFF5B4u));                       /* 6F16A672: mov eax, dword ptr [ebp - 0xa4c] */\n"
"}\n";

static size_t count_occurrences(const char *p, size_t n, const char *needle)
{
    size_t count = 0;
    const char *q = p;

    while ((q = find_sub(q, n - (size_t)(q - p), needle)) != NULL) {
        ++count;
        q += strlen(needle);
    }
    return count;
}

static int self_test(char *err, size_t errsz)
{
    Buf out1 = {0};
    Buf out2 = {0};
    Counts c1;
    Counts c2;
    Sites s1 = {0};
    Sites s2 = {0};
    int ok = 1;
    char err2[512];
    unsigned exp_fix[K_COUNT];

    /* The fixture exercises one sample of every site kind, including the two
     * relocated order handlers.  Corpus totals live in expected_total and are
     * checked against the coverage JSON below. */
    memcpy(exp_fix, expected_total, sizeof(exp_fix));
    exp_fix[K_ORDER_OBJ_CONTAINER] = 5;
    exp_fix[K_ORDER_GROUP_FULL] = 1;
    exp_fix[K_TRACE_EVRELEASE] = 1;
    exp_fix[K_SLOT_UNLINK] = 1;
    exp_fix[K_GROUP24] = 9;
    /* The fixture deliberately does not carry the new point-order anchors:
     * they are covered by the whole-corpus totals and by the real lift. */
    exp_fix[K_TRACE_FORMATION] = 0;

    if (process_buffer(SELF_FIXTURE, strlen(SELF_FIXTURE), NULL,
                       &out1, &c1, &s1, err2, sizeof(err2)) != 0) {
        snprintf(err, errsz, "self-test first pass failed: %s", err2);
        goto fail;
    }
    if (!counts_match(&c1, exp_fix)) {
        snprintf(err, errsz, "self-test counts mismatch after first pass");
        goto fail;
    }
    if (count_occurrences(out1.p, out1.n, "wc3_sel24_init_group") != 2 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_note_selection") != 1 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_select") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_hide_buttons(c, c->ebx)") != 1 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_fill") != 2 ||
        count_occurrences(out1.p, out1.n, "0x144u") != 1 ||
        count_occurrences(out1.p, out1.n, "0x00000138u") != 1 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_cap()") != 41 ||
        count_occurrences(out1.p, out1.n, "0x9A0u") != 1 ||
        count_occurrences(out1.p, out1.n, "0x9A8u") != 1 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF6A0u") != 4 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF6B8u") != 2 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF6BCu") != 2 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF860u") != 4 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF8A4u") != 2 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF8C0u") != 2 ||
        count_occurrences(out1.p, out1.n, "0xFFFFF8A8u") != 4 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_addunit") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_evt2") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_lookup") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_guard") != 6 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 null-object guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 event null-object guard") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 lookup null-object guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 null-target guard") != 4 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 send null-object guard") != 3 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 dead-object guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 dead-vtable guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 dead-target guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 remove entry trace") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 remove unit trace") != 3 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 event birth trace") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 null-reference guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 weak-pair target check") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 released-slot guard") != 3 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 missing order event guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 orderev result trace") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 orderev event trace") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_order_unit") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_order_loop") != 4 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_order_slots") != 3 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_order_apply") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_order_exec") != 2 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 null-event guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "selection24 null-task guard") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_tasknull") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_evinit") != 1 ||
        count_occurrences(out1.p, out1.n,
                          "wc3_sel24_trace_orderdrop") != 1 ||
        count_occurrences(out1.p, out1.n, "0x5B8u") != 1 ||
        count_occurrences(out1.p, out1.n, "0x5B4u") != 1 ||
        count_occurrences(out1.p, out1.n, "[esi + 0x4c], 0x18 */") != 1 ||
        count_occurrences(out1.p, out1.n, "0xFFFFFA58u") != 1 ||
        count_occurrences(out1.p, out1.n, "0xFFFFFAA4u") != 1 ||
        count_occurrences(out1.p, out1.n, "0xFFFFFA5Cu") != 1 ||
        count_occurrences(out1.p, out1.n, "[ebp - 0x5a8]") != 1 ||
        count_occurrences(out1.p, out1.n, "[ebp - 0x5a4]") != 1 ||
        count_occurrences(out1.p, out1.n, "0xFFFFFE1Cu") != 0 ||
        count_occurrences(out1.p, out1.n, "0xFFFFFE20u") != 0 ||
        count_occurrences(out1.p, out1.n, "0xFFFFFE68u") != 0) {
        snprintf(err, errsz, "self-test first pass content mismatch");
        goto fail;
    }
    if (count_occurrences(out1.p, out1.n, "0xFFFFF5B4u") != 0 ||
        count_occurrences(out1.p, out1.n, "0xFFFFECB4u") != 1 ||
        count_occurrences(out1.p, out1.n, "0x000001C0u") != 2 ||
        count_occurrences(out1.p, out1.n, "0x0000013Cu") != 1 ||
        count_occurrences(out1.p, out1.n, "0x13Cu") != 2 ||
        count_occurrences(out1.p, out1.n, "0x0000019Cu") != 1 ||
        count_occurrences(out1.p, out1.n, "0x1C8u") != 1 ||
        count_occurrences(out1.p, out1.n, "wc3_sel24_cap() - 1") != 1 ||
        count_occurrences(out1.p, out1.n, "0xBu") != 0 ||
        count_occurrences(out1.p, out1.n, "0x00000100u") != 0 ||
        count_occurrences(out1.p, out1.n, "0x000000ACu") != 0 ||
        count_occurrences(out1.p, out1.n, "0xACu") != 0 ||
        count_occurrences(out1.p, out1.n, "0x000000DCu") != 0 ||
        count_occurrences(out1.p, out1.n, "0x108u") != 0) {
        snprintf(err, errsz, "self-test group24 shift mismatch");
        goto fail;
    }

    if (process_buffer(out1.p, out1.n, NULL, &out2, &c2, &s2,
                       err2, sizeof(err2)) != 0) {
        snprintf(err, errsz, "self-test second pass failed: %s", err2);
        goto fail;
    }
    if (!counts_match(&c2, exp_fix)) {
        snprintf(err, errsz, "self-test counts mismatch after second pass");
        goto fail;
    }
    if (out1.n != out2.n || memcmp(out1.p, out2.p, out1.n) != 0) {
        snprintf(err, errsz, "self-test is not idempotent");
        goto fail;
    }

    {
        /* P0-6 return-address elision turns the two trace anchors into
         * "c->esp -= 4; WC3_DIRECT_CALL(...)".  Exercise that form so the
         * alternate anchor cannot regress silently. */
        static const char *const pushed[] = {
            "push32(c, 0x6F6B93F3u); WC3_DIRECT_CALL(L_6F04CDD0, c);",
            "push32(c, 0x6F6B942Bu); WC3_DIRECT_CALL(L_6F04CDD0, c);"
        };
        static const char elided[] =
            "c->esp -= 4; WC3_DIRECT_CALL(L_6F04CDD0, c);";
        size_t vlen = strlen(SELF_FIXTURE);
        char *variant = (char *)malloc(vlen + 1);
        Buf out3 = {0};
        Counts c3;
        Sites s3 = {0};
        size_t k;

        if (!variant) {
            snprintf(err, errsz, "self-test out of memory");
            goto fail;
        }
        memcpy(variant, SELF_FIXTURE, vlen + 1);
        for (k = 0; k < sizeof(pushed) / sizeof(pushed[0]); k++) {
            size_t plen = strlen(pushed[k]);
            size_t elen = strlen(elided);
            char *hit = (char *)find_sub(variant, strlen(variant), pushed[k]);
            if (!hit) {
                snprintf(err, errsz,
                         "self-test elided anchor %u missing from fixture",
                         (unsigned)k);
                free(variant);
                goto fail;
            }
            memmove(hit + elen, hit + plen, strlen(hit + plen) + 1);
            memcpy(hit, elided, elen);
        }
        if (process_buffer(variant, strlen(variant), NULL, &out3, &c3, &s3,
                           err2, sizeof(err2)) != 0) {
            snprintf(err, errsz, "self-test elided anchor failed: %s", err2);
            free(variant);
            buf_free(&out3);
            free(s3.v);
            goto fail;
        }
        if (!counts_match(&c3, exp_fix) ||
            count_occurrences(out3.p, out3.n,
                              "wc3_sel24_trace_order_apply") != 2) {
            snprintf(err, errsz, "self-test elided anchor mismatch");
            free(variant);
            buf_free(&out3);
            free(s3.v);
            goto fail;
        }
        free(variant);
        buf_free(&out3);
        free(s3.v);
    }

    {
        size_t flen = strlen(SELF_FIXTURE);
        char *tampered = (char *)malloc(flen + 1);
        Buf out3 = {0};
        Counts c3;
        Sites s3 = {0};

        if (!tampered) {
            snprintf(err, errsz, "self-test out of memory");
            goto fail;
        }
        memcpy(tampered, SELF_FIXTURE, flen + 1);
        {
            char *hit = (char *)find_sub(tampered, flen, "0x134u");
            if (hit)
                memcpy(hit, "0x999u", 6);
        }
        if (process_buffer(tampered, flen, NULL, &out3, &c3, &s3,
                           err2, sizeof(err2)) == 0) {
            snprintf(err, errsz, "self-test did not reject a tampered template");
            free(tampered);
            buf_free(&out3);
            free(s3.v);
            goto fail;
        }
        free(tampered);
        buf_free(&out3);
        free(s3.v);
    }

    {
        /* Mirrors a current report: PASS_REV 45 (369 sites). */
        const char json[] =
            "{\"pass\":\"" PASS "\",\"pass_rev\":45,\"counts\":{"
            "\"group_alloc\":1,\"init_group\":2,\"select\":2,"
            "\"cap_outer\":2,\"cap_inner\":2,\"fill_block\":2,"
            "\"fill_grid\":1,\"note_selection\":1,\"hide_all\":1,"
            "\"addunit_cap\":1,\"cap_local_list\":5,\"cap_copy_list\":3,"
            "\"cap_sync_add\":1,\"cap_net_recv\":1,\"cap_net_apply\":5,"
            "\"cap_net_send\":2,\"cap_select_all\":1,"
            "\"order_frame\":2,\"order_sort\":4,\"order_loop\":4,"
            "\"order_callback\":8,\"order_callback_b\":4,"
            "\"trace_evt2\":1,\"trace_lookup\":2,"
            "\"cap_group_store\":3,\"cap_add_merge\":3,\"guard_null\":1,"
            "\"guard_a60\":1,\"guard_abe0\":1,"
            "\"trace_order_unit_b\":1,\"trace_order_unit_c\":1,"
            "\"trace_order_loop_b\":2,\"trace_order_loop_c\":2,"
            "\"guard_620\":1,\"trace_order_apply\":2,\"guard_1f20\":4,"
            "\"trace_order_exec\":2,\"guard_send_null\":3,\"guard_fdd0\":2,"
            "\"guard_e60\":1,\"trace_remove\":4,\"trace_evbirth\":1,"
            "\"guard_e60b\":1,\"trace_apply\":2,\"guard_tag\":2,\"cap_slot\":10,"
            "\"guard_slot\":3,\"guard_event\":1,"
            "\"guard_fc0\":1,\"guard_e60_entry\":1,\"trace_tasknull\":1,"
            "\"trace_evinit\":1,\"trace_orderdrop\":1,"
            "\"order_obj_container\":35,\"trace_order_slots\":3,\"freelist_head\":1,\"order_group_full\":2,\"trace_evrelease\":1,\"slot_unlink\":1,\"group24\":206,\"trace_formation\":9},"
            "\"total\":375}";
        Counts c3;

        if (json_counts_parse(json, &c3, err2, sizeof(err2)) != 0 ||
            !counts_match(&c3, expected_total)) {
            snprintf(err, errsz, "self-test coverage parser mismatch");
            goto fail;
        }
    }

    printf("self-test: OK (%u sites, idempotent, fail-closed)\n",
           counts_sum(&c1));
    ok = 1;
    goto done;

fail:
    ok = 0;
done:
    buf_free(&out1);
    buf_free(&out2);
    free(s1.v);
    free(s2.v);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "usage: " PASS " --lift IN --out OUT [--json REPORT] "
            "[--profile seed|closed] [--quiet]\n"
            "       " PASS " --verify-coverage @manifest [--quiet]\n"
            "       " PASS " --self-test\n");
}

int main(int argc, char **argv)
{
    const char *lift = NULL;
    const char *out = NULL;
    const char *json = NULL;
    const char *profile = NULL;
    const char *coverage = NULL;
    int quiet = 0;
    int self = 0;
    int i;
    char err[1024];

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--lift") == 0 && i + 1 < argc) {
            lift = argv[++i];
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "--json") == 0 && i + 1 < argc) {
            json = argv[++i];
        } else if (strcmp(a, "--profile") == 0 && i + 1 < argc) {
            profile = argv[++i];
        } else if (strcmp(a, "--verify-coverage") == 0 && i + 1 < argc) {
            coverage = argv[++i];
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--self-test") == 0) {
            self = 1;
        } else {
            usage();
            return 2;
        }
    }

    if (self) {
        if (self_test(err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", PASS, err);
            return 1;
        }
        return 0;
    }

    if (coverage) {
        if (verify_coverage(coverage, quiet, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", PASS, err);
            return 1;
        }
        return 0;
    }

    if (!lift || !out) {
        usage();
        return 2;
    }
    if (run_pass(lift, out, json, profile, quiet, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", PASS, err);
        return 1;
    }
    return 0;
}