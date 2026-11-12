/*
 * scan_wc3debug.c - find product debug code outside the WC3_DEBUG guard.
 *
 * Adapted from E:\projects\warcraft3_recomp\tools\scan_wc3debug.c for the
 * Android-only tree.  Project code uses the tagged guard
 * `#if WC3_DEBUG && WC3_DEBUG_<TARGET>`; this scanner accepts both that form
 * and a bare `#if WC3_DEBUG`, and reports code outside either.
 *
 * This is a static source scanner, not a compiler.  It understands enough C
 * preprocessing to know whether a line is inside a WC3_DEBUG region, then
 * reports four kinds of problems:
 *
 *   1. output calls outside the guard whose message looks like a trace,
 *   2. debug switches (getenv("WC3_*TRACE"), debug-looking symbols) outside
 *      the guard,
 *   3. file-local symbols declared outside the guard but used only by code
 *      inside it.  Those are the "associated" declarations that must move
 *      under the guard together with the fprintf they serve,
 *   4. known diagnostic hooks and their state (for example
 *      wc3_dispatch_watch / g_dispatch_diag) that are referenced outside the
 *      guard but do not emit a message themselves.
 *
 * Errors, warnings and lifecycle status are kept.  The scanner uses a
 * deliberately small heuristic list for that; --verbose prints everything it
 * decided to keep so the list can be reviewed.  A function-like macro that
 * has a release no-op definition (the common `_LOG` pattern) is treated as a
 * guard, not as unwrapped debug code: its call sites expand to nothing when
 * WC3_DEBUG is off.
 *
 * Build (MSYS2/UCRT64 gcc, through ccache per BP-12):
 *   ccache gcc -std=c11 -O2 -Wall -Wextra -c tools/scan_wc3debug.c -o build/tmp/scan_wc3debug.o
 *   ccache gcc build/tmp/scan_wc3debug.o -o build/tmp/scan_wc3debug.exe
 *
 * Usage (from the repository root):
 *   build/tmp/scan_wc3debug.exe runtime apps
 *   build/tmp/scan_wc3debug.exe --no-symbols runtime apps
 *   build/tmp/scan_wc3debug.exe --exclude headless runtime apps
 *
 * runtime/platform/headless is the verification target, not the APK product;
 * exclude it when enforcing release hygiene for the shipped library.  The
 * standalone probe/boot programs are diagnostic tools too.
 *
 * Limits: this is a source heuristic, not a C compiler.  It evaluates simple
 * #ifdef/#if defined(WC3_DEBUG) nesting and collects file-scope static symbols,
 * macros and top-level functions.  Function-local declarations are skipped to
 * avoid name collisions.  A symbol is reported as associated-only only when
 * every in-file use is inside the guard; an outside assignment or branch is
 * shown, not hidden.  Calls to a logging macro with a release no-op branch are
 * not reported as output sites.
 *
 * Exit status: 0 = no high-confidence finding (review sites may still be
 * printed), 1 = high-confidence finding, 2 = usage/IO error.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define SCAN_PATH_MAX 4096
#define SCAN_GUARD_MAX 128
#define SCAN_HASH_MIN 1024
#define SCAN_MAX_DECL_LINES 8
#define SCAN_MAX_USE_LINES 10
#define SCAN_MAX_LINE_PRINT 180

/* ------------------------------------------------------------------ */
/* small growable string                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} Buf;
static void buf_init(Buf *b)
{
    b->s = NULL;
    b->len = 0;
    b->cap = 0;
}
static void buf_reserve(Buf *b, size_t need)
{
    if (need + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < need + 1)
        cap *= 2;
    char *p = (char *)realloc(b->s, cap);
    if (!p) {
        fprintf(stderr, "scan_wc3debug: out of memory\n");
        exit(2);
    }
    b->s = p;
    b->cap = cap;
}
static void buf_clear(Buf *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = '\0';
}
static void buf_putc(Buf *b, char c)
{
    buf_reserve(b, b->len + 1);
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}
static void buf_puts(Buf *b, const char *s)
{
    size_t n = strlen(s);
    buf_reserve(b, b->len + n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}
static void buf_free(Buf *b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}
/* ------------------------------------------------------------------ */
/* string helpers                                                     */
/* ------------------------------------------------------------------ */

static int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}
static int contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return 0;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               ascii_tolower((unsigned char)p[i]) ==
               ascii_tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}
static int equals_ci(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) !=
            ascii_tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
static int ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix)
        return 0;
    size_t ns = strlen(s);
    size_t nf = strlen(suffix);
    if (nf > ns)
        return 0;
    return equals_ci(s + ns - nf, suffix);
}
static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}
static int is_ident_start(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_ident_char(int c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static int has_word_ci(const char *s, const char *word)
{
    if (!s || !word || !*word)
        return 0;
    size_t n = strlen(word);
    for (const char *p = s; *p; p++) {
        if (p > s && (is_ident_char((unsigned char)p[-1]) || p[-1] == '_'))
            continue;
        size_t i = 0;
        while (i < n && p[i] &&
               ascii_tolower((unsigned char)p[i]) ==
               ascii_tolower((unsigned char)word[i]))
            i++;
        if (i == n && !is_ident_char((unsigned char)p[n]))
            return 1;
    }
    return 0;
}
static int is_excluded_path(const char *path, char **excludes, int n_excludes)
{
    for (int i = 0; i < n_excludes; i++) {
        if (contains_ci(path, excludes[i]))
            return 1;
    }
    return 0;
}
static int has_source_ext(const char *path)
{
    static const char *exts[] = {
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx"
    };
    const char *dot = strrchr(path, '.');
    if (!dot)
        return 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (equals_ci(dot, exts[i]))
            return 1;
    }
    return 0;
}
static int is_header_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return 0;
    return equals_ci(dot, ".h") || equals_ci(dot, ".hpp") ||
           equals_ci(dot, ".hh") || equals_ci(dot, ".hxx");
}
/* ------------------------------------------------------------------ */
/* file collection                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char **v;
    int n;
    int cap;
} StrList;
typedef struct {
    int verbose;
    int symbols;
    int all;
    int max_size_mb;
    char **excludes;
    int n_excludes;
} Options;
static void strlist_add(StrList *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        char **p = (char **)realloc(l->v, (size_t)l->cap * sizeof(char *));
        if (!p) {
            fprintf(stderr, "scan_wc3debug: out of memory\n");
            exit(2);
        }
        l->v = p;
    }
    l->v[l->n] = (char *)malloc(strlen(s) + 1);
    if (!l->v[l->n]) {
        fprintf(stderr, "scan_wc3debug: out of memory\n");
        exit(2);
    }
    strcpy(l->v[l->n], s);
    l->n++;
}
static int strlist_cmp(const void *a, const void *b)
{
    const char *const *pa = (const char *const *)a;
    const char *const *pb = (const char *const *)b;
    return strcmp(*pa, *pb);
}
static int skip_dir_name(const char *name, const Options *opt)
{
    if (opt->all)
        return 0;
    static const char *skip[] = {
        ".git", ".svn", ".hg", "node_modules", "__pycache__",
        "build", "build27", "build32", "third_party", "refs",
        ".workbuddy-ai"
    };
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
        if (equals_ci(name, skip[i]))
            return 1;
    }
    return 0;
}
#ifdef _WIN32
static void gather_path(const char *path, StrList *files, const Options *opt)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "scan_wc3debug: cannot stat %s\n", path);
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        char pattern[SCAN_PATH_MAX];
        snprintf(pattern, sizeof(pattern), "%s\\*", path);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                skip_dir_name(fd.cFileName, opt))
                continue;
            char full[SCAN_PATH_MAX];
            snprintf(full, sizeof(full), "%s\\%s", path, fd.cFileName);
            if (is_excluded_path(full, opt->excludes, opt->n_excludes))
                continue;
            gather_path(full, files, opt);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    } else if (has_source_ext(path) &&
               !is_excluded_path(path, opt->excludes, opt->n_excludes)) {
        strlist_add(files, path);
    }
}
#else
static void gather_path(const char *path, StrList *files, const Options *opt)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "scan_wc3debug: cannot open %s\n", path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[SCAN_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (skip_dir_name(e->d_name, opt))
                continue;
            gather_path(full, files, opt);
        } else if (S_ISREG(st.st_mode) && has_source_ext(full) &&
                   !is_excluded_path(full, opt->excludes, opt->n_excludes)) {
            strlist_add(files, full);
        }
    }
    closedir(d);
}
#endif

/* ------------------------------------------------------------------ */
/* line preprocessing                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int block_comment;
    int in_string;
    int in_char;
} LexState;
typedef struct {
    Buf clean;    /* comments removed, string contents kept */
    Buf tokens;   /* comments and string/char contents replaced by spaces */
    Buf literals; /* string literal contents, newline separated */
} LineInfo;
static void strip_line(const char *line, size_t len, LexState *st, LineInfo *li)
{
    buf_clear(&li->clean);
    buf_clear(&li->tokens);
    buf_clear(&li->literals);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)line[i];
        if (st->block_comment) {
            if (c == '*' && i + 1 < len && line[i + 1] == '/') {
                buf_puts(&li->clean, "  ");
                buf_puts(&li->tokens, "  ");
                i += 2;
                st->block_comment = 0;
            } else {
                buf_putc(&li->clean, ' ');
                buf_putc(&li->tokens, ' ');
                i++;
            }
            continue;
        }
        if (st->in_string) {
            if (c == '\\' && i + 1 < len) {
                char esc = line[i + 1];
                buf_putc(&li->clean, (char)c);
                buf_putc(&li->tokens, ' ');
                buf_putc(&li->clean, esc);
                buf_putc(&li->tokens, ' ');
                buf_putc(&li->literals,
                         (esc == 'n' || esc == 'r' || esc == 't') ? ' ' : esc);
                i += 2;
                continue;
            }
            if (c == '"') {
                buf_putc(&li->clean, '"');
                buf_putc(&li->tokens, ' ');
                st->in_string = 0;
                i++;
                continue;
            }
            buf_putc(&li->clean, (char)c);
            buf_putc(&li->tokens, ' ');
            buf_putc(&li->literals, (char)c);
            i++;
            continue;
        }
        if (st->in_char) {
            if (c == '\\' && i + 1 < len) {
                buf_putc(&li->clean, (char)c);
                buf_putc(&li->tokens, ' ');
                buf_putc(&li->clean, line[i + 1]);
                buf_putc(&li->tokens, ' ');
                i += 2;
                continue;
            }
            if (c == '\'') {
                buf_putc(&li->clean, '\'');
                buf_putc(&li->tokens, ' ');
                st->in_char = 0;
                i++;
                continue;
            }
            buf_putc(&li->clean, (char)c);
            buf_putc(&li->tokens, ' ');
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            while (i < len) {
                buf_putc(&li->clean, ' ');
                buf_putc(&li->tokens, ' ');
                i++;
            }
            break;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            buf_puts(&li->clean, "  ");
            buf_puts(&li->tokens, "  ");
            st->block_comment = 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            buf_putc(&li->clean, '"');
            buf_putc(&li->tokens, ' ');
            st->in_string = 1;
            i++;
            continue;
        }
        if (c == '\'') {
            buf_putc(&li->clean, '\'');
            buf_putc(&li->tokens, ' ');
            st->in_char = 1;
            i++;
            continue;
        }
        buf_putc(&li->clean, (char)c);
        buf_putc(&li->tokens, (char)c);
        i++;
    }

    /* A normal C string cannot cross a newline without a backslash.  Reset
       otherwise so a malformed string does not hide the rest of the file. */
    if (st->in_string && (len == 0 || line[len - 1] != '\\'))
        st->in_string = 0;
    if (st->in_char && (len == 0 || line[len - 1] != '\\'))
        st->in_char = 0;
    buf_putc(&li->literals, '\n');
}

/* ------------------------------------------------------------------ */
/* WC3_DEBUG guard tracking                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int kind;     /* 0 = other, 1 = ifdef/if WC3_DEBUG, 2 = ifndef WC3_DEBUG */
    int branch;   /* is this branch the debug branch? */
    int parent;   /* debug state outside this conditional */
    int debug;    /* parent || (kind != 0 && branch) */
} Guard;
static void guard_update(Guard *g)
{
    g->debug = g->parent || (g->kind != 0 && g->branch);
}
static int expr_negates_token(const char *expr, const char *token)
{
    const char *p = strstr(expr, token);
    if (!p)
        return 0;

    /* Look only immediately before this token.  A broad search for
       "!defined" would misread the Android/host guard
       "#if !defined(__ANDROID__) || defined(WC3_DEBUG)" as a
       non-debug branch. */
    size_t i = (size_t)(p - expr);
    while (i > 0 && isspace((unsigned char)expr[i - 1]))
        i--;
    if (i > 0 && expr[i - 1] == '(') {
        i--;
        while (i > 0 && isspace((unsigned char)expr[i - 1]))
            i--;
    }
    if (i >= 8 && !strncmp(expr + i - 8, "!defined", 8))
        return 1;
    if (i >= 9 && !strncmp(expr + i - 9, "! defined", 9))
        return 1;

    /* Also catch a plain "! WC3_DEBUG" reasonably close to the token. */
    const char *q = p;
    int back = 0;
    while (q > expr && back < 24) {
        q--;
        back++;
        if (*q == '!' && (q == expr || isspace((unsigned char)q[-1]) ||
                          q[-1] == '('))
            return 1;
        if (*q == '\n' || *q == ';')
            break;
    }
    return 0;
}
static int expr_negates_debug(const char *expr)
{
    return expr_negates_token(expr, "WC3_DEBUG");
}
static int expr_has_debug(const char *expr)
{
    return strstr(expr, "WC3_DEBUG") != NULL;
}
static void guard_push(Guard *stack, int *depth, int kind, int branch)
{
    if (*depth >= SCAN_GUARD_MAX) {
        (*depth)++;
        return;
    }
    Guard *g = &stack[*depth];
    g->kind = kind;
    g->branch = branch;
    g->parent = (*depth > 0) ? stack[*depth - 1].debug : 0;
    guard_update(g);
    (*depth)++;
}
static int current_debug(const Guard *stack, int depth)
{
    if (depth <= 0)
        return 0;
    if (depth > SCAN_GUARD_MAX)
        depth = SCAN_GUARD_MAX;
    return stack[depth - 1].debug;
}
static int parse_directive(const char *clean, char *word, size_t word_cap,
                           const char **rest)
{
    const char *p = skip_ws(clean);
    if (*p != '#')
        return 0;
    p++;
    p = skip_ws(p);
    size_t n = 0;
    while (is_ident_char((unsigned char)*p) && n + 1 < word_cap)
        word[n++] = *p++;
    word[n] = '\0';
    *rest = skip_ws(p);
    return n > 0;
}

/* ------------------------------------------------------------------ */
/* symbol table                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[128];
    int decl_lines[SCAN_MAX_DECL_LINES];
    int n_decl_lines;
    int decl_debug;
    int decl_debug_any;
    int decl_release_any;
    int is_static;
    int is_macro;
    int is_function;
    int is_extern;
    int next_hash;
    int uses_total;
    int uses_debug;
    int uses_outside;
    int use_lines[SCAN_MAX_USE_LINES];
    int use_debug_lines[SCAN_MAX_USE_LINES];
    int n_use_lines;
    int n_use_debug_lines;
} Symbol;
typedef struct {
    Symbol *v;
    int n;
    int cap;
    int *hash;
    int hash_cap;
} SymTab;
static void symtab_init(SymTab *t)
{
    memset(t, 0, sizeof(*t));
}
static unsigned hash_str(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}
static int symtab_find(const SymTab *t, const char *name)
{
    if (!t->hash || t->hash_cap <= 0)
        return -1;
    unsigned h = hash_str(name) & (unsigned)(t->hash_cap - 1);
    for (int i = t->hash[h]; i >= 0; i = t->v[i].next_hash) {
        if (!strcmp(t->v[i].name, name))
            return i;
    }
    return -1;
}
static int sym_is_decl_line(const Symbol *s, int line)
{
    for (int i = 0; i < s->n_decl_lines; i++) {
        if (s->decl_lines[i] == line)
            return 1;
    }
    return 0;
}
static void sym_add_decl_line(Symbol *s, int line)
{
    if (sym_is_decl_line(s, line))
        return;
    if (s->n_decl_lines < SCAN_MAX_DECL_LINES)
        s->decl_lines[s->n_decl_lines++] = line;
}
static int sym_name_is_keyword(const char *name)
{
    static const char *kw[] = {
        "if", "else", "for", "while", "do", "switch", "case", "default",
        "return", "break", "continue", "goto", "sizeof", "typeof", "defined",
        "static", "extern", "inline", "const", "volatile", "register", "auto",
        "typedef", "struct", "union", "enum", "void", "int", "char", "short",
        "long", "float", "double", "signed", "unsigned", "bool", "_Bool",
        "size_t", "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t", "uintptr_t", "intptr_t",
        "DWORD", "WORD", "BYTE", "BOOL", "HRESULT", "LPVOID", "HWND", "FILE",
        "MssTrace", "CPU", "GEvent", "GMutex", "WaveDev", "WaitItem"
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (!strcmp(name, kw[i]))
            return 1;
    }
    return 0;
}
static int is_logging_macro_name(const char *name)
{
    static const char *names[] = {
        "LOGI", "LOGE", "LOGW", "LOGD", "ALOGI", "ALOGE", "ALOGW",
        "ALOGD", "LOG_TAG", "__android_log_print"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i]))
            return 1;
    }
    return 0;
}
static int symtab_add(SymTab *t, const char *name, int line, int debug,
                      int is_static, int is_macro, int is_function,
                      int is_extern)
{
    if (!name || !*name || strlen(name) >= sizeof(t->v[0].name))
        return -1;
    if (sym_name_is_keyword(name))
        return -1;
    if (is_macro && is_logging_macro_name(name))
        return -1;
    /* Single-letter/local-style names create false collisions with function
       locals that this scanner deliberately does not collect. */
    if (strlen(name) < 4 && !(name[0] == 'g' && name[1] == '_'))
        return -1;
    if (!is_static && !is_macro && !(name[0] == 'g' && name[1] == '_'))
        return -1;
    int idx = symtab_find(t, name);
    if (idx >= 0) {
        Symbol *s = &t->v[idx];
        if (!debug && s->decl_debug) {
            s->decl_debug = 0;
            s->decl_lines[0] = line;
            s->n_decl_lines = 1;
        } else {
            sym_add_decl_line(s, line);
        }
        s->decl_debug_any |= debug;
        s->decl_release_any |= !debug;
        s->is_static |= is_static;
        s->is_macro |= is_macro;
        s->is_function |= is_function;
        s->is_extern |= is_extern;
        return idx;
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        Symbol *p = (Symbol *)realloc(t->v, (size_t)t->cap * sizeof(Symbol));
        if (!p) {
            fprintf(stderr, "scan_wc3debug: out of memory\n");
            exit(2);
        }
        t->v = p;
    }
    Symbol *s = &t->v[t->n];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->decl_lines[0] = line;
    s->n_decl_lines = 1;
    s->decl_debug = debug;
    s->decl_debug_any = debug;
    s->decl_release_any = !debug;
    s->is_static = is_static;
    s->is_macro = is_macro;
    s->is_function = is_function;
    s->is_extern = is_extern;
    s->next_hash = -1;
    return t->n++;
}
static void symtab_build_hash(SymTab *t)
{
    t->hash_cap = SCAN_HASH_MIN;
    while (t->hash_cap < t->n * 2)
        t->hash_cap *= 2;
    t->hash = (int *)malloc((size_t)t->hash_cap * sizeof(int));
    if (!t->hash) {
        fprintf(stderr, "scan_wc3debug: out of memory\n");
        exit(2);
    }
    for (int i = 0; i < t->hash_cap; i++)
        t->hash[i] = -1;
    for (int i = 0; i < t->n; i++) {
        unsigned h = hash_str(t->v[i].name) & (unsigned)(t->hash_cap - 1);
        t->v[i].next_hash = t->hash[h];
        t->hash[h] = i;
    }
}
static void symtab_free(SymTab *t)
{
    free(t->v);
    free(t->hash);
    memset(t, 0, sizeof(*t));
}
static int type_prefix(const char *s)
{
    static const char *prefix[] = {
        "static", "extern", "inline", "const", "volatile", "register",
        "auto", "typedef", "struct", "union", "enum", "void", "int",
        "char", "short", "long", "float", "double", "signed", "unsigned",
        "bool", "_Bool", "size_t", "ssize_t", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
        "uintptr_t", "intptr_t", "DWORD", "WORD", "BYTE", "BOOL",
        "HRESULT", "LPVOID", "HWND", "FILE"
    };
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++) {
        if (has_word_ci(s, prefix[i]))
            return 1;
    }
    /* Common project typedefs: only look at the first token. */
    const char *p = skip_ws(s);
    const char *q = p;
    while (is_ident_char((unsigned char)*q))
        q++;
    size_t n = (size_t)(q - p);
    if (n > 2 && p[n - 2] == '_' && p[n - 1] == 't')
        return 1;
    return 0;
}
static int function_name_before_paren(const char *s, char *out, size_t cap)
{
    const char *paren = strchr(s, '(');
    if (!paren)
        return 0;
    const char *p = paren;
    while (p > s && isspace((unsigned char)p[-1]))
        p--;
    while (p > s && p[-1] == '*')
        p--;
    const char *end = p;
    while (p > s && is_ident_char((unsigned char)p[-1]))
        p--;
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= cap)
        return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}
static void collect_declaration(const char *clean, const char *tokens,
                                int line, int debug, SymTab *tab)
{
    /* Only file-scope declarations are associated-code candidates.  Locals
       are indented and must not be matched against same-named globals. */
    if (clean != skip_ws(clean))
        return;
    const char *trim = skip_ws(clean);
    if (!*trim || *trim == '#')
        return;
    if (!type_prefix(tokens))
        return;
    int is_static = has_word_ci(tokens, "static");
    int is_extern = has_word_ci(tokens, "extern");

    /* Function or function prototype: the first '(' is before the first '='
       and there is a type-looking prefix. */
    const char *first_paren = strchr(tokens, '(');
    const char *first_eq = strchr(tokens, '=');
    if (first_paren && (!first_eq || first_paren < first_eq) &&
        !has_word_ci(trim, "if") && !has_word_ci(trim, "for") &&
        !has_word_ci(trim, "while") && !has_word_ci(trim, "switch") &&
        !has_word_ci(trim, "return") && !strstr(tokens, "(*") &&
        !strstr(tokens, "( *")) {
        char name[128];
        if (function_name_before_paren(tokens, name, sizeof(name)) &&
            !sym_name_is_keyword(name)) {
            symtab_add(tab, name, line, debug, is_static, 0, 1, is_extern);
            return;
        }
    }

    /* Variable declaration.  Walk tokens at top level and keep the last
       identifier before '=', ';', ',' or '['.  That skips the type name,
       struct body members and array dimensions. */
    int brace = 0;
    int paren = 0;
    int done = 0;
    char last[128];
    last[0] = '\0';
    for (const char *p = tokens; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '{') {
            brace++;
            p++;
            continue;
        }
        if (c == '}') {
            if (brace > 0)
                brace--;
            p++;
            continue;
        }
        if (c == '(') {
            paren++;
            p++;
            continue;
        }
        if (c == ')') {
            if (paren > 0)
                paren--;
            p++;
            continue;
        }
        if (brace == 0 && paren == 0) {
            if (c == '=' || c == ';' || c == ',' || c == '[' || c == ':') {
                if (!done && last[0] &&
                    (!is_static || 1) &&
                    (is_static || (last[0] == 'g' && last[1] == '_'))) {
                    symtab_add(tab, last, line, debug, is_static, 0, 0,
                               is_extern);
                }
                last[0] = '\0';
                done = 0;
                if (c == ';')
                    break;
                if (c == '=')
                    done = 1;
                if (c == '[')
                    done = 1;
                p++;
                continue;
            }
            if (has_word_ci(p, "__attribute__") ||
                has_word_ci(p, "__declspec")) {
                if (!done && last[0] &&
                    (is_static || (last[0] == 'g' && last[1] == '_')))
                    symtab_add(tab, last, line, debug, is_static, 0, 0,
                               is_extern);
                last[0] = '\0';
                done = 1;
                p++;
                continue;
            }
        }
        if (is_ident_start(c)) {
            const char *start = p;
            while (is_ident_char((unsigned char)*p))
                p++;
            size_t n = (size_t)(p - start);
            if (brace == 0 && paren == 0 && n < sizeof(last)) {
                memcpy(last, start, n);
                last[n] = '\0';
            }
            continue;
        }
        p++;
    }
}

/* ------------------------------------------------------------------ */
/* finding/reporting state                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *path;
    int *line_flags; /* bit 0: output, bit 1: env, bit 2: marker,
                        bit 3: diagnostic hook/state */
    int nlines;
    int findings;
    int kept;
    int review;
    int verbose;
} FileState;
static void print_line_excerpt(const char *line)
{
    const char *p = skip_ws(line);
    int n = 0;
    while (*p && *p != '\n' && n < SCAN_MAX_LINE_PRINT) {
        putchar(*p++);
        n++;
    }
    if (*p && n == SCAN_MAX_LINE_PRINT)
        printf("...");
}
static void report_finding(FileState *fs, int line, const char *category,
                           const char *detail, const char *source_line)
{
    if (!strncmp(category, "review-", 7))
        fs->review++;
    else
        fs->findings++;
    printf("%s:%d: %s: %s\n", fs->path, line, category, detail);
    if (source_line) {
        printf("    ");
        print_line_excerpt(source_line);
        printf("\n");
    }
    if (fs->line_flags && line > 0 && line <= fs->nlines)
        fs->line_flags[line - 1] |= 1;
}

/* ------------------------------------------------------------------ */
/* message classification                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CLASS_ERROR,
    CLASS_STATUS,
    CLASS_DEBUG,
    CLASS_REVIEW
} MsgClass;
static int has_any_ci(const char *s, const char *const *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (contains_ci(s, words[i]))
            return 1;
    }
    return 0;
}
static int looks_like_error(const char *s)
{
    static const char *words[] = {
        "error", "failed", "failure", "cannot", "can't", "unable",
        "invalid", "missing ", "not found", "no such", "unresolved",
        "exhausted", "refused", "timed out", "timeout", "unimplemented",
        "bad ", "warning", "unavailable", "unsupported", "denied", "abort",
        "fatal", "crash", "terminate", "exitprocess", "exit(", "exit",
        "exiting", "purecall", "watson", "unwind", "overlap",
        "crosses 4 gb", "ignored", "stack ", "no dxvk", "not mapped",
        "has no", "will not", "cannot map", "full", "leak", "corrupt",
        "mismatch", "overflow", "underflow", "stale", "unexpected"
    };
    return has_any_ci(s, words, sizeof(words) / sizeof(words[0]));
}
static int looks_like_status(const char *s)
{
    static const char *words[] = {
        "created", "destroyed", "register n", "registernatives ok",
        "guest returned", "data dir", "oncreate", "activity created",
        "input queue", "window created", "window destroyed",
        "loaded", "starting", "started", "stopped", "ready", "usage"
    };
    return has_any_ci(s, words, sizeof(words) / sizeof(words[0]));
}
static int looks_like_debug(const char *s)
{
    static const char *tags[] = {
        "[mss]", "[mss-api]", "[mss-file]", "[mss-obj]", "[mss-list]",
        "[mss-3d]", "[mss-vol]", "[mss-game]", "[mss-str]", "[wave]",
        "[disp]", "[tree]", "[fn]", "[tex]", "[obj]", "[reg]", "[sh]",
        "[file]", "[find]", "[path]", "[crt]", "[va]", "[d3d9]", "[dbg]",
        "[probe]", "[pe]", "[host]", "[asi]", "[ui]", "[lift]", "[smoke]"
    };
    static const char *words[] = {
        "trace", "debug", "diag", "probe", "dump", "watch", "fake",
        "hooking", "calling ", "base=", "entry=",
        "unresolved=", "eax=", "esp=", "eip=", "stk=", "ra=",
        "handle=", "value=", "state=", "path=", "stream", "sample",
        "mss", "disp", "tree", "temporary", "deferred", "boot probe",
        "milestone", "pass", "fail", "smoke", "key action", "touch ",
        "action=", "pointer="
    };
    if (has_any_ci(s, tags, sizeof(tags) / sizeof(tags[0])))
        return 1;
    return has_any_ci(s, words, sizeof(words) / sizeof(words[0]));
}
static MsgClass classify_output(const char *call, const char *msg)
{
    if (equals_ci(call, "OutputDebugStringA") ||
        equals_ci(call, "OutputDebugStringW"))
        return CLASS_DEBUG;
    if (equals_ci(call, "LOGE") || equals_ci(call, "ALOGE") ||
        equals_ci(call, "LOGW") || equals_ci(call, "ALOGW"))
        return CLASS_ERROR;
    if (equals_ci(call, "perror"))
        return CLASS_ERROR;
    if (looks_like_error(msg))
        return CLASS_ERROR;
    /* The generated dispatcher prints this immediately before abort(); it is
       the fatal error report, not a trace.  The [probe] variant stays debug. */
    if (contains_ci(msg, "[dispatch]") &&
        contains_ci(msg, "unknown guest target"))
        return CLASS_ERROR;
    if (looks_like_debug(msg))
        return CLASS_DEBUG;
    if (equals_ci(call, "LOGI") || equals_ci(call, "ALOGI") ||
        looks_like_status(msg))
        return CLASS_STATUS;
    if (contains_ci(msg, "%s") && strlen(msg) < 4)
        return CLASS_REVIEW;
    return CLASS_REVIEW;
}

/* ------------------------------------------------------------------ */
/* output-call scanning                                               */
/* ------------------------------------------------------------------ */

static int is_output_call_name(const char *name)
{
    static const char *names[] = {
        "fprintf", "printf", "vfprintf", "vprintf", "fputs", "puts",
        "putchar", "fputc", "perror", "__android_log_print",
        "OutputDebugStringA", "OutputDebugStringW", "LOGI", "LOGE",
        "LOGW", "LOGD", "ALOGI", "ALOGE", "ALOGW", "ALOGD"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i]))
            return 1;
    }
    return 0;
}
static int find_output_call(const char *tokens, char *call, size_t call_cap)
{
    for (const char *p = tokens; *p; ) {
        if (!is_ident_start((unsigned char)*p)) {
            p++;
            continue;
        }
        const char *start = p;
        while (is_ident_char((unsigned char)*p))
            p++;
        size_t n = (size_t)(p - start);
        if (n < call_cap) {
            char tmp[64];
            if (n < sizeof(tmp)) {
                memcpy(tmp, start, n);
                tmp[n] = '\0';
                if (is_output_call_name(tmp)) {
                    snprintf(call, call_cap, "%s", tmp);
                    return 1;
                }
            }
        }
    }
    /* std::cout / std::cerr are tokenized separately. */
    if (strstr(tokens, "cout") && strstr(tokens, "cout") > tokens) {
        const char *p = strstr(tokens, "cout");
        if (p >= tokens + 2 && p[-1] == ':' && p[-2] == ':') {
            snprintf(call, call_cap, "std::cout");
            return 1;
        }
    }
    if (strstr(tokens, "cerr") && strstr(tokens, "cerr") > tokens) {
        const char *p = strstr(tokens, "cerr");
        if (p >= tokens + 2 && p[-1] == ':' && p[-2] == ':') {
            snprintf(call, call_cap, "std::cerr");
            return 1;
        }
    }
    return 0;
}
static int call_is_stdio_output(const char *tokens, const char *call)
{
    /* fprintf()/vfprintf() to an arbitrary FILE* is data serialization, not
     * diagnostics.  The registry and boot-session writers in this tree use
     * fprintf(file, ...) on purpose and must not be guarded. */
    if (!strcmp(call, "fprintf") || !strcmp(call, "vfprintf")) {
        const char *p = strchr(tokens, '(');
        if (!p)
            return 0;
        p = skip_ws(p + 1);
        return !strncmp(p, "stderr", 6) || !strncmp(p, "stdout", 6);
    }
    if (!strcmp(call, "fputs") || !strcmp(call, "fputc")) {
        /* The stream is the last argument; checking for either standard
         * stream is enough because a file variable named stderr would itself
         * be a bug. */
        return strstr(tokens, "stderr") != NULL ||
               strstr(tokens, "stdout") != NULL;
    }
    return 1;
}
static void collect_statement_msg(LineInfo *li, int nlines, int start,
                                  Buf *msg)
{
    buf_clear(msg);
    int balance = 0;
    int seen_open = 0;
    for (int j = start; j < nlines && j < start + 24; j++) {
        const char *t = li[j].tokens.s ? li[j].tokens.s : "";
        for (const char *p = t; *p; p++) {
            if (*p == '(') {
                balance++;
                seen_open = 1;
            } else if (*p == ')') {
                balance--;
            }
        }
        if (li[j].literals.s && li[j].literals.len)
            buf_puts(msg, li[j].literals.s);
        if (seen_open && balance <= 0 && strchr(t, ';'))
            break;
        if (!seen_open && strchr(t, ';'))
            break;
    }
}
static int recent_error_output(const char *const *lines, int start,
                               int lookback)
{
    for (int j = start - 1; j >= 0 && j >= start - lookback; j--) {
        const char *s = lines[j];
        if ((contains_ci(s, "fprintf") || contains_ci(s, "printf") ||
             contains_ci(s, "LOGE") || contains_ci(s, "ALOGE")) &&
            (looks_like_error(s) ||
             (contains_ci(s, "[dispatch]") &&
              contains_ci(s, "unknown guest target"))))
            return 1;
    }
    return 0;
}
static int macro_defined_under_debug(LineInfo *li, int nlines, int *debug_line,
                                     const char *name)
{
    for (int i = 0; i < nlines; i++) {
        const char *t = skip_ws(li[i].clean.s ? li[i].clean.s : "");
        char word[64];
        const char *rest;
        if (*t != '#' || !debug_line[i] ||
            !parse_directive(t, word, sizeof(word), &rest) ||
            strcmp(word, "define"))
            continue;
        const char *p = skip_ws(rest);
        size_t n = strlen(name);
        if (!strncmp(p, name, n) && !is_ident_char((unsigned char)p[n]))
            return 1;
    }
    return 0;
}
static void scan_outputs(FileState *fs, LineInfo *li, int *debug_line,
                         const char *const *lines)
{
    Buf msg;
    buf_init(&msg);
    char call[64];
    for (int i = 0; i < fs->nlines; i++) {
        const char *trim = skip_ws(li[i].clean.s ? li[i].clean.s : "");
        if (*trim == '#')
            continue;
        if (!li[i].tokens.s || !find_output_call(li[i].tokens.s, call,
                                                  sizeof(call)))
            continue;
        if (!call_is_stdio_output(li[i].tokens.s, call))
            continue;
        if (is_logging_macro_name(call) &&
            macro_defined_under_debug(li, fs->nlines, debug_line, call))
            continue;
        /* Ignore the implementation line of OutputDebugStringA/W itself. */
        if (type_prefix(trim) &&
            (contains_ci(trim, "OutputDebugStringA(") ||
             contains_ci(trim, "OutputDebugStringW(")))
            continue;
        if (fs->line_flags)
            fs->line_flags[i] |= 1;
        collect_statement_msg(li, fs->nlines, i, &msg);
        const char *m = msg.s ? msg.s : "";
        MsgClass cls = classify_output(call, m);

        /* fprintf(stderr, "%s", s) inside OutputDebugStringA/W has no own
           literal; classify by the enclosing helper name. */
        if (cls == CLASS_REVIEW && contains_ci(call, "fprintf")) {
            for (int j = i; j >= 0 && j >= i - 4; j--) {
                if (contains_ci(lines[j], "OutputDebugString")) {
                    cls = CLASS_DEBUG;
                    break;
                }
            }
        }

        /* Multi-line detail immediately after a FAILED/error report belongs to
           that report, not to routine tracing. */
        if ((cls == CLASS_DEBUG || cls == CLASS_REVIEW) &&
            recent_error_output(lines, i, 24))
            cls = CLASS_ERROR;
        if (!debug_line[i]) {
            if (cls == CLASS_ERROR || cls == CLASS_STATUS) {
                fs->kept++;
                if (fs->verbose) {
                    printf("%s:%d: kept-%s: ", fs->path, i + 1,
                           cls == CLASS_ERROR ? "error" : "status");
                    print_line_excerpt(lines[i]);
                    printf("\n");
                }
            } else {
                char detail[512];
                snprintf(detail, sizeof(detail), "%s output outside WC3_DEBUG: %s",
                         cls == CLASS_DEBUG ? "debug" : "review",
                         m[0] ? m : "(no literal message)");
                report_finding(fs, i + 1,
                               cls == CLASS_DEBUG ? "unwrapped-debug-output"
                                                  : "review-output",
                               detail, lines[i]);
            }
        }
    }
    buf_free(&msg);
}

/* ------------------------------------------------------------------ */
/* env-var and debug-marker scanning                                  */
/* ------------------------------------------------------------------ */

static int first_string_literal(const char *s, char *out, size_t cap)
{
    const char *p = strchr(s, '"');
    if (!p)
        return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            out[n++] = p[1];
            p += 2;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return n > 0;
}
static int is_functional_env(const char *name)
{
    static const char *names[] = {
        /* Product switches: these change what the shipped game does, so they
         * are deliberately available without WC3_DEBUG. */
        "WC3_LOAD_GAME", "WC3_D3D9", "WC3_NOSOUND",
        "WC3_ARGS", "WC3_DATA_DIR", "WC3_GAME_DATA_DIR",
        "WC3_DXVK_VERSION", "WC3_DXVK_WIDTH", "WC3_DXVK_HEIGHT",
        "WC3_THIN_TEX_ALPHA", "WC3_THIN_WO_SKIP",
        "DXVK_STATE_CACHE", "DXVK_HUD", "DXVK_FILTER_DEVICE_NAME"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i]))
            return 1;
    }
    return 0;
}
static int is_debug_env(const char *name)
{
    static const char *words[] = {
        "TRACE", "DEBUG", "DIAG", "DUMP", "WATCH", "FAKE", "NOBLOCK",
        "NO_BLOCK", "TREE", "TEX", "OBJ_WATCH", "A_TRACE", "VA_TRACE",
        "IO_TRACE", "D3D9_TRACE", "WAIT_SEC", "SKIP_MSS", "COOKIE",
        "OBJ_DUMP", "WAVEOUT_TRACE", "WAVEOUT_FAKE", "_FN", "PROBE",
        "REG_GUARD"
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (contains_ci(name, words[i]))
            return 1;
    }
    return 0;
}
static int is_crash_report_env(const char *name)
{
    /* These are optional detail controls for the crash report, not traces.
       They are intentionally available in release builds. */
    return !strcmp(name, "WC3_COOKIE") || !strcmp(name, "WC3_OBJ_DUMP");
}
static void scan_envs(FileState *fs, LineInfo *li, int *debug_line,
                      const char *const *lines)
{
    for (int i = 0; i < fs->nlines; i++) {
        const char *trim = skip_ws(li[i].clean.s ? li[i].clean.s : "");
        if (debug_line[i] || *trim == '#' || !li[i].clean.s)
            continue;
        const char *g = strstr(li[i].clean.s, "getenv");
        if (!g)
            continue;
        const char *arg = strchr(g, '(');
        if (!arg)
            continue;
        char name[128];
        if (!first_string_literal(arg, name, sizeof(name)))
            continue;
        if (is_functional_env(name))
            continue;
        if (is_crash_report_env(name)) {
            if (fs->verbose)
                printf("%s:%d: kept-crash-env: %s is a crash-report detail "
                       "control\\n", fs->path, i + 1, name);
            continue;
        }
        if (is_debug_env(name)) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "debug environment switch %s is read outside WC3_DEBUG", name);
            report_finding(fs, i + 1, "unwrapped-debug-env", detail, lines[i]);
            if (fs->line_flags)
                fs->line_flags[i] |= 2;
        } else if (strncmp(name, "WC3_", 4) == 0) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "unrecognised WC3_ environment switch %s", name);
            report_finding(fs, i + 1, "review-env", detail, lines[i]);
        }
    }
}
static int is_debug_tag(const char *tag)
{
    static const char *tags[] = {
        "[mss]", "[mss-api]", "[mss-file]", "[mss-obj]", "[mss-list]",
        "[mss-3d]", "[mss-vol]", "[mss-game]", "[mss-str]", "[wave]",
        "[disp]", "[tree]", "[fn]", "[tex]", "[obj]", "[reg]", "[sh]",
        "[file]", "[find]", "[path]", "[crt]", "[va]", "[d3d9]", "[dbg]",
        "[probe]", "[pe]", "[asi]", "[ui]", "[lift]", "[smoke]"
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (!strcmp(tag, tags[i]))
            return 1;
    }
    return 0;
}
static int literal_has_debug_marker(const char *lit)
{
    if (!lit || !*lit)
        return 0;
    /* A debug message can be passed to a helper rather than fprintf.  Only
       match explicit project tags or probe phrases here; matching generic
       substrings like "mss" turns Mss32.dll and DISPLAY1 into false hits. */
    if (looks_like_error(lit) || looks_like_status(lit))
        return 0;
    const char *p = skip_ws(lit);
    if (*p == '[') {
        const char *end = strchr(p, ']');
        if (end) {
            char tag[64];
            size_t n = (size_t)(end - p) + 1;
            if (n < sizeof(tag)) {
                memcpy(tag, p, n);
                tag[n] = '\0';
                if (is_debug_tag(tag))
                    return 1;
            }
        }
    }
    static const char *phrases[] = {
        "android boot probe:", "android dispatch smoke:", "android lift smoke:",
        "android pe smoke:", "trace target", "debug trace", "debug dump",
        "diagnostic dump", "boot probe:"
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (contains_ci(lit, phrases[i]))
            return 1;
    }
    return 0;
}
static void scan_markers(FileState *fs, LineInfo *li, int *debug_line,
                         const char *const *lines)
{
    for (int i = 0; i < fs->nlines; i++) {
        const char *trim = skip_ws(li[i].clean.s ? li[i].clean.s : "");
        if (debug_line[i] || *trim == '#' || !li[i].literals.s)
            continue;
        if (fs->line_flags && (fs->line_flags[i] & 3))
            continue; /* already reported as an output call or env switch */
        if (!literal_has_debug_marker(li[i].literals.s))
            continue;
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "debug-looking message string outside WC3_DEBUG");
        report_finding(fs, i + 1, "unwrapped-debug-marker", detail, lines[i]);
        if (fs->line_flags)
            fs->line_flags[i] |= 4;
    }
}

/* ------------------------------------------------------------------ */
/* diagnostic hooks and state                                         */
/* ------------------------------------------------------------------ */

static int find_listed_ident(const char *tokens, const char *const *names,
                             size_t nnames, char *out, size_t out_cap)
{
    for (const char *p = tokens; *p; ) {
        if (!is_ident_start((unsigned char)*p)) {
            p++;
            continue;
        }
        const char *start = p;
        while (is_ident_char((unsigned char)*p))
            p++;
        size_t n = (size_t)(p - start);
        if (n >= out_cap || n >= 128)
            continue;
        char tmp[128];
        memcpy(tmp, start, n);
        tmp[n] = '\0';
        for (size_t i = 0; i < nnames; i++) {
            if (!strcmp(tmp, names[i])) {
                snprintf(out, out_cap, "%s", tmp);
                return 1;
            }
        }
    }
    return 0;
}
static int is_generated_dispatch_abi(const char *name)
{
    /* The generated corpus and its no-op fallbacks resolve these symbols even
     * when the HLE side is a graphics-only debug build.  They are part of the
     * frozen guest ABI, not a diagnostic that can be hidden in release: taking
     * them out makes the dispatcher fail to resolve its own hooks. */
    static const char *names[] = {
        "g_dispatch_diag", "g_stack_guard",
        "wc3_trace_dispatch", "wc3_dispatch_watch",
        "wc3_host_stack_guard", "wc3_stack_balance"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i]))
            return 1;
    }
    return 0;
}
static void scan_debug_refs(FileState *fs, LineInfo *li, int *debug_line,
                            const char *const *lines)
{
    static const char *hooks[] = {
        "wc3_trace_dispatch", "wc3_dispatch_watch",
        "wc3_host_stack_guard", "wc3_stack_balance"
    };
    static const char *states[] = {
        "g_dispatch_diag", "g_stack_guard",
        "g_disp_ring", "g_disp_pos", "g_disp_len"
    };
    if (is_header_path(fs->path))
        return;
    for (int i = 0; i < fs->nlines; i++) {
        const char *trim = skip_ws(li[i].clean.s ? li[i].clean.s : "");
        if (debug_line[i] || *trim == '#' || !li[i].tokens.s)
            continue;
        char name[128];
        const char *category = NULL;
        const char *kind = NULL;
        if (find_listed_ident(li[i].tokens.s, hooks,
                              sizeof(hooks) / sizeof(hooks[0]),
                              name, sizeof(name))) {
            category = "unwrapped-debug-hook";
            kind = "diagnostic hook";
        } else if (find_listed_ident(li[i].tokens.s, states,
                                     sizeof(states) / sizeof(states[0]),
                                     name, sizeof(name))) {
            category = "unwrapped-debug-state";
            kind = "diagnostic state";
        }
        if (!category)
            continue;
        if (is_generated_dispatch_abi(name)) {
            fs->kept++;
            if (fs->verbose) {
                printf("%s:%d: kept-generated-abi: %s %s\n", fs->path, i + 1,
                       kind, name);
            }
            continue;
        }
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "%s %s is referenced outside WC3_DEBUG", kind, name);
        report_finding(fs, i + 1, category, detail, lines[i]);
        if (fs->line_flags)
            fs->line_flags[i] |= 8;
    }
}

/* ------------------------------------------------------------------ */
/* symbol uses and associated-code analysis                           */
/* ------------------------------------------------------------------ */

static void record_use(Symbol *s, int line, int debug)
{
    s->uses_total++;
    if (debug) {
        s->uses_debug++;
        if (s->n_use_debug_lines < SCAN_MAX_USE_LINES)
            s->use_debug_lines[s->n_use_debug_lines++] = line;
    } else {
        s->uses_outside++;
        if (s->n_use_lines < SCAN_MAX_USE_LINES)
            s->use_lines[s->n_use_lines++] = line;
    }
}
static void scan_uses(SymTab *tab, LineInfo *li, int *debug_line,
                      int nlines)
{
    for (int i = 0; i < nlines; i++) {
        const char *p = skip_ws(li[i].tokens.s ? li[i].tokens.s : "");
        if (*p == '#')
            continue;
        p = li[i].tokens.s ? li[i].tokens.s : "";
        while (*p) {
            if (!is_ident_start((unsigned char)*p)) {
                p++;
                continue;
            }
            const char *start = p;
            while (is_ident_char((unsigned char)*p))
                p++;
            size_t n = (size_t)(p - start);
            if (n >= sizeof(tab->v[0].name))
                continue;
            char name[128];
            memcpy(name, start, n);
            name[n] = '\0';
            int idx = symtab_find(tab, name);
            if (idx < 0)
                continue;
            Symbol *s = &tab->v[idx];
            if (sym_is_decl_line(s, i + 1))
                continue;
            record_use(s, i + 1, debug_line[i]);
        }
    }
}
static int is_debug_name(const char *name)
{
    if (contains_ci(name, "fake_next"))
        return 0; /* functional synthetic-handle allocator */
    if (!strcmp(name, "g_mss_trace") || !strcmp(name, "h_mss_trace"))
        return 0; /* functional Mss32 export table / hook */
    if (ends_with_ci(name, "_log") || contains_ci(name, "_log_"))
        return 1;
    static const char *words[] = {
        "trace", "debug", "diag", "dump", "watch", "fake",
        "noblock", "no_block", "wait_sec", "skip_mss", "cookie",
        "obj_dump"
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (contains_ci(name, words[i]))
            return 1;
    }
    return 0;
}
static const char *symbol_kind(const Symbol *s)
{
    if (s->is_function)
        return "function";
    if (s->is_macro)
        return "macro";
    return "variable";
}
static void print_use_lines(const char *path, const char *const *lines,
                            const int *v, int n)
{
    for (int i = 0; i < n; i++) {
        int line = v[i];
        printf("    use at %s:%d: ", path, line);
        if (line > 0)
            print_line_excerpt(lines[line - 1]);
        printf("\n");
    }
}
static void report_symbols(FileState *fs, SymTab *tab,
                           const char *const *lines)
{
    for (int i = 0; i < tab->n; i++) {
        Symbol *s = &tab->v[i];
        if (s->is_extern || (!s->is_static && !s->is_macro))
            continue;
        if (s->uses_total == 0)
            continue;
        /* A macro is a compile-time guard, not a runtime instruction.  The
         * common `_LOG` macros have a release no-op branch; their call sites
         * are handled by scan_outputs(), where the macro definition can be
         * checked.  Reporting every use again as a symbol is pure noise. */
        if (s->is_macro)
            continue;
        /* A symbol with both a debug and a release definition is the usual
         * `#if ... real #else no-op #endif` fallback.  It is intentional,
         * not associated debug-only code. */
        if (s->decl_debug_any && s->decl_release_any)
            continue;

        /* Function names still get the associated-only check above/below,
           but a function used outside the guard is a real API even when its
           name contains "debug" (for example h_output_debug_string). */
        int debug_name = !s->is_function && is_debug_name(s->name);
        if (!s->decl_debug && s->uses_outside == 0 && s->uses_debug > 0) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                     "%s %s %s is declared outside WC3_DEBUG but used only "
                     "inside it (%d use%s)",
                     s->is_static ? "static" : "file-local",
                     symbol_kind(s), s->name, s->uses_debug,
                     s->uses_debug == 1 ? "" : "s");
            report_finding(fs, s->decl_lines[0], "associated-debug-only",
                           detail, lines[s->decl_lines[0] - 1]);
            print_use_lines(fs->path, lines, s->use_debug_lines,
                            s->n_use_debug_lines);
            continue;
        }
        if (s->decl_debug && s->uses_outside > 0) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                     "%s %s %s is declared under WC3_DEBUG but used outside "
                     "it (%d outside use%s)",
                     s->is_static ? "static" : "file-local",
                     symbol_kind(s), s->name, s->uses_outside,
                     s->uses_outside == 1 ? "" : "s");
            report_finding(fs, s->decl_lines[0], "debug-decl-used-outside",
                           detail, lines[s->decl_lines[0] - 1]);
            print_use_lines(fs->path, lines, s->use_lines, s->n_use_lines);
            continue;
        }
        if (!s->decl_debug && s->uses_outside > 0 && debug_name) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                     "%s %s %s has a debug-shaped name and is used outside "
                     "WC3_DEBUG (%d outside use%s, %d guarded use%s)",
                     s->is_static ? "static" : "file-local",
                     symbol_kind(s), s->name, s->uses_outside,
                     s->uses_outside == 1 ? "" : "s", s->uses_debug,
                     s->uses_debug == 1 ? "" : "s");
            report_finding(fs, s->decl_lines[0], "debug-symbol-outside",
                           detail, lines[s->decl_lines[0] - 1]);
            print_use_lines(fs->path, lines, s->use_lines, s->n_use_lines);
            continue;
        }
        if (fs->verbose && !s->decl_debug && s->uses_debug > 0 &&
            s->uses_outside > 0) {
            printf("%s:%d: shared-debug-symbol: %s %s %s is used both inside "
                   "and outside WC3_DEBUG; not purely debug-only\n",
                   fs->path, s->decl_lines[0], symbol_kind(s), s->name,
                   s->is_static ? "(static)" : "(file-local)");
        }
    }
}

/* ------------------------------------------------------------------ */
/* one file                                                           */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len, int max_mb)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "scan_wc3debug: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (max_mb > 0 && sz > (long)max_mb * 1024L * 1024L) {
        fprintf(stderr, "scan_wc3debug: skipping %s (%ld bytes > %d MB)\n",
                path, sz, max_mb);
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *data = (char *)malloc((size_t)sz + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[got] = '\0';
    *out_len = got;
    return data;
}
static char **split_lines(char *data, size_t len, int *out_n)
{
    int n = 1;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            n++;
    }
    char **lines = (char **)malloc((size_t)n * sizeof(char *));
    if (!lines)
        return NULL;
    int k = 0;
    char *p = data;
    lines[k++] = p;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            data[i] = '\0';
            if (i > 0 && data[i - 1] == '\r')
                data[i - 1] = '\0';
            if (i + 1 <= len)
                lines[k++] = data + i + 1;
        }
    }
    *out_n = k;
    return lines;
}
static void scan_file(const char *path, const Options *opt, int *files_done,
                      int *total_findings, int *total_kept,
                      int *total_review)
{
    size_t len = 0;
    char *data = read_file(path, &len, opt->max_size_mb);
    if (!data)
        return;
    int nlines = 0;
    char **lines = split_lines(data, len, &nlines);
    if (!lines) {
        free(data);
        return;
    }
    LineInfo *li = (LineInfo *)calloc((size_t)nlines, sizeof(LineInfo));
    int *debug_line = (int *)calloc((size_t)nlines, sizeof(int));
    int *line_flags = (int *)calloc((size_t)nlines, sizeof(int));
    if (!li || !debug_line || !line_flags) {
        free(data);
        free(lines);
        free(li);
        free(debug_line);
        free(line_flags);
        return;
    }
    for (int i = 0; i < nlines; i++) {
        buf_init(&li[i].clean);
        buf_init(&li[i].tokens);
        buf_init(&li[i].literals);
    }
    LexState lex;
    memset(&lex, 0, sizeof(lex));
    for (int i = 0; i < nlines; i++) {
        size_t n = strlen(lines[i]);
        strip_line(lines[i], n, &lex, &li[i]);
    }
    Guard guards[SCAN_GUARD_MAX];
    memset(guards, 0, sizeof(guards));
    int depth = 0;
    for (int i = 0; i < nlines; i++) {
        char word[64];
        const char *rest = NULL;
        if (parse_directive(li[i].clean.s ? li[i].clean.s : "",
                            word, sizeof(word), &rest)) {
            if (!strcmp(word, "ifdef")) {
                int k = expr_has_debug(rest);
                guard_push(guards, &depth, k ? 1 : 0, k ? 1 : 0);
            } else if (!strcmp(word, "ifndef")) {
                int k = expr_has_debug(rest);
                guard_push(guards, &depth, k ? 2 : 0, 0);
            } else if (!strcmp(word, "if")) {
                int k = expr_has_debug(rest);
                int neg = k ? expr_negates_debug(rest) : 0;
                guard_push(guards, &depth, k ? (neg ? 2 : 1) : 0,
                            k ? (neg ? 0 : 1) : 0);
            } else if (!strcmp(word, "elif")) {
                if (depth > 0 && depth <= SCAN_GUARD_MAX) {
                    Guard *g = &guards[depth - 1];
                    if (g->kind == 1 || g->kind == 2) {
                        int k = expr_has_debug(rest);
                        int neg = k ? expr_negates_debug(rest) : 0;
                        g->branch = (k && !neg) ? 1 : 0;
                        guard_update(g);
                    }
                }
            } else if (!strcmp(word, "else")) {
                if (depth > 0 && depth <= SCAN_GUARD_MAX) {
                    Guard *g = &guards[depth - 1];
                    if (g->kind == 1)
                        g->branch = 0;
                    else if (g->kind == 2)
                        g->branch = 1;
                    guard_update(g);
                }
            } else if (!strcmp(word, "endif")) {
                if (depth > 0)
                    depth--;
            }
        }
        debug_line[i] = current_debug(guards, depth);
    }
    FileState fs;
    memset(&fs, 0, sizeof(fs));
    fs.path = path;
    fs.line_flags = line_flags;
    fs.nlines = nlines;
    fs.verbose = opt->verbose;
    scan_outputs(&fs, li, debug_line, (const char *const *)lines);
    scan_envs(&fs, li, debug_line, (const char *const *)lines);
    scan_markers(&fs, li, debug_line, (const char *const *)lines);
    scan_debug_refs(&fs, li, debug_line, (const char *const *)lines);
    if (opt->symbols && !is_header_path(path)) {
        SymTab tab;
        symtab_init(&tab);
        for (int i = 0; i < nlines; i++) {
            const char *t = skip_ws(li[i].clean.s ? li[i].clean.s : "");
            if (*t == '#') {
                char word[64];
                const char *rest = NULL;
                if (parse_directive(t, word, sizeof(word), &rest) &&
                    !strcmp(word, "define")) {
                    const char *p = skip_ws(rest);
                    char name[128];
                    size_t n = 0;
                    while (is_ident_char((unsigned char)*p) &&
                           n + 1 < sizeof(name))
                        name[n++] = *p++;
                    name[n] = '\0';
                    if (n > 0 && name[0] != '_')
                        symtab_add(&tab, name, i + 1, debug_line[i], 1, 1,
                                   0, 0);
                }
                continue;
            }
            collect_declaration(li[i].clean.s ? li[i].clean.s : "",
                                li[i].tokens.s ? li[i].tokens.s : "",
                                i + 1, debug_line[i], &tab);
        }
        symtab_build_hash(&tab);
        scan_uses(&tab, li, debug_line, nlines);
        report_symbols(&fs, &tab, (const char *const *)lines);
        symtab_free(&tab);
    }
    if (opt->verbose && fs.findings == 0)
        printf("%s: no high-confidence findings (%d kept, %d review)\n",
               path, fs.kept, fs.review);
    *total_findings += fs.findings;
    *total_kept += fs.kept;
    *total_review += fs.review;
    (*files_done)++;
    for (int i = 0; i < nlines; i++) {
        buf_free(&li[i].clean);
        buf_free(&li[i].tokens);
        buf_free(&li[i].literals);
    }
    free(li);
    free(debug_line);
    free(line_flags);
    free(lines);
    free(data);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    printf("usage: %s [options] <file-or-directory>...\n", argv0);
    printf("\n");
    printf("Finds debug output and debug-only associated declarations that are\n");
    printf("not covered by #if WC3_DEBUG (the tag is WC3_DEBUG_<TARGET>).\n");
    printf("With no path, scans runtime/ and apps/.\n");
    printf("\n");
    printf("options:\n");
    printf("  -v, --verbose       also print kept errors/status and shared symbols\n");
    printf("  --no-symbols        only scan output/env/message sites\n");
    printf("  --all               do not skip build/, third_party/, refs/\n");
    printf("  --exclude TEXT      skip paths containing TEXT (repeatable)\n");
    printf("  --max-size MB       skip files larger than MB (default 32, 0=no limit)\n");
    printf("  -h, --help          this help\n");
    printf("\nexamples:\n");
    printf("  scan_wc3debug runtime apps\n");
    printf("  scan_wc3debug --exclude headless runtime apps\n");
    printf("\nexit status: 0 = clean or review-only, 1 = high-confidence finding,\n");
    printf("             2 = usage/IO error\n");
}
int main(int argc, char **argv)
{
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.symbols = 1;
    opt.max_size_mb = 32;
    StrList paths;
    memset(&paths, 0, sizeof(paths));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            opt.verbose = 1;
        } else if (!strcmp(argv[i], "--no-symbols")) {
            opt.symbols = 0;
        } else if (!strcmp(argv[i], "--all")) {
            opt.all = 1;
        } else if (!strcmp(argv[i], "--exclude")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "scan_wc3debug: --exclude needs a value\n");
                return 2;
            }
            opt.excludes = (char **)realloc(
                opt.excludes, (size_t)(opt.n_excludes + 1) * sizeof(char *));
            if (!opt.excludes) {
                fprintf(stderr, "scan_wc3debug: out of memory\n");
                return 2;
            }
            opt.excludes[opt.n_excludes++] = argv[++i];
        } else if (!strcmp(argv[i], "--max-size")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "scan_wc3debug: --max-size needs a value\n");
                return 2;
            }
            opt.max_size_mb = atoi(argv[++i]);
            if (opt.max_size_mb < 0)
                opt.max_size_mb = 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "scan_wc3debug: unknown option %s\n", argv[i]);
            return 2;
        } else {
            strlist_add(&paths, argv[i]);
        }
    }
    if (paths.n == 0) {
        /* Default to the product-owned source roots.  The old tree's "." was
         * too broad: it also scanned tool sources and host-only probes. */
        strlist_add(&paths, "runtime");
        strlist_add(&paths, "apps");
    }
    StrList files;
    memset(&files, 0, sizeof(files));
    for (int i = 0; i < paths.n; i++)
        gather_path(paths.v[i], &files, &opt);

    if (files.n == 0) {
        fprintf(stderr, "scan_wc3debug: no source files found\n");
        return 2;
    }
    qsort(files.v, (size_t)files.n, sizeof(char *), strlist_cmp);

    int files_done = 0;
    int total_findings = 0;
    int total_kept = 0;
    int total_review = 0;

    for (int i = 0; i < files.n; i++) {
        scan_file(files.v[i], &opt, &files_done, &total_findings,
                  &total_kept, &total_review);
    }
    printf("\nScanned %d source file(s).\n", files_done);
    printf("Findings: %d (review sites: %d)\n", total_findings,
           total_review);
    printf("Kept as error/status: %d output site(s)\n", total_kept);
    printf("Legend: unwrapped-debug-* / associated-debug-only / "
           "debug-symbol-outside are findings.\n");

    for (int i = 0; i < files.n; i++)
        free(files.v[i]);
    free(files.v);
    for (int i = 0; i < paths.n; i++)
        free(paths.v[i]);
    free(paths.v);
    free(opt.excludes);
    return total_findings > 0 ? 1 : 0;
}
