/* gen_boot_dispatch.c - generate the boot dispatcher tables.
 *
 * The generated headers are byte-for-byte identical to the Python output:
 * the boot dispatcher's LIFTED_FUNCS list and its constant open-addressing
 * hash table are both derived here from the same ordered VA set.
 *
 *   gen_boot_dispatch --list VAs.h --out boot_dispatch_list.h \
 *                     --hash-out boot_dispatch_hash.h [--prefix 6F ...] \
 *                     [--expect-count N]
 *   gen_boot_dispatch --self-test
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir((p), 0777)
#endif

#define LIFT_HASH_BITS 19
#define LIFT_HASH_SIZE (1u << LIFT_HASH_BITS)
#define LIFT_HASH_MULTIPLIER 2654435761u

typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Buf;

static void die(const char *msg)
{
    fprintf(stderr, "gen_boot_dispatch: %s\n", msg);
    exit(2);
}

static void die_oom(void)
{
    die("out of memory");
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xrealloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

static void buf_need(Buf *b, size_t extra)
{
    size_t need = b->n + extra;
    if (need < b->cap) return;
    if (b->cap == 0) b->cap = 4096;
    while (b->cap <= need) {
        if (b->cap > (size_t)-1 / 2) die_oom();
        b->cap *= 2;
    }
    b->p = (char *)xrealloc(b->p, b->cap);
    b->p[b->n] = 0;
}

static void buf_add(Buf *b, const void *p, size_t n)
{
    buf_need(b, n);
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) die("formatting failed");
    buf_need(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
    b->p[b->n] = 0;
}

static char *slurp(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *p;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    p = (char *)xrealloc(NULL, (size_t)sz + 1);
    if (fread(p, 1, (size_t)sz, f) != (size_t)sz) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[sz] = 0;
    *out_n = (size_t)sz;
    return p;
}

static int write_all(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t put;
    if (!f) return -1;
    put = fwrite(data, 1, n, f);
    if (fclose(f) != 0 || put != n) return -1;
    return 0;
}

/* Path.write_text() uses the platform text newline.  Keep the generated
 * headers byte-identical to the Python reference on Windows too. */
static int write_text(const char *path, const char *data, size_t n)
{
#if defined(_WIN32)
    Buf tmp = {0};
    size_t i;
    for (i = 0; i < n; i++) {
        if (data[i] == '\n')
            buf_add(&tmp, "\r\n", 2);
        else
            buf_add(&tmp, data + i, 1);
    }
    {
        int rc = write_all(path, tmp.p ? tmp.p : "", tmp.n);
        free(tmp.p);
        return rc;
    }
#else
    return write_all(path, data, n);
#endif
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    char *p;
    for (p = tmp; *p; p++) {
        if ((*p == '/' || *p == '\\') && p != tmp && p[1]) {
            char c = *p;
            *p = 0;
            if (!is_dir(tmp)) make_dir(tmp);
            *p = c;
        }
    }
    if (!is_dir(tmp)) make_dir(tmp);
    if (!is_dir(tmp)) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct {
    uint32_t va;
    char text[9];
} Value;

static int value_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Value *)a)->va;
    uint32_t y = ((const Value *)b)->va;
    return x < y ? -1 : x > y ? 1 : 0;
}

static uint32_t hash_slot(uint32_t va)
{
    uint32_t h = (uint32_t)(va * LIFT_HASH_MULTIPLIER);
    return h >> (32 - LIFT_HASH_BITS);
}

static uint32_t *build_slots(const Value *values, size_t n)
{
    uint32_t *slots = (uint32_t *)calloc(LIFT_HASH_SIZE, sizeof(uint32_t));
    size_t i;
    if (!slots) die_oom();
    for (i = 0; i < n; i++) {
        uint32_t h = hash_slot(values[i].va);
        while (slots[h] != 0)
            h = (h + 1) & (LIFT_HASH_SIZE - 1);
        slots[h] = values[i].va;
    }
    return slots;
}

static void render_hash_header(Buf *out, const Value *values, size_t n)
{
    uint32_t *slots = build_slots(values, n);
    uint32_t h;
    buf_str(out,
            "/* AUTO-GENERATED by tools/lift/csrc_local/gen_boot_dispatch.c - do not edit */\n"
            "/* O-24/O-53: constant open-addressing map for the boot dispatcher.\n"
            " *\n"
            " * This is the table wc3_dispatch_init() used to rebuild on every\n");
    buf_printf(out, " * start: %zu entries, %d hash bits,\n", n, LIFT_HASH_BITS);
    buf_printf(out,
               " * g_lift_hash[%u] LiftEnt slots.  Each non-empty slot\n"
               " * stores the guest VA and the lifted function pointer directly, so\n"
               " * find_lifted() has one probe read instead of an index into g_lift[].\n"
               " * Empty slots are zero-initialised: va == 0 is the terminator.\n"
               " * No speed claim is made until G-10/PM-19 measures cold_start. */\n"
               "#ifndef WC3_BOOT_DISPATCH_HASH_H\n"
               "#define WC3_BOOT_DISPATCH_HASH_H\n"
               "\n"
               "#include <stdint.h>\n"
               "\n"
               "#define LIFT_HASH_BITS %d\n"
               "#define LIFT_HASH_SIZE (1u << LIFT_HASH_BITS)\n"
               "\n"
               "static const LiftEnt g_lift_hash[LIFT_HASH_SIZE] = {\n",
               LIFT_HASH_SIZE, LIFT_HASH_BITS);
    for (h = 0; h < LIFT_HASH_SIZE; h++) {
        if (!slots[h]) continue;
        buf_printf(out, "    [0x%05X] = { 0x%08Xu, L_%08X },\n",
                   h, slots[h], slots[h]);
    }
    buf_str(out, "};\n\n#endif\n");
    free(slots);
}

static void render_list(Buf *out, const Value *values, size_t n,
                        char **prefixes, size_t nprefix)
{
    size_t i;
    buf_str(out,
            "/* AUTO-GENERATED by tools/lift/csrc_local/gen_boot_dispatch.c - do not edit */\n");
    buf_printf(out, "/* %zu functions: prefixes ", n);
    for (i = 0; i < nprefix; i++) {
        if (i) buf_str(out, ", ");
        buf_str(out, prefixes[i]);
    }
    buf_str(out, " */\n"
                 "#ifndef WC3_BOOT_DISPATCH_LIST_H\n"
                 "#define WC3_BOOT_DISPATCH_LIST_H\n"
                 "\n"
                 "#define LIFTED_FUNCS(X) \\\n");
    for (i = 0; i < n; i++)
        buf_printf(out, "    X(%s)%s\n", values[i].text,
                   i + 1 == n ? "" : " \\");
    buf_str(out, "\n#endif\n");
}

static int parse_values(const char *text, size_t n,
                        char **prefixes, size_t nprefix,
                        Value **out_values, size_t *out_n)
{
    Value *v = NULL;
    size_t count = 0, cap = 0;
    size_t i;
    for (i = 0; i + 11 <= n; i++) {
        int j, ok = 1;
        char s[9];
        uint32_t va = 0;
        if (text[i] != 'X' || text[i + 1] != '(') continue;
        for (j = 0; j < 8; j++) {
            int h = hexval((unsigned char)text[i + 2 + j]);
            if (h < 0) { ok = 0; break; }
            va = (va << 4) | (uint32_t)h;
        }
        if (!ok || text[i + 10] != ')') continue;
        for (j = 0; j < 8; j++) {
            int c = (unsigned char)text[i + 2 + j];
            if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
            s[j] = (char)c;
        }
        s[8] = 0;
        if (nprefix) {
            size_t k;
            int match = 0;
            for (k = 0; k < nprefix; k++) {
                size_t pl = strlen(prefixes[k]);
                if (pl <= 8 && memcmp(s, prefixes[k], pl) == 0) {
                    match = 1;
                    break;
                }
            }
            if (!match) continue;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 1024;
            v = (Value *)xrealloc(v, cap * sizeof(*v));
        }
        v[count].va = va;
        memcpy(v[count].text, s, 9);
        count++;
        i += 10;
    }
    if (count > 1) qsort(v, count, sizeof(*v), value_cmp);
    {
        size_t w = 0;
        for (i = 0; i < count; i++) {
            if (w && v[w - 1].va == v[i].va) continue;
            v[w++] = v[i];
        }
        count = w;
    }
    *out_values = v;
    *out_n = count;
    return 0;
}

static int self_test(void)
{
    uint32_t *seen = (uint32_t *)calloc(LIFT_HASH_SIZE, sizeof(uint32_t));
    Value pair[2];
    Value *values;
    size_t i, n = 4096;
    uint32_t first = 0, second = 0;
    uint32_t *slots;
    Buf header = {0};

    if (!seen) die_oom();
    for (i = 0; i < 1000000; i++) {
        uint32_t va = 0x00400000u + (uint32_t)i * 4u;
        uint32_t h = hash_slot(va);
        if (seen[h]) {
            first = seen[h];
            second = va;
            break;
        }
        seen[h] = va;
    }
    free(seen);
    if (!first) die("self-test: could not find a hash collision");
    pair[0].va = first;
    snprintf(pair[0].text, sizeof(pair[0].text), "%08X", first);
    pair[1].va = second;
    snprintf(pair[1].text, sizeof(pair[1].text), "%08X", second);
    slots = build_slots(pair, 2);
    if (!slots[hash_slot(first)] || !slots[hash_slot(second)])
        die("self-test: bad collision slots");
    free(slots);

    values = (Value *)xrealloc(NULL, n * sizeof(*values));
    for (i = 0; i < n; i++) {
        values[i].va = 0x00400000u + (uint32_t)i * 4u;
        snprintf(values[i].text, sizeof(values[i].text), "%08X", values[i].va);
    }
    slots = build_slots(values, n);
    for (i = 0; i < n; i++) {
        uint32_t h = hash_slot(values[i].va);
        while (slots[h] && slots[h] != values[i].va)
            h = (h + 1) & (LIFT_HASH_SIZE - 1);
        if (slots[h] != values[i].va)
            die("self-test: lookup failed");
    }
    free(slots);
    render_hash_header(&header, values, n);
    if (!strstr(header.p, "LiftEnt g_lift_hash[LIFT_HASH_SIZE]") ||
        strstr(header.p, "static const uint32_t g_lift_hash") ||
        strstr(header.p, "g_lift[e") ||
        strstr(header.p, "index payload"))
        die("self-test: rendered header does not match the table");
    free(header.p);
    free(values);
    printf("gen_boot_dispatch self-test: OK (%zu entries, collision %08X/%08X)\n",
           n, first, second);
    return 0;
}

static void usage(void)
{
    fputs("usage: gen_boot_dispatch --list LIST --out OUT [--hash-out OUT] "
          "[--prefix P]... [--expect-count N]\n"
          "       gen_boot_dispatch --self-test\n", stderr);
}

int main(int argc, char **argv)
{
    const char *list_path = NULL;
    const char *out_path = NULL;
    char *hash_path = NULL;
    char **prefixes = NULL;
    size_t nprefix = 0;
    long expect_count = -1;
    char *text = NULL;
    size_t text_n = 0;
    Value *values = NULL;
    size_t nvalues = 0;
    Buf list = {0}, hash = {0};
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test")) return self_test();
        else if (!strcmp(argv[i], "--list") && i + 1 < argc)
            list_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--hash-out") && i + 1 < argc)
            hash_path = argv[++i];
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) {
            prefixes = (char **)xrealloc(prefixes,
                                         (nprefix + 1) * sizeof(*prefixes));
            prefixes[nprefix++] = argv[++i];
        } else if (!strcmp(argv[i], "--expect-count") && i + 1 < argc)
            expect_count = strtol(argv[++i], NULL, 10);
        else { usage(); return 2; }
    }
    if (!list_path || !out_path) {
        usage();
        return 2;
    }
    if (!nprefix) {
        prefixes = (char **)xrealloc(NULL, 2 * sizeof(*prefixes));
        prefixes[0] = "004";
        prefixes[1] = "350";
        nprefix = 2;
    }
    text = slurp(list_path, &text_n);
    if (!text) {
        fprintf(stderr, "gen_boot_dispatch: cannot read %s\n", list_path);
        return 2;
    }
    parse_values(text, text_n, prefixes, nprefix, &values, &nvalues);
    free(text);
    if (!nvalues) {
        fprintf(stderr, "gen_boot_dispatch: no VAs with the requested prefixes "
                        "in %s\n", list_path);
        return 2;
    }
    if (expect_count >= 0 && (size_t)expect_count != nvalues) {
        fprintf(stderr,
                "gen_boot_dispatch: dispatcher list has %zu functions, "
                "manifest says %ld\n", nvalues, expect_count);
        return 2;
    }
    if (nvalues > LIFT_HASH_SIZE) {
        fprintf(stderr,
                "gen_boot_dispatch: dispatcher list has %zu functions, "
                "hash table has only %u slots\n", nvalues, LIFT_HASH_SIZE);
        return 2;
    }

    render_list(&list, values, nvalues, prefixes, nprefix);
    render_hash_header(&hash, values, nvalues);

    if (hash_path) {
        const char *slash;
        char *parent;
        size_t cut;
        slash = strrchr(hash_path, '/');
        if (!slash) slash = strrchr(hash_path, '\\');
        if (slash) {
            parent = xstrdup(hash_path);
            cut = (size_t)(slash - hash_path);
            parent[cut] = 0;
            mkdir_p(parent);
            free(parent);
        }
    } else {
        /* Keep tools/build_boot_probe.ps1 working: it passes only --out and
         * already has the output directory on its include path. */
        const char *slash = strrchr(out_path, '/');
        if (!slash) slash = strrchr(out_path, '\\');
        if (slash) {
            size_t dl = (size_t)(slash - out_path);
            hash_path = (char *)xrealloc(NULL, dl + 24);
            memcpy(hash_path, out_path, dl);
            memcpy(hash_path + dl, "/boot_dispatch_hash.h", 22);
        } else {
            hash_path = "boot_dispatch_hash.h";
        }
    }
    {
        const char *slash = strrchr(out_path, '/');
        if (!slash) slash = strrchr(out_path, '\\');
        if (slash) {
            char *parent = xstrdup(out_path);
            size_t cut = (size_t)(slash - out_path);
            parent[cut] = 0;
            mkdir_p(parent);
            free(parent);
        }
    }
    if (write_text(out_path, list.p ? list.p : "", list.n) != 0 ||
        write_text(hash_path, hash.p ? hash.p : "", hash.n) != 0) {
        fprintf(stderr, "gen_boot_dispatch: cannot write %s or %s\n",
                out_path, hash_path);
        return 2;
    }
    printf("wrote %s (%zu functions)\n", out_path, nvalues);
    printf("wrote %s (%u slots)\n", hash_path, LIFT_HASH_SIZE);
    free(list.p);
    free(hash.p);
    free(values);
    free(prefixes);
    return 0;
}
