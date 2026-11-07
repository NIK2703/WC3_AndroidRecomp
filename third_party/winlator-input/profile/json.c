#define _GNU_SOURCE
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

// ---- Internal value types ----

typedef struct {
    char** keys;
    TpJsonValue** values;
    int count;
    int capacity;
} ObjectMembers;

typedef struct {
    TpJsonValue** items;
    int count;
    int capacity;
} ArrayItems;

struct TpJsonValue {
    TpJsonType type;
    union {
        int bool_val;
        double num_val;
        char* str_val;
        ArrayItems arr;
        ObjectMembers obj;
    } data;
};

// ---- Parser context ----

typedef struct {
    const char* p;
    const char* start;
    const char* error;
    int depth;
} ParseCtx;

#define TP_JSON_MAX_DEPTH 64

// ---- Forward declarations ----

static TpJsonValue* parse_value(ParseCtx* ctx);
static void skip_ws(ParseCtx* ctx);

// ---- Helpers ----

static void skip_ws(ParseCtx* ctx) {
    while (*ctx->p && (unsigned char)*ctx->p <= 32) ctx->p++;
}

static int peek(ParseCtx* ctx) {
    skip_ws(ctx);
    return (unsigned char)*ctx->p;
}

static int advance(ParseCtx* ctx) {
    int c = (unsigned char)*ctx->p;
    if (c) ctx->p++;
    return c;
}

static int expect(ParseCtx* ctx, int c) {
    skip_ws(ctx);
    if ((unsigned char)*ctx->p != c) {
        ctx->error = "unexpected character";
        return 0;
    }
    ctx->p++;
    return 1;
}

static TpJsonValue* make_val(TpJsonType type) {
    TpJsonValue* v = (TpJsonValue*)calloc(1, sizeof(TpJsonValue));
    if (v) v->type = type;
    return v;
}

// ---- Array dynamic ----

static void arr_push(ArrayItems* a, TpJsonValue* item) {
    if (a->count >= a->capacity) {
        int newcap = a->capacity ? a->capacity * 2 : 8;
        TpJsonValue** tmp = (TpJsonValue**)realloc(a->items, (size_t)newcap * sizeof(TpJsonValue*));
        if (!tmp) return;
        a->items = tmp;
        a->capacity = newcap;
    }
    a->items[a->count++] = item;
}

// ---- Object dynamic ----

static void obj_push(ObjectMembers* o, const char* key, TpJsonValue* val) {
    if (o->count >= o->capacity) {
        int newcap = o->capacity ? o->capacity * 2 : 8;
        char** ktmp = (char**)realloc(o->keys, (size_t)newcap * sizeof(char*));
        TpJsonValue** vtmp = (TpJsonValue**)realloc(o->values, (size_t)newcap * sizeof(TpJsonValue*));
        if (!ktmp || !vtmp) return;
        o->keys = ktmp;
        o->values = vtmp;
        o->capacity = newcap;
    }
    o->keys[o->count] = key ? strdup(key) : NULL;
    o->values[o->count] = val;
    o->count++;
}

// ---- String parser ----

static char* parse_string_raw(ParseCtx* ctx) {
    if (*ctx->p != '"') { ctx->error = "expected '\"'"; return NULL; }
    ctx->p++; // skip opening quote
    size_t cap = 64, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { ctx->error = "out of memory"; return NULL; }
    while (*ctx->p && *ctx->p != '"') {
        if (len + 2 >= cap) {
            cap *= 2;
            char* tmp = (char*)realloc(buf, cap);
            if (!tmp) { free(buf); ctx->error = "out of memory"; return NULL; }
            buf = tmp;
        }
        if (*ctx->p == '\\') {
            ctx->p++;
            switch (*ctx->p) {
                case '"':  buf[len++] = '"'; break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'u': {
                    unsigned int codepoint = 0;
                    for (int i = 0; i < 4; i++) {
                        ctx->p++;
                        int c = (unsigned char)*ctx->p;
                        if (c >= '0' && c <= '9') codepoint = (codepoint << 4) + (c - '0');
                        else if (c >= 'a' && c <= 'f') codepoint = (codepoint << 4) + (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') codepoint = (codepoint << 4) + (c - 'A' + 10);
                        else { free(buf); ctx->error = "invalid unicode escape"; return NULL; }
                    }
                    if (codepoint < 0x80) {
                        buf[len++] = (char)codepoint;
                    } else if (codepoint < 0x800) {
                        buf[len++] = (char)(0xC0 | (codepoint >> 6));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        buf[len++] = (char)(0xE0 | (codepoint >> 12));
                        buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                    break;
                }
                default:
                    free(buf);
                    ctx->error = "invalid escape";
                    return NULL;
            }
            if (*ctx->p) ctx->p++;
        } else {
            buf[len++] = *ctx->p;
            ctx->p++;
        }
    }
    if (*ctx->p != '"') { free(buf); ctx->error = "unterminated string"; return NULL; }
    ctx->p++; // skip closing quote
    buf[len] = '\0';
    return buf;
}

static TpJsonValue* parse_string(ParseCtx* ctx) {
    char* s = parse_string_raw(ctx);
    if (!s) return NULL;
    TpJsonValue* v = make_val(TP_JSON_STRING);
    if (!v) { free(s); return NULL; }
    v->data.str_val = s;
    return v;
}

// ---- Number parser ----

static TpJsonValue* parse_number(ParseCtx* ctx) {
    const char* start = ctx->p;
    if (*ctx->p == '-') ctx->p++;
    while (*ctx->p >= '0' && *ctx->p <= '9') ctx->p++;
    if (*ctx->p == '.') {
        ctx->p++;
        while (*ctx->p >= '0' && *ctx->p <= '9') ctx->p++;
    }
    if (*ctx->p == 'e' || *ctx->p == 'E') {
        ctx->p++;
        if (*ctx->p == '+' || *ctx->p == '-') ctx->p++;
        while (*ctx->p >= '0' && *ctx->p <= '9') ctx->p++;
    }
    size_t len = (size_t)(ctx->p - start);
    char buf[64];
    if (len >= sizeof(buf)) { ctx->error = "number too long"; return NULL; }
    memcpy(buf, start, len);
    buf[len] = '\0';
    TpJsonValue* v = make_val(TP_JSON_NUMBER);
    if (!v) return NULL;
    v->data.num_val = strtod(buf, NULL);
    return v;
}

// ---- Array parser ----

static TpJsonValue* parse_array(ParseCtx* ctx) {
    TpJsonValue* v = make_val(TP_JSON_ARRAY);
    if (!v) return NULL;
    advance(ctx); // skip '['
    if (++ctx->depth > TP_JSON_MAX_DEPTH) { ctx->error = "max depth exceeded"; tp_json_free(v); return NULL; }
    if (peek(ctx) == ']') { advance(ctx); ctx->depth--; return v; }
    while (1) {
        TpJsonValue* item = parse_value(ctx);
        if (!item) { tp_json_free(v); return NULL; }
        arr_push(&v->data.arr, item);
        if (peek(ctx) == ',') { advance(ctx); continue; }
        break;
    }
    if (!expect(ctx, ']')) { tp_json_free(v); return NULL; }
    ctx->depth--;
    return v;
}

// ---- Object parser ----

static TpJsonValue* parse_object(ParseCtx* ctx) {
    TpJsonValue* v = make_val(TP_JSON_OBJECT);
    if (!v) return NULL;
    advance(ctx); // skip '{'
    if (++ctx->depth > TP_JSON_MAX_DEPTH) { ctx->error = "max depth exceeded"; tp_json_free(v); return NULL; }
    if (peek(ctx) == '}') { advance(ctx); ctx->depth--; return v; }
    while (1) {
        skip_ws(ctx);
        if (*ctx->p != '"') { ctx->error = "expected string key"; tp_json_free(v); return NULL; }
        char* key = parse_string_raw(ctx);
        if (!key) { tp_json_free(v); return NULL; }
        if (!expect(ctx, ':')) { free(key); tp_json_free(v); return NULL; }
        TpJsonValue* val = parse_value(ctx);
        if (!val) { free(key); tp_json_free(v); return NULL; }
        obj_push(&v->data.obj, key, val);
        free(key);
        if (peek(ctx) == ',') { advance(ctx); continue; }
        break;
    }
    if (!expect(ctx, '}')) { tp_json_free(v); return NULL; }
    ctx->depth--;
    return v;
}

// ---- Value dispatcher ----

static TpJsonValue* parse_value(ParseCtx* ctx) {
    skip_ws(ctx);
    switch (*ctx->p) {
        case '{': return parse_object(ctx);
        case '[': return parse_array(ctx);
        case '"': return parse_string(ctx);
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number(ctx);
        case 't':
            if (strncmp(ctx->p, "true", 4) == 0) {
                ctx->p += 4;
                TpJsonValue* v = make_val(TP_JSON_BOOL);
                if (v) v->data.bool_val = 1;
                return v;
            }
            break;
        case 'f':
            if (strncmp(ctx->p, "false", 5) == 0) {
                ctx->p += 5;
                TpJsonValue* v = make_val(TP_JSON_BOOL);
                if (v) v->data.bool_val = 0;
                return v;
            }
            break;
        case 'n':
            if (strncmp(ctx->p, "null", 4) == 0) {
                ctx->p += 4;
                return make_val(TP_JSON_NULL);
            }
            break;
    }
    ctx->error = "unexpected token";
    return NULL;
}

// ---- Public API ----

TpJsonValue* tp_json_parse(const char* json) {
    if (!json) return NULL;
    ParseCtx ctx;
    ctx.p = json;
    ctx.start = json;
    ctx.error = NULL;
    ctx.depth = 0;
    TpJsonValue* val = parse_value(&ctx);
    skip_ws(&ctx);
    if (ctx.error || *ctx.p) {
        if (val) tp_json_free(val);
        return NULL;
    }
    return val;
}

void tp_json_free(TpJsonValue* val) {
    if (!val) return;
    switch (val->type) {
        case TP_JSON_STRING:
            free(val->data.str_val);
            break;
        case TP_JSON_ARRAY:
            for (int i = 0; i < val->data.arr.count; i++)
                tp_json_free(val->data.arr.items[i]);
            free(val->data.arr.items);
            break;
        case TP_JSON_OBJECT:
            for (int i = 0; i < val->data.obj.count; i++) {
                free(val->data.obj.keys[i]);
                tp_json_free(val->data.obj.values[i]);
            }
            free(val->data.obj.keys);
            free(val->data.obj.values);
            break;
        default:
            break;
    }
    free(val);
}

TpJsonType tp_json_get_type(const TpJsonValue* val) {
    return val ? val->type : TP_JSON_NULL;
}

bool tp_json_is_null(const TpJsonValue* val) { return !val || val->type == TP_JSON_NULL; }
bool tp_json_is_bool(const TpJsonValue* val) { return val && val->type == TP_JSON_BOOL; }
bool tp_json_is_number(const TpJsonValue* val) { return val && val->type == TP_JSON_NUMBER; }
bool tp_json_is_string(const TpJsonValue* val) { return val && val->type == TP_JSON_STRING; }
bool tp_json_is_array(const TpJsonValue* val) { return val && val->type == TP_JSON_ARRAY; }
bool tp_json_is_object(const TpJsonValue* val) { return val && val->type == TP_JSON_OBJECT; }

bool tp_json_get_bool(const TpJsonValue* val) {
    return val && val->type == TP_JSON_BOOL && val->data.bool_val;
}

double tp_json_get_number(const TpJsonValue* val) {
    return (val && val->type == TP_JSON_NUMBER) ? val->data.num_val : 0.0;
}

int tp_json_get_int(const TpJsonValue* val) {
    return (int)tp_json_get_number(val);
}

const char* tp_json_get_string(const TpJsonValue* val) {
    return (val && val->type == TP_JSON_STRING) ? val->data.str_val : NULL;
}

int tp_json_arr_len(const TpJsonValue* arr) {
    return (arr && arr->type == TP_JSON_ARRAY) ? arr->data.arr.count : 0;
}

TpJsonValue* tp_json_arr_get(const TpJsonValue* arr, int index) {
    if (!arr || arr->type != TP_JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->data.arr.count) return NULL;
    return arr->data.arr.items[index];
}

int tp_json_get_array_count(const TpJsonValue* arr) {
    return tp_json_arr_len(arr);
}

TpJsonValue* tp_json_get_array_item(const TpJsonValue* arr, int index) {
    return tp_json_arr_get(arr, index);
}

TpJsonValue* tp_json_obj_get(const TpJsonValue* obj, const char* key) {
    if (!obj || obj->type != TP_JSON_OBJECT || !key) return NULL;
    for (int i = 0; i < obj->data.obj.count; i++) {
        if (strcmp(obj->data.obj.keys[i], key) == 0)
            return obj->data.obj.values[i];
    }
    return NULL;
}

TpJsonValue* tp_json_get_object_field(const TpJsonValue* obj, const char* key) {
    return tp_json_obj_get(obj, key);
}

TpJsonValue* tp_json_get_nested(const TpJsonValue* val, const char* path) {
    if (!val || !path) return NULL;
    char buf[256];
    const char* p = path;
    const TpJsonValue* cur = val;
    while (*p && cur) {
        const char* end = strchr(p, '.');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(buf)) return NULL;
        memcpy(buf, p, len);
        buf[len] = '\0';
        cur = tp_json_obj_get(cur, buf);
        p = end ? end + 1 : p + len;
    }
    return (TpJsonValue*)cur;
}

const char* tp_json_get_string_def(const TpJsonValue* obj, const char* key, const char* def) {
    TpJsonValue* f = tp_json_obj_get(obj, key);
    const char* s = f ? tp_json_get_string(f) : NULL;
    return s ? s : (def ? def : "");
}

bool tp_json_get_bool_def(const TpJsonValue* obj, const char* key, bool def) {
    TpJsonValue* f = tp_json_obj_get(obj, key);
    return (f && f->type == TP_JSON_BOOL) ? f->data.bool_val : def;
}

int tp_json_get_int_def(const TpJsonValue* obj, const char* key, int def) {
    TpJsonValue* f = tp_json_obj_get(obj, key);
    return (f && f->type == TP_JSON_NUMBER) ? (int)f->data.num_val : def;
}

double tp_json_get_num_def(const TpJsonValue* obj, const char* key, double def) {
    TpJsonValue* f = tp_json_obj_get(obj, key);
    return (f && f->type == TP_JSON_NUMBER) ? f->data.num_val : def;
}

double tp_json_get_double_def(const TpJsonValue* obj, const char* key, double def) {
    return tp_json_get_num_def(obj, key, def);
}

// ---- Serializer (for round-trip) ----

static void serialize_indent(FILE* f, int indent, int level) {
    if (indent < 0) return;
    for (int i = 0; i < level * indent; i++) fputc(' ', f);
}

static void serialize_value(FILE* f, const TpJsonValue* val, int indent, int level) {
    if (!val) { fputs("null", f); return; }
    switch (val->type) {
        case TP_JSON_NULL:
            fputs("null", f);
            break;
        case TP_JSON_BOOL:
            fputs(val->data.bool_val ? "true" : "false", f);
            break;
        case TP_JSON_NUMBER: {
            double d = val->data.num_val;
            if (d == floor(d) && !isinf(d) && !isnan(d))
                fprintf(f, "%d", (int)d);
            else
                fprintf(f, "%g", d);
            break;
        }
        case TP_JSON_STRING:
            fputc('"', f);
            for (const char* s = val->data.str_val; *s; s++) {
                switch (*s) {
                    case '"': fputs("\\\"", f); break;
                    case '\\': fputs("\\\\", f); break;
                    case '\b': fputs("\\b", f); break;
                    case '\f': fputs("\\f", f); break;
                    case '\n': fputs("\\n", f); break;
                    case '\r': fputs("\\r", f); break;
                    case '\t': fputs("\\t", f); break;
                    default:
                        if ((unsigned char)*s < 32)
                            fprintf(f, "\\u%04x", (unsigned char)*s);
                        else
                            fputc(*s, f);
                        break;
                }
            }
            fputc('"', f);
            break;
        case TP_JSON_ARRAY:
            fputc('[', f);
            for (int i = 0; i < val->data.arr.count; i++) {
                if (i > 0) fputc(',', f);
                if (indent >= 0) fputc(' ', f);
                serialize_value(f, val->data.arr.items[i], indent, level + 1);
            }
            fputc(']', f);
            break;
        case TP_JSON_OBJECT:
            fputs("{\n", f);
            for (int i = 0; i < val->data.obj.count; i++) {
                if (i > 0) fputs(",\n", f);
                serialize_indent(f, indent, level + 1);
                fprintf(f, "\"%s\": ", val->data.obj.keys[i]);
                serialize_value(f, val->data.obj.values[i], indent, level + 1);
            }
            fputc('\n', f);
            serialize_indent(f, indent, level);
            fputc('}', f);
            break;
    }
}

char* tp_json_serialize(const TpJsonValue* val, int indent) {
    if (!val) return NULL;
    char* buf = NULL;
    size_t size = 0;
    FILE* f = open_memstream(&buf, &size);
    if (!f) return NULL;
    serialize_value(f, val, indent, 0);
    fclose(f);
    return buf;
}
