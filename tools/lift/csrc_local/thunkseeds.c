/* thunkseeds.c - address-taken starts that entry.exe misses.
 *
 * entry.exe applies MSVC's 16-byte alignment rule to starts that follow a
 * terminator without padding.  A small number of real functions begin
 * immediately after a six-byte `jmp dword ptr [imm32]` thunk and are therefore
 * unaligned; entry.exe drops them even though a dword in the image points at
 * them.  Widening entry.exe to --align 1 would also admit every accidental
 * terminator byte inside an instruction, so this tool isolates two classes
 * with direct evidence:
 *
 *   - a 4-byte value in any section points into an executable section and the
 *     six bytes before the target are FF 25 (jmp dword ptr [imm32]);
 *   - a 4-byte value in a non-executable section points at an unaligned start
 *     immediately after a complete C3 (`ret`) or C2 xx xx (`ret imm16`), the
 *     source is not a large jump-table run, and the target does not look like
 *     an epilogue, an x87 case block, or a caller-frame continuation.
 *
 * The second class covers CRT initializer tables and callback/vtable slots
 * (war3 0x45C96C, Mss32 0x3514E628, ijl15 0x60050C70) without admitting the
 * EH and switch continuations stored in .rdata.  The target must not already
 * be in the --known seed list.
 *
 * The result is a seed file in the same `0xVA 512 name` format used by
 * closure_pipeline.  A target whose first 12 bytes are three in-image code
 * pointers is a jump-table base, not a function, and is skipped.
 *
 *   thunkseeds --module image --known seeds.txt --out extra.txt
 */

#define _CRT_SECURE_NO_WARNINGS
#include "pe.h"
#include "csglue.h"

static void die(const char *msg)
{
    fprintf(stderr, "thunkseeds: %s\n", msg);
    exit(2);
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    size_t got;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

typedef struct { uint32_t *v; size_t n, cap; } U32Vec;

/* --audit prints every code pointer stored in a non-executable section that
 * is not in --known, without applying any entry heuristic.  It is a
 * diagnostic for missing indirect targets, not a seed source. */
static int audit_mode;

static void u32_add(U32Vec *a, uint32_t v)
{
    if (!v) return;
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->v = (uint32_t *)realloc(a->v, a->cap * sizeof(uint32_t));
        if (!a->v) die("out of memory (VA vector)");
    }
    a->v[a->n++] = v;
}

static int u32_cmp(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void u32_sort_unique(U32Vec *a)
{
    size_t i, j;
    if (a->n == 0) return;
    qsort(a->v, a->n, sizeof(uint32_t), u32_cmp);
    for (i = 1, j = 1; i < a->n; i++)
        if (a->v[i] != a->v[j - 1]) a->v[j++] = a->v[i];
    a->n = j;
}

static int u32_contains(const U32Vec *a, uint32_t v)
{
    size_t lo = 0, hi = a->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a->v[mid] == v) return 1;
        if (a->v[mid] < v) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* Accept both closure inventories (`X(6F001000)`) and seed files
 * (`0x6F001000 512 seed_...`).  Anything else is ignored. */
static void load_known(const char *path, U32Vec *out)
{
    char *t = read_text(path);
    char *line, *next;
    if (!t) return;
    for (line = t; *line; line = next) {
        char *end = strchr(line, '\n'), *p, *endp;
        unsigned long va;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        line[strcspn(line, "\r")] = 0;
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == ';') continue;
        if (p[0] == 'X' && p[1] == '(') {
            va = strtoul(p + 2, &endp, 16);
            if (endp == p + 2 || *endp != ')') continue;
        } else {
            va = strtoul(p, &endp, 16);
            if (endp == p) continue;
        }
        if (va) u32_add(out, (uint32_t)va);
    }
    free(t);
    u32_sort_unique(out);
}

static int is_code_target(Pe *pe, uint32_t va)
{
    return pe_code_section_for_va(pe, va) >= 0;
}

static int code_run_at(Pe *pe, const uint8_t *buf, uint32_t i,
                       uint32_t rawsize, int *zero_neighbor)
{
    int run = 1;
    uint32_t k;
    *zero_neighbor = 0;
    if (i + 4 > rawsize) return 0;
    for (k = 4; i >= k; k += 4) {
        uint32_t v = pe_rd32(buf + i - k);
        if (!is_code_target(pe, v)) {
            if (v == 0) *zero_neighbor = 1;
            break;
        }
        run++;
    }
    for (k = 4; i + k + 4 <= rawsize; k += 4) {
        uint32_t v = pe_rd32(buf + i + k);
        if (!is_code_target(pe, v)) {
            if (v == 0) *zero_neighbor = 1;
            break;
        }
        run++;
    }
    return run;
}

static int plausible_start(uint8_t b)
{
    return b != 0xCC && b != 0x90 && b != 0x00;
}

/* A cold block inside a function often uses the caller's frame without ever
 * establishing one itself, while a real function entry that is reached
 * through a function pointer either sets up ebp or avoids ebp entirely.
 * Decode the first instructions and reject a start that reads [ebp...] before
 * `push ebp` / `mov ebp, esp`.  This distinguishes Mss32's callback wrapper at
 * 0x35131933 from the EH/switch continuations in game.dll that follow a ret
 * but use the outer frame. */
static int frame_dependent_start(Pe *pe, long long off)
{
    uint8_t code[64];
    csh h;
    cs_insn *insn;
    const uint8_t *p;
    size_t len;
    uint64_t addr = 0;
    int setup = 0, saw = 0, decoded = 0, k;

    if (off < 0 || !pe_read_at(pe, off, code, sizeof code)) return 1;
    if (csg_open_x86(&h) != 0) return 1;
    insn = csg.malloc_insn(h);
    if (!insn) { csg.close(&h); return 1; }
    p = code;
    len = sizeof code;
    for (k = 0; k < 24 && len; k++) {
        if (!csg.iter(h, &p, &len, &addr, insn)) break;
        decoded = 1;
        if (!strcmp(insn->mnemonic, "push") &&
            !strcmp(insn->op_str, "ebp"))
            setup = 1;
        if (!strcmp(insn->mnemonic, "mov") &&
            (strstr(insn->op_str, "ebp, esp") ||
             strstr(insn->op_str, "ebp,esp")))
            setup = 1;
        if (strstr(insn->op_str, "ebp") && !setup) {
            saw = 1;
            break;
        }
    }
    csg.free_insn(insn, 0);
    csg.close(&h);
    return saw || !decoded;
}

/* A cold block inside a function often starts with an epilogue or an x87
 * operation; a function pointer must name a real call target instead. */
static int epilogue_or_fpu_entry(Pe *pe, long long off)
{
    uint8_t b[2];
    if (!pe_read_at(pe, off, b, sizeof b)) return 1;
    if (b[0] == 0x83 && b[1] == 0xC4) return 1;   /* add esp, imm8 */
    if (b[0] == 0x81 && b[1] == 0xC4) return 1;   /* add esp, imm32 */
    if (b[0] == 0x8B && b[1] == 0x65) return 1;   /* mov esp, ebp */
    if (b[0] == 0xC9 || b[0] == 0x5D || b[0] == 0x5E || b[0] == 0x5F)
        return 1;                                  /* leave / pop ebp-edi */
    if (b[0] >= 0xD8 && b[0] <= 0xDF) return 1;   /* x87 opcode */
    return 0;
}

/* entry.exe requires 16-byte alignment for starts after a terminator, but an
 * address-taken callback may follow the previous function's ret without
 * padding.  Writable data holds callback/vtable slots; read-only .rdata also
 * holds the CRT's small function-pointer tables (war3 0x45C96C) alongside EH
 * scope tables and switch continuations.  Reject executable sources, large
 * table runs, epilogue/FPU starts, and starts that read [ebp] before setting
 * up a frame.  A target is accepted only when the preceding instruction is a
 * complete one-byte `ret` (C3) or `ret imm16` (C2 xx xx). */
static int unaligned_address_taken_start(Pe *pe, long long off,
                                         const PeSec *sec, int source_run,
                                         int zero_neighbor)
{
    uint8_t first, before[3];
    int executable = (sec->chars & 0x20000000u) != 0;
    int max_run = 7;
    if (off < 1) return 0;
    if (executable) {
        /* A code-resident dword table is a jump table.  Its entries are
         * literal dispatch targets, including unaligned case blocks that
         * begin with an epilogue or use the caller's frame.  An isolated
         * immediate is not a table: require at least two adjacent code
         * pointers. */
        if (source_run < 2 || source_run > 4096) return 0;
    } else {
        /* Large runs are usually jump tables of internal case blocks.  A
         * zero-delimited table in read-only data is a function-pointer table
         * instead: war3 0x45C800 is the CRT's 87-entry initializer list, whose
         * first entry 0x404599 follows a ret without alignment padding.  A
         * large writable table is a compiler switch table too (ijl15 keeps one
         * in .data1); those are lifted as synthetic block edges by the caller,
         * never as whole-function seeds. */
        if ((sec->chars & 0x40000000u) && zero_neighbor) max_run = 256;
        if ((sec->chars & 0x80000000u) != 0) max_run = 4096;
        if (source_run < 1 || source_run > max_run) return 0;
    }
    if (!pe_read_at(pe, off, &first, 1) || !plausible_start(first)) return 0;
    /* A large writable table is a compiler switch table.  Its entries are
     * case blocks and need not follow a ret, establish a frame, or avoid an
     * epilogue: they are lifted in isolation, exactly like code-resident
     * switch arms.  The caller sends them through closure2's strict block
     * extent, never through the seed catalog. */
    if (!executable && (sec->chars & 0x80000000u) && source_run >= 8)
        return 1;
    if (!executable) {
        if (epilogue_or_fpu_entry(pe, off)) return 0;
        if (frame_dependent_start(pe, off)) return 0;
    }
    if (pe_read_at(pe, off - 1, before, 1) && before[0] == 0xC3)
        return 1;
    if (off >= 3 && pe_read_at(pe, off - 3, before, 3) &&
        before[0] == 0xC2)
        return 1;
    return 0;
}

/* MSVC emits an adjustor thunk for a COM interface whose implementation
 * lives in a base subobject.  A vtable slot points at
 *
 *   add ecx, imm32      81 C1 xx xx xx xx   (or 83 C1 xx for imm8)
 *   jmp target          E9 rel32             (or EB rel8)
 *
 * and the subtract form appears in vtables too.  These are real dispatch
 * entries, not the data that surrounds them: the target is an in-image code
 * address.  Recover them from the pointer table and the instruction bytes;
 * do not infer a function from alignment or from a preceding ret. */
static int adjustor_thunk_start(Pe *pe, uint32_t va, long long off)
{
    uint8_t b[12];
    uint32_t target;
    int32_t rel;
    size_t jmp_at;

    if (!pe_read_at(pe, off, b, sizeof b)) return 0;
    if (b[0] == 0x81 && (b[1] == 0xC1 || b[1] == 0xE9)) {
        jmp_at = 6;
    } else if (b[0] == 0x83 && (b[1] == 0xC1 || b[1] == 0xE9)) {
        jmp_at = 3;
    } else {
        return 0;
    }

    if (b[jmp_at] == 0xE9) {
        rel = (int32_t)pe_rd32(b + jmp_at + 1);
        target = (uint32_t)((int64_t)va + (int64_t)jmp_at + 5 + rel);
    } else if (b[jmp_at] == 0xEB) {
        rel = (int8_t)b[jmp_at + 1];
        target = (uint32_t)((int64_t)va + (int64_t)jmp_at + 2 + rel);
    } else {
        return 0;
    }
    if (target == va) return 0;
    return is_code_target(pe, target);
}

/* Intel/MSVC function alignment commonly fills the gap between two functions
 * with a multi-byte NOP instead of int3.  The last instruction before the
 * target then ends on an instruction boundary that `entry.exe` does not
 * recognise (it only looks at the immediately preceding byte for one NOP).
 * These are the encodings emitted by the Intel and MSVC compilers in the
 * 1.27b modules; all of them are semantically no-ops. */
static int nop_len_before(Pe *pe, long long off);

static int nop_padded_start(Pe *pe, long long off)
{
    uint8_t b[6];
    long long p = off;
    int total = 0;
    for (;;) {
        int n = nop_len_before(pe, p);
        if (!n) break;
        p -= n;
        total += n;
        if (total > 64) return 0;
    }
    if (!total || p < 1) return 0;
    if (!pe_read_at(pe, p - 1, b, 1)) return 0;
    if (b[0] == 0xC3 || b[0] == 0xCC) return 1;
    if (p >= 3 && pe_read_at(pe, p - 3, b, 3) && b[0] == 0xC2) return 1;
    if (p >= 2 && pe_read_at(pe, p - 2, b, 2) && b[0] == 0xEB) return 1;
    if (p >= 5 && pe_read_at(pe, p - 5, b, 5) && b[0] == 0xE9) return 1;
    if (p >= 6 && pe_read_at(pe, p - 6, b, 6) &&
        b[0] == 0xFF && b[1] == 0x25)
        return 1;
    return 0;
}

/* A compiler often lays a jump table directly in front of the next function.
 * The table has no trailing padding, so the first function byte follows the
 * last code-pointer entry.  An aligned target taken from a read-only pointer
 * table is a real function entry when the three preceding dwords are code
 * pointers and the target starts like a function (not an epilogue, FPU case
 * block, or a block that consumes the caller's frame). */
static int aligned_data_table_start(Pe *pe, long long off, const PeSec *sec)
{
    uint8_t b[4];
    int k;
    if ((sec->chars & PE_SCN_MEM_EXECUTE) != 0) return 0;
    if ((sec->chars & 0x40000000u) == 0) return 0;
    if (off < 12) return 0;
    for (k = 1; k <= 3; k++) {
        uint32_t v;
        if (!pe_read_at(pe, off - 4 * k, b, sizeof b)) return 0;
        v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
            ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        if (!is_code_target(pe, v)) return 0;
    }
    if (epilogue_or_fpu_entry(pe, off)) return 0;
    if (frame_dependent_start(pe, off)) return 0;
    return 1;
}

static int nop_len_before(Pe *pe, long long off)
{
    static const struct { const char *p; unsigned n; } pat[] = {
        { "\x90", 1 },
        { "\x66\x90", 2 },
        { "\x0F\x1F\x00", 3 },
        { "\x0F\x1F\x40\x00", 4 },
        { "\x0F\x1F\x44\x00\x00", 5 },
        { "\x66\x0F\x1F\x44\x00\x00", 6 },
        { "\x0F\x1F\x80\x00\x00\x00\x00", 7 },
        { "\x0F\x1F\x84\x00\x00\x00\x00\x00", 8 },
        { "\x8D\x76\x00", 3 },
        { "\x8D\xB4\x26\x00\x00\x00\x00", 7 },
        { "\x8D\x74\x26\x00", 4 },
        { "\x8D\xBC\x27\x00\x00\x00\x00", 7 },
        { "\x8D\x9B\x00\x00\x00\x00", 6 },
        { "\x8D\xA4\x24\x00\x00\x00\x00", 7 },
        { "\x8D\x64\x24\x00", 4 },
        { "\x8D\x49\x00", 3 },
        { "\x8D\x52\x00", 3 },
        { "\x8D\x5B\x00", 3 },
        { "\x8D\x6D\x00", 3 },
        { "\x8D\x7F\x00", 3 },
        { "\x8B\xFF", 2 },
        { "\x89\xF6", 2 },
        { "\x8B\xC0", 2 },
        { "\x89\xC0", 2 },
    };
    uint8_t b[8];
    int n;
    size_t i;
    if (off < 1) return 0;
    n = off < (long long)sizeof b ? (int)off : (int)sizeof b;
    if (!pe_read_at(pe, off - n, b, (size_t)n)) return 0;
    for (i = 0; i < sizeof pat / sizeof pat[0]; i++) {
        if (pat[i].n > (unsigned)n) continue;
        if (!memcmp(b + n - pat[i].n, pat[i].p, pat[i].n)) return (int)pat[i].n;
    }
    return 0;
}

/* A jump-table base is a dword array of code pointers inside .rdata.  The
 * candidates here are executable bytes, so this is a cheap safety net rather
 * than the main table filter in closure_pipeline. */
static int looks_like_table(Pe *pe, uint32_t va)
{
    uint32_t a, b, c;
    if (!pe_read_u32(pe, va, &a) || !pe_read_u32(pe, va + 4, &b) ||
        !pe_read_u32(pe, va + 8, &c))
        return 0;
    /* A one-entry switch table has a code pointer followed by zero padding.
     * The same bytes in a real function would mean a `push/mov` of a function
     * address followed by an all-zero instruction, which does not occur in
     * compiler output at a function entry. */
    if (is_code_target(pe, a) && (b == 0 || c == 0)) return 1;
    return is_code_target(pe, a) && is_code_target(pe, b) && is_code_target(pe, c);
}

/* A dword in an executable section can be a real immediate function address
 * (`mov [global], imm32`, `push imm32`) or the base of a switch/jump table.
 * Only the latter is an instruction operand of the form
 *   FF 24/14 <SIB> disp32      (jmp/call [index*4 + table])
 *   8B/8D <SIB> disp32         (mov/lea reg, [index*4 + table])
 * A table base is data, not a function entry, so reject it before the
 * alignment/padding rule.  This keeps genuine address-taken immediates such
 * as ijl15's `mov [0x60053cd4], 0x600048e0`. */
static int text_table_base(const uint8_t *buf, uint32_t i)
{
    if (i < 3) return 0;
    /* FF 24 <sib> disp32 / FF 14 <sib> disp32: jmp/call [index*4+table].
     * 8B/8D 04 <sib> disp32: mov/lea reg, [index*4+table].
     * The dword at `i` is the disp32, so the three opcode bytes precede it. */
    if ((buf[i - 3] == 0xFF || buf[i - 3] == 0x8B ||
         buf[i - 3] == 0x8D) &&
        (buf[i - 2] & 0xC7) == 0x04 &&
        (buf[i - 1] & 0xC7) == 0x85)
        return 1;
    return 0;
}

static int scan_section(Pe *pe, const PeSec *sec, U32Vec *out, U32Vec *edges,
                        const U32Vec *known)
{
    uint8_t *buf;
    uint32_t i;
    if (sec->rawsize < 4) return 0;
    buf = (uint8_t *)malloc(sec->rawsize);
    if (!buf) die("out of memory (section scan)");
    if (!pe_read_at(pe, sec->rawptr, buf, sec->rawsize)) {
        free(buf);
        return 0;
    }
    for (i = 0; i + 4 <= sec->rawsize; i++) {
        uint32_t v = pe_rd32(buf + i);
        uint8_t before[6], first;
        long long off;
        int jmp_tail = 0, zero_neighbor = 0, source_run = 0, adjustor = 0;
        int source_exec = (sec->chars & PE_SCN_MEM_EXECUTE) != 0;
        int source_writable = (sec->chars & 0x80000000u) != 0;
        if (!v) continue;
        if (!is_code_target(pe, v)) continue;
        if (known && u32_contains(known, v)) continue;
        if (audit_mode) {
            if (!source_exec) {
                long long toff = pe_code_off(pe, v);
                uint8_t first[8] = { 0 }, prev[8] = { 0 };
                int nf = 0, np = 0, run = 1, j;
                uint32_t k;
                if (toff < 0) continue;
                if (pe_read_at(pe, toff, first, sizeof first)) nf = (int)sizeof first;
                if (toff >= (long long)sizeof prev &&
                    pe_read_at(pe, toff - (long long)sizeof prev, prev, sizeof prev))
                    np = (int)sizeof prev;
                for (k = 4; i >= k; k += 4) {
                    if (!is_code_target(pe, pe_rd32(buf + i - k))) break;
                    run++;
                }
                for (k = 4; i + k + 4 <= sec->rawsize; k += 4) {
                    if (!is_code_target(pe, pe_rd32(buf + i + k))) break;
                    run++;
                }
                printf("AUDIT %08X src=%s+%X run=%d first=", v, sec->name, i,
                       run);
                for (j = 0; j < nf; j++) printf("%02X", first[j]);
                printf(" prev=");
                for (j = 0; j < np; j++) printf("%02X", prev[j]);
                printf("\n");
            }
            continue;
        }
        off = pe_code_off(pe, v);
        if (off < 0) continue;
        if (!source_exec) adjustor = adjustor_thunk_start(pe, v, off);
        if (off >= 6 && pe_read_at(pe, off - 6, before, sizeof before))
            jmp_tail = before[0] == 0xFF && before[1] == 0x25;
        if (!jmp_tail) {
            if ((v & 15u) != 0) {
                source_run = code_run_at(pe, buf, i, sec->rawsize,
                                         &zero_neighbor);
                if (!adjustor &&
                    !unaligned_address_taken_start(pe, off, sec, source_run,
                                                   zero_neighbor))
                    continue;
            } else {
                if (source_exec && text_table_base(buf, i)) continue;
                if (source_writable) {
                    source_run = code_run_at(pe, buf, i, sec->rawsize,
                                             &zero_neighbor);
                    if (source_run >= 8) {
                        if (!adjustor &&
                            !unaligned_address_taken_start(
                                pe, off, sec, source_run, zero_neighbor))
                            continue;
                    } else if (!adjustor && !nop_padded_start(pe, off) &&
                               !aligned_data_table_start(pe, off, sec)) {
                        continue;
                    }
                } else if (!adjustor && !nop_padded_start(pe, off) &&
                           !aligned_data_table_start(pe, off, sec)) {
                    continue;
                }
            }
        }
        if (!pe_read_at(pe, off, &first, 1)) continue;
        if (!plausible_start(first)) continue;
        if (looks_like_table(pe, v)) continue;
        /* A code-resident pointer table holds jump-table arms, i.e. mid-
         * function labels.  Lifting those from the seed catalog would run
         * liftcore's whole-function heuristic across the table that follows
         * the label.  Emit them as synthetic jump edges instead: closure2
         * gives every such target a strict first-block extent and grows it
         * only under block mode. */
        if (!jmp_tail &&
            ((source_exec && (v & 15u) != 0) ||
             (!source_exec && source_writable && source_run >= 8)))
            u32_add(edges, v);
        else
            u32_add(out, v);
    }
    free(buf);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: thunkseeds --module PE --out FILE [--known FILE] "
            "[--exclude FILE] [--edges-out FILE] [--audit]\n");
}

int main(int argc, char **argv)
{
    const char *module = NULL, *out_path = NULL, *known_path = NULL;
    const char *exclude_path = NULL, *edges_path = NULL;
    char edges_default[1024];
    U32Vec found = { 0 }, edges = { 0 }, known = { 0 }, exclude = { 0 };
    FILE *f;
    Pe pe;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) module = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--known") && i + 1 < argc) known_path = argv[++i];
        else if (!strcmp(argv[i], "--exclude") && i + 1 < argc) exclude_path = argv[++i];
        else if (!strcmp(argv[i], "--edges-out") && i + 1 < argc) edges_path = argv[++i];
        else if (!strcmp(argv[i], "--audit")) audit_mode = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (!module || (!out_path && !audit_mode)) { usage(); return 2; }
    if (!edges_path && out_path) {
        snprintf(edges_default, sizeof edges_default, "%s.edges.c", out_path);
        edges_path = edges_default;
    }
    if (!pe_open(&pe, module)) die("cannot open module as PE32");
    if (known_path) load_known(known_path, &known);
    if (exclude_path) load_known(exclude_path, &exclude);

    for (i = 0; i < pe.nsec; i++)
        scan_section(&pe, &pe.sec[i], &found, &edges, &known);
    u32_sort_unique(&found);
    u32_sort_unique(&edges);

    if (audit_mode) {
        printf("thunkseeds: audit %s: done\n", module);
        free(found.v);
        free(edges.v);
        free(known.v);
        free(exclude.v);
        pe_close(&pe);
        return 0;
    }

    f = fopen(out_path, "w");
    if (!f) { pe_close(&pe); die("cannot open --out"); }
    {
        size_t n = 0;
        for (i = 0; i < (int)found.n; i++) {
            if (u32_contains(&exclude, found.v[i])) continue;
            fprintf(f, "0x%08X 512 extra_%08X\n", found.v[i], found.v[i]);
            n++;
        }
        printf("thunkseeds: %s: %zu extra seed(s)\n", module, n);
    }
    fclose(f);

    f = fopen(edges_path, "w");
    if (!f) { pe_close(&pe); die("cannot open --edges-out"); }
    {
        size_t n = 0;
        fprintf(f, "/* AUTO-GENERATED jump-table edges; scanned by closure2, "
                   "not compiled. */\n#include \"cpu.h\"\n\n"
                   "void L_EDGES(CPU *c)\n{\n");
        for (i = 0; i < (int)edges.n; i++) {
            if (u32_contains(&exclude, edges.v[i])) continue;
            fprintf(f, "    dispatch_jmp(c, 0x%08Xu); return;\n", edges.v[i]);
            n++;
        }
        fprintf(f, "}\n");
        printf("thunkseeds: %s: %zu jump-table edge(s)\n", module, n);
    }
    fclose(f);
    free(found.v);
    free(edges.v);
    free(known.v);
    free(exclude.v);
    pe_close(&pe);
    return 0;
}
