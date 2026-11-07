#include "overlay_profile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *err, size_t cap, const char *message) {
    if (!err || cap == 0) return;
    snprintf(err, cap, "%s", message);
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

static const char *skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return p;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += (p + 1 < end) ? 2 : 1;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return end;
}

/* Find a key in a flat object region.  It deliberately does not descend into
 * nested objects; callers pass the exact object body for element fields. */
static const char *find_key(const char *start, const char *end,
                            const char *key, const char **value) {
    size_t key_len = strlen(key);
    const char *p = start;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) break;
        if (*p == '"') {
            const char *ks = p + 1;
            const char *ke = skip_string(p, end);
            if (ke <= ks || *(ke - 1) != '"') return NULL;
            size_t n = (size_t)((ke - 1) - ks);
            if (n == key_len && memcmp(ks, key, n) == 0) {
                const char *q = skip_ws(ke, end);
                if (q < end && *q == ':') {
                    *value = skip_ws(q + 1, end);
                    return *value;
                }
            }
            p = ke;
        } else {
            p++;
        }
    }
    return NULL;
}

/* Top-level object keys only; ignores keys nested in element objects/arrays.
 * The scan starts at the opening brace of the profile object, so top-level
 * members are at depth 1. */
static const char *find_top_level_key(const char *start, const char *end,
                                      const char *key, const char **value) {
    size_t key_len = strlen(key);
    int depth = 0;
    const char *p = start;
    while (p < end) {
        if (*p == '"') {
            const char *ks = p + 1;
            const char *ke = skip_string(p, end);
            if (ke <= ks || *(ke - 1) != '"') return NULL;
            if (depth == 1) {
                size_t n = (size_t)((ke - 1) - ks);
                if (n == key_len && memcmp(ks, key, n) == 0) {
                    const char *q = skip_ws(ke, end);
                    if (q < end && *q == ':') {
                        *value = skip_ws(q + 1, end);
                        return *value;
                    }
                }
            }
            p = ke;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (depth > 0) depth--;
        }
        p++;
    }
    return NULL;
}

static const char *matching_brace(const char *p, const char *end,
                                  char open, char close) {
    if (p >= end || *p != open) return NULL;
    int depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skip_string(p, end);
            continue;
        }
        if (*p == open) depth++;
        else if (*p == close) {
            depth--;
            if (depth == 0) return p;
        }
        p++;
    }
    return NULL;
}

static int parse_string_value(const char *p, const char *end,
                              char *out, size_t out_cap) {
    p = skip_ws(p, end);
    if (p >= end || *p != '"') return 0;
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            c = *p++;
            switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'u': {
                    /* The overlay only needs ASCII labels.  Keep the low byte
                     * of the four hex digits, which is sufficient for the
                     * Latin-1 range and avoids a UTF-16 decoder. */
                    unsigned value = 0;
                    for (int i = 0; i < 4 && p < end; i++) {
                        int digit = -1;
                        char h = *p++;
                        if (h >= '0' && h <= '9') digit = h - '0';
                        else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
                        if (digit < 0) break;
                        value = (value << 4) | (unsigned)digit;
                    }
                    c = (char)(value & 0xFFu);
                    break;
                }
                default: break;
            }
        }
        if (o + 1 < out_cap) out[o++] = c;
    }
    if (out_cap > 0) out[o] = '\0';
    return 1;
}

static int parse_number_value(const char *p, const char *end, double *out) {
    p = skip_ws(p, end);
    if (p >= end) return 0;
    char *q = NULL;
    double value = strtod(p, &q);
    if (!q || q == p || q > end) return 0;
    *out = value;
    return 1;
}

static void number_field(const char *body, const char *body_end,
                         const char *key, double *out) {
    const char *value = NULL;
    if (find_key(body, body_end, key, &value))
        parse_number_value(value, body_end, out);
}

static void string_field(const char *body, const char *body_end,
                         const char *key, char *out, size_t out_cap) {
    const char *value = NULL;
    if (find_key(body, body_end, key, &value))
        parse_string_value(value, body_end, out, out_cap);
}

static void first_binding_string(const char *body, const char *body_end,
                                 char *out, size_t out_cap) {
    out[0] = '\0';
    const char *value = NULL;
    if (!find_key(body, body_end, "bindings", &value)) return;

    const char *p = skip_ws(value, body_end);
    if (p >= body_end || *p != '[') return;
    p = skip_ws(p + 1, body_end);
    while (p < body_end && *p == '[') {
        p = skip_ws(p + 1, body_end);
        if (p < body_end && *p == '"') {
            parse_string_value(p, body_end, out, out_cap);
            return;
        }
        p = skip_ws(p + 1, body_end);
        if (p < body_end && *p == ']') p++;
        p = skip_ws(p, body_end);
        if (p < body_end && *p == ',') p = skip_ws(p + 1, body_end);
        else break;
    }
}

static void copy_label(char *out, size_t out_cap, const char *text) {
    if (!out || out_cap == 0) return;
    size_t i = 0;
    while (text && text[i] != '\0' && i + 1 < out_cap) {
        out[i] = text[i];
        i++;
    }
    out[i] = '\0';
}

static void fallback_label(const char *binding, char *out, size_t out_cap) {
    out[0] = '\0';
    if (!binding || binding[0] == '\0') return;

    if (strncmp(binding, "KEY_", 4) == 0) {
        const char *name = binding + 4;
        if (name[0] != '\0' && name[1] == '\0' && name[0] >= '0' &&
            name[0] <= '9') {
            snprintf(out, out_cap, "%c", name[0]);
            return;
        }
        if (strcmp(name, "TAB") == 0) { copy_label(out, out_cap, "TAB"); return; }
        if (strcmp(name, "ESC") == 0) { copy_label(out, out_cap, "ESC"); return; }
        if (strcmp(name, "ENTER") == 0) { copy_label(out, out_cap, "ENTER"); return; }
        if (strncmp(name, "F", 1) == 0 && isdigit((unsigned char)name[1])) {
            copy_label(out, out_cap, name);
            return;
        }
        if (strcmp(name, "CTRL_L") == 0) {
            copy_label(out, out_cap, "L CTRL");
            return;
        }
        if (strcmp(name, "CTRL_R") == 0) {
            copy_label(out, out_cap, "R CTRL");
            return;
        }
        if (strcmp(name, "SHIFT_L") == 0) {
            copy_label(out, out_cap, "L SHIFT");
            return;
        }
        if (strcmp(name, "SHIFT_R") == 0) {
            copy_label(out, out_cap, "R SHIFT");
            return;
        }
    } else if (strncmp(binding, "MOD_", 4) == 0) {
        copy_label(out, out_cap, binding + 4);
        return;
    } else if (strncmp(binding, "MOUSE_", 6) == 0) {
        copy_label(out, out_cap, "MOUSE");
        return;
    } else if (strncmp(binding, "GAMEPAD_", 8) == 0) {
        copy_label(out, out_cap, "PAD");
        return;
    }

    const char *start = binding;
    if (strncmp(start, "KEY_", 4) == 0) start += 4;
    copy_label(out, out_cap, start);
    for (char *p = out; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

static int read_file(const char *path, char **out_data, size_t *out_size,
                     char *err, size_t err_cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_error(err, err_cap, "cannot open profile");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_error(err, err_cap, "cannot seek profile");
        return -1;
    }
    long length = ftell(f);
    if (length < 0) {
        fclose(f);
        set_error(err, err_cap, "cannot size profile");
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_error(err, err_cap, "cannot rewind profile");
        return -1;
    }
    char *data = (char *)malloc((size_t)length + 1u);
    if (!data) {
        fclose(f);
        set_error(err, err_cap, "out of memory");
        return -1;
    }
    size_t got = fread(data, 1, (size_t)length, f);
    fclose(f);
    if (got != (size_t)length) {
        free(data);
        set_error(err, err_cap, "short profile read");
        return -1;
    }
    data[got] = '\0';
    *out_data = data;
    *out_size = got;
    return 0;
}

static int parse_icp_data(const char *data, size_t size, Wc3OverlayProfile *out,
                          char *err, size_t err_cap) {
    if (!data || !out) {
        set_error(err, err_cap, "invalid argument");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    overlay_style_defaults(&out->style);

    const char *begin = data;
    const char *end = data + size;

    double number = 0.0;
    const char *value = NULL;
    if (find_top_level_key(begin, end, "fillAlphaInactive", &value))
        parse_number_value(value, end, &number);
    out->style.fill_alpha_inactive = (int)(number + 0.5);
    if (out->style.fill_alpha_inactive < 0)
        out->style.fill_alpha_inactive = 0;
    if (out->style.fill_alpha_inactive > 255)
        out->style.fill_alpha_inactive = 255;

    number = 0.8;
    if (find_top_level_key(begin, end, "cornerRadius", &value))
        parse_number_value(value, end, &number);
    if (number > 0.0) out->style.corner_radius = (float)number;

    /*
     * ElementRenderer hardcodes 0.15 for strokeWidthMultiplier(); the .icp
     * strokeWidth field is not used by the reference renderer.  Keep the
     * rendered value at 0.15 so the detached overlay matches what Winlator
     * actually draws.
     */
    out->style.stroke_width_multiplier = 0.15f;

    const char *elements = NULL;
    if (!find_top_level_key(begin, end, "elements", &elements)) {
        set_error(err, err_cap, "profile has no elements");
        return -1;
    }
    elements = skip_ws(elements, end);
    if (elements >= end || *elements != '[') {
        set_error(err, err_cap, "elements is not an array");
        return -1;
    }

    const char *p = skip_ws(elements + 1, end);
    while (p < end && *p != ']') {
        if (*p != '{') break;
        const char *object_end = matching_brace(p, end, '{', '}');
        if (!object_end) {
            set_error(err, err_cap, "unterminated element object");
            return -1;
        }
        if (out->element_count >= WC3_OVERLAY_MAX_ELEMENTS) {
            set_error(err, err_cap, "too many elements");
            return -1;
        }

        const char *body = p + 1;
        const char *body_end = object_end;

        Wc3OverlayElement element;
        memset(&element, 0, sizeof(element));
        element.type = WC3_OVERLAY_ELEMENT_BUTTON;
        element.shape = WC3_OVERLAY_SHAPE_RECT;
        element.scale = 1.0f;
        element.width_units = 8.0f;
        element.height_units = 4.0f;
        element.corner_radius = -1.0f;
        element.dpad_corner_radius = -1.0f;
        element.alpha_fill = 255;
        element.alpha_glow = 255;
        element.alpha_stroke = 255;
        element.alpha_outer = 255;

        char type[32] = {0};
        string_field(body, body_end, "type", type, sizeof(type));
        if (strcmp(type, "D_PAD") == 0) {
            element.type = WC3_OVERLAY_ELEMENT_DPAD;
            element.visual_layers = WC3_OVERLAY_LAYER_FILL |
                                    WC3_OVERLAY_LAYER_STROKE_TEXT;
        } else {
            char shape[32] = {0};
            string_field(body, body_end, "shape", shape, sizeof(shape));
            if (strcmp(shape, "CIRCLE") == 0)
                element.shape = WC3_OVERLAY_SHAPE_CIRCLE;
        }

        number = 0.0;
        number_field(body, body_end, "x", &number);
        element.nx = (float)number;
        number = 0.0;
        number_field(body, body_end, "y", &number);
        element.ny = (float)number;
        number = 8.0;
        number_field(body, body_end, "elementWidth", &number);
        if (number > 0.0) element.width_units = (float)number;
        number = 4.0;
        number_field(body, body_end, "elementHeight", &number);
        if (number > 0.0) element.height_units = (float)number;
        number = 1.0;
        number_field(body, body_end, "scale", &number);
        if (number > 0.0) element.scale = (float)number;
        number = -1.0;
        number_field(body, body_end, "cornerRadius", &number);
        element.corner_radius = (float)number;
        number = -1.0;
        number_field(body, body_end, "dpadCornerRadius", &number);
        element.dpad_corner_radius = (float)number;

        const char *opacity_value = NULL;
        if (find_key(body, body_end, "opacity", &opacity_value)) {
            double opacity = 0.0;
            if (parse_number_value(opacity_value, body_end, &opacity)) {
                element.has_opacity = true;
                element.opacity = (float)opacity;
            }
        }

        string_field(body, body_end, "text", element.text,
                     sizeof(element.text));

        char binding[64] = {0};
        first_binding_string(body, body_end, binding, sizeof(binding));
        if (element.text[0] == '\0' && element.type != WC3_OVERLAY_ELEMENT_DPAD)
            fallback_label(binding, element.text, sizeof(element.text));

        out->elements[out->element_count++] = element;

        p = skip_ws(object_end + 1, end);
        if (p < end && *p == ',') p = skip_ws(p + 1, end);
        else if (p < end && *p != ']') {
            set_error(err, err_cap, "malformed elements array");
            return -1;
        }
    }

    if (out->element_count == 0) {
        set_error(err, err_cap, "profile has no elements");
        return -1;
    }
    return 0;
}

int overlay_profile_load_icp_text(const char *data, size_t size,
                                      Wc3OverlayProfile *out, char *err,
                                      size_t err_cap) {
    return parse_icp_data(data, size, out, err, err_cap);
}

int overlay_profile_load_icp(const char *path, Wc3OverlayProfile *out,
                                 char *err, size_t err_cap) {
    if (!path || !out) {
        set_error(err, err_cap, "invalid argument");
        return -1;
    }
    char *data = NULL;
    size_t size = 0;
    if (read_file(path, &data, &size, err, err_cap) != 0) return -1;
    int result = parse_icp_data(data, size, out, err, err_cap);
    free(data);
    return result;
}

void overlay_apply_profile(Wc3Overlay *overlay,
                               const Wc3OverlayProfile *profile,
                               int width, int height) {
    if (!overlay || !profile) return;
    overlay_init(overlay, width, height, &profile->style);
    for (int i = 0; i < profile->element_count; i++)
        overlay_add_element(overlay, &profile->elements[i]);
    overlay_commit(overlay);
}
