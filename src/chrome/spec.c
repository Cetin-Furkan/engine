#include "chrome/spec.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum : uint32_t {
    k_spec_max_bytes = 16384,
};

static ChromeSpec s_spec;
static bool       s_loaded;
static char       s_err[320];

typedef struct CursorShapeName {
    const char *name;
    uint32_t    value;
} CursorShapeName;

static const CursorShapeName k_shapes[] = {
    { "default", 1 },       { "context_menu", 2 },  { "help", 3 },
    { "pointer", 4 },       { "progress", 5 },      { "wait", 6 },
    { "cell", 7 },          { "crosshair", 8 },     { "text", 9 },
    { "vertical_text", 10 },{ "alias", 11 },        { "copy", 12 },
    { "move", 13 },         { "no_drop", 14 },      { "not_allowed", 15 },
    { "grab", 16 },         { "grabbing", 17 },     { "e_resize", 18 },
    { "n_resize", 19 },     { "ne_resize", 20 },    { "nw_resize", 21 },
    { "s_resize", 22 },     { "se_resize", 23 },    { "sw_resize", 24 },
    { "w_resize", 25 },     { "ew_resize", 26 },    { "ns_resize", 27 },
    { "nesw_resize", 28 },  { "nwse_resize", 29 },  { "col_resize", 30 },
    { "row_resize", 31 },   { "all_scroll", 32 },   { "zoom_in", 33 },
    { "zoom_out", 34 },     { "dnd_ask", 35 },      { "all_resize", 36 },
};

bool chrome_cursor_shape(const char *name, uint32_t *out_shape) {
    if (name == nullptr || out_shape == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < (uint32_t)(sizeof k_shapes / sizeof k_shapes[0]); ++i) {
        if (strcmp(name, k_shapes[i].name) == 0) {
            *out_shape = k_shapes[i].value;
            return true;
        }
    }
    return false;
}

const ChromeSpec *chrome_spec(void) {
    return s_loaded ? &s_spec : nullptr;
}

static void fail_line(uint32_t line, const char *msg) {
    snprintf(s_err, sizeof s_err, "line %u: %s", line, msg);
}

static char *trim(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    if (*s == '\0') {
        return s;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return s;
}

static bool copy_token(char *dst, uint32_t cap, const char *src) {
    uint32_t n = (uint32_t)strlen(src);
    if (n == 0 || n + 1u > cap) {
        return false;
    }
    memcpy(dst, src, n + 1u);
    return true;
}

static bool parse_u32(const char *text, uint32_t *out) {
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    unsigned long v = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || v > 0xfffffffful) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *text, ChromeColor *out) {
    if (text == nullptr || text[0] != '#' || strlen(text) != 7 || out == nullptr) {
        return false;
    }
    int r1 = hex_nibble(text[1]), r0 = hex_nibble(text[2]);
    int g1 = hex_nibble(text[3]), g0 = hex_nibble(text[4]);
    int b1 = hex_nibble(text[5]), b0 = hex_nibble(text[6]);
    if (r1 < 0 || r0 < 0 || g1 < 0 || g0 < 0 || b1 < 0 || b0 < 0) {
        return false;
    }
    out->r = (uint8_t)((r1 << 4) | r0);
    out->g = (uint8_t)((g1 << 4) | g0);
    out->b = (uint8_t)((b1 << 4) | b0);
    return true;
}

static bool read_file(const char *path, char *buf, uint32_t cap, uint32_t *out_len) {
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        snprintf(s_err, sizeof s_err, "%s", strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        snprintf(s_err, sizeof s_err, "%s", strerror(errno));
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > cap - 1u) {
        fclose(f);
        snprintf(s_err, sizeof s_err, "%s is missing or larger than %u bytes", path, cap - 1u);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        snprintf(s_err, sizeof s_err, "%s", strerror(errno));
        fclose(f);
        return false;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        snprintf(s_err, sizeof s_err, "short read on %s", path);
        return false;
    }
    buf[n] = '\0';
    *out_len = (uint32_t)n;
    return true;
}

static bool exe_relative(char *out, uint32_t cap) {
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0 || (size_t)n >= sizeof exe) {
        return false;
    }
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash == nullptr) {
        return false;
    }
    *slash = '\0';
    for (uint32_t up = 0; up < 5u; ++up) {
        int wrote = snprintf(out, cap, "%s/chrome/window.chrome", exe);
        if (wrote > 0 && (uint32_t)wrote < cap && access(out, R_OK) == 0) {
            return true;
        }
        slash = strrchr(exe, '/');
        if (slash == nullptr || slash == exe) {
            break;
        }
        *slash = '\0';
    }
    return false;
}

static bool resolve_path(char *out, uint32_t cap) {
    const char *env = getenv("ENGINE_CHROME");
    if (env != nullptr && env[0] != '\0') {
        if (strlen(env) + 1u > cap) {
            snprintf(s_err, sizeof s_err, "ENGINE_CHROME is too long");
            return false;
        }
        memcpy(out, env, strlen(env) + 1u);
        return true;
    }
    const char *cwd_rel = "chrome/window.chrome";
    if (access(cwd_rel, R_OK) == 0) {
        memcpy(out, cwd_rel, strlen(cwd_rel) + 1u);
        return true;
    }
    if (exe_relative(out, cap)) {
        return true;
    }
    snprintf(s_err, sizeof s_err,
             "chrome spec not found (ENGINE_CHROME, ./chrome/window.chrome, or next to the executable)");
    return false;
}

enum : uint32_t {
    SEC_GLOBAL  = 0,
    SEC_LAYER   = 1,
    SEC_CONTROL = 2,
};

static bool take_layer_key(ChromeLayerSpec *layer, bool *seen_name, bool *seen_edge,
                           bool *seen_thickness, bool *seen_span, bool *seen_color,
                           const char *key, const char *val, uint32_t line) {
    if (strcmp(key, "name") == 0) {
        if (*seen_name || !copy_token(layer->name, sizeof layer->name, val)) {
            fail_line(line, "layer name missing, repeated, or too long");
            return false;
        }
        *seen_name = true;
        return true;
    }
    if (strcmp(key, "edge") == 0) {
        if (*seen_edge || (strcmp(val, "top") != 0 && strcmp(val, "bottom") != 0)) {
            fail_line(line, "layer edge must be top or bottom, once");
            return false;
        }
        memcpy(layer->edge, val, strlen(val) + 1u);
        *seen_edge = true;
        return true;
    }
    if (strcmp(key, "thickness") == 0) {
        uint32_t v = 0;
        if (*seen_thickness || !parse_u32(val, &v) || v == 0) {
            fail_line(line, "layer thickness must be a positive integer, once");
            return false;
        }
        layer->thickness = v;
        *seen_thickness = true;
        return true;
    }
    if (strcmp(key, "span") == 0) {
        if (*seen_span || strcmp(val, "full") != 0) {
            fail_line(line, "layer span must be full, once");
            return false;
        }
        memcpy(layer->span, val, strlen(val) + 1u);
        *seen_span = true;
        return true;
    }
    if (strcmp(key, "color") == 0) {
        if (*seen_color || !parse_color(val, &layer->color)) {
            fail_line(line, "layer color must be #RRGGBB, once");
            return false;
        }
        *seen_color = true;
        return true;
    }
    fail_line(line, "unknown layer key");
    return false;
}

static bool take_control_key(ChromeControlSpec *ctl, bool *seen_name, bool *seen_layer,
                             bool *seen_action, bool *seen_align, bool *seen_span,
                             bool *seen_color, bool *seen_glyph, bool *seen_cursor,
                             const char *key, const char *val, uint32_t line) {
    if (strcmp(key, "name") == 0) {
        if (*seen_name || !copy_token(ctl->name, sizeof ctl->name, val)) {
            fail_line(line, "control name missing, repeated, or too long");
            return false;
        }
        *seen_name = true;
        return true;
    }
    if (strcmp(key, "layer") == 0) {
        if (*seen_layer || !copy_token(ctl->layer, sizeof ctl->layer, val)) {
            fail_line(line, "control layer missing, repeated, or too long");
            return false;
        }
        *seen_layer = true;
        return true;
    }
    if (strcmp(key, "action") == 0) {
        if (*seen_action || strcmp(val, "close") != 0) {
            fail_line(line, "control action must be close, once");
            return false;
        }
        memcpy(ctl->action, val, strlen(val) + 1u);
        *seen_action = true;
        return true;
    }
    if (strcmp(key, "align") == 0) {
        if (*seen_align || (strcmp(val, "start") != 0 && strcmp(val, "end") != 0)) {
            fail_line(line, "control align must be start or end, once");
            return false;
        }
        memcpy(ctl->align, val, strlen(val) + 1u);
        *seen_align = true;
        return true;
    }
    if (strcmp(key, "span") == 0) {
        uint32_t v = 0;
        if (*seen_span || !parse_u32(val, &v) || v == 0) {
            fail_line(line, "control span must be a positive integer, once");
            return false;
        }
        ctl->span = v;
        *seen_span = true;
        return true;
    }
    if (strcmp(key, "color") == 0) {
        if (*seen_color || !parse_color(val, &ctl->color)) {
            fail_line(line, "control color must be #RRGGBB, once");
            return false;
        }
        *seen_color = true;
        return true;
    }
    if (strcmp(key, "glyph") == 0) {
        if (*seen_glyph || !parse_color(val, &ctl->glyph)) {
            fail_line(line, "control glyph must be #RRGGBB, once");
            return false;
        }
        *seen_glyph = true;
        return true;
    }
    if (strcmp(key, "cursor") == 0) {
        uint32_t shape = 0;
        if (*seen_cursor || !chrome_cursor_shape(val, &shape)) {
            fail_line(line, "control cursor is repeated or not a cursor-shape name");
            return false;
        }
        ctl->cursor_shape = shape;
        ctl->has_cursor = true;
        *seen_cursor = true;
        return true;
    }
    fail_line(line, "unknown control key");
    return false;
}

static bool layer_complete(const ChromeLayerSpec *layer, bool seen_name, bool seen_edge,
                           bool seen_thickness, bool seen_span, bool seen_color, uint32_t line) {
    if (!seen_name || !seen_edge || !seen_thickness || !seen_span || !seen_color ||
        layer->name[0] == '\0') {
        fail_line(line, "layer requires name, edge, thickness, span, and color");
        return false;
    }
    return true;
}

static bool control_complete(const ChromeControlSpec *ctl, bool seen_name, bool seen_layer,
                             bool seen_action, bool seen_align, bool seen_span,
                             bool seen_color, bool seen_glyph, uint32_t line) {
    if (!seen_name || !seen_layer || !seen_action || !seen_align || !seen_span ||
        !seen_color || !seen_glyph) {
        fail_line(line, "control requires name, layer, action, align, span, color, and glyph");
        return false;
    }
    (void)ctl;
    return true;
}

static int parse_spec(char *text, ChromeSpec *spec) {
    uint32_t section = SEC_GLOBAL;
    ChromeLayerSpec layer = {0};
    ChromeControlSpec ctl = {0};
    bool ln = false, le = false, lt = false, ls = false, lc = false;
    bool cn = false, cl = false, ca = false, cal = false, cs = false;
    bool cc = false, cg = false, cu = false;
    bool got_title = false, got_app = false, got_cursor = false;
    bool got_resize = false, got_band = false;
    uint32_t line_no = 0;
    char *cursor = text;

    while (cursor != nullptr && *cursor != '\0') {
        char *nl = strchr(cursor, '\n');
        if (nl != nullptr) {
            *nl = '\0';
        }
        ++line_no;
        char *line = trim(cursor);
        if (line[0] != '\0' && line[0] != '#') {
            if (strcmp(line, "layer") == 0 || strcmp(line, "control") == 0) {
                if (section == SEC_LAYER) {
                    if (!layer_complete(&layer, ln, le, lt, ls, lc, line_no)) return -1;
                    if (spec->nlayers >= ENG_CHROME_LAYERS) {
                        fail_line(line_no, "too many layers for ENG_CHROME_LAYERS");
                        return -1;
                    }
                    spec->layers[spec->nlayers++] = layer;
                } else if (section == SEC_CONTROL) {
                    if (!control_complete(&ctl, cn, cl, ca, cal, cs, cc, cg, line_no)) return -1;
                    if (spec->ncontrols >= ENG_CHROME_CONTROLS) {
                        fail_line(line_no, "too many controls for ENG_CHROME_CONTROLS");
                        return -1;
                    }
                    spec->controls[spec->ncontrols++] = ctl;
                }
                section = (strcmp(line, "layer") == 0) ? SEC_LAYER : SEC_CONTROL;
                layer = (ChromeLayerSpec){0};
                ctl = (ChromeControlSpec){0};
                ln = le = lt = ls = lc = false;
                cn = cl = ca = cal = cs = cc = cg = cu = false;
            } else {
                char *eq = strchr(line, '=');
                if (eq == nullptr) {
                    fail_line(line_no, "expected key = value, layer, or control");
                    return -1;
                }
                *eq = '\0';
                char *key = trim(line);
                char *val = trim(eq + 1);
                if (key[0] == '\0' || val[0] == '\0') {
                    fail_line(line_no, "empty key or value");
                    return -1;
                }
                if (section == SEC_GLOBAL) {
                    if (strcmp(key, "window.title") == 0) {
                        if (got_title || !copy_token(spec->title, sizeof spec->title, val)) {
                            fail_line(line_no, "window.title missing, repeated, or too long");
                            return -1;
                        }
                        got_title = true;
                    } else if (strcmp(key, "window.app_id") == 0) {
                        if (got_app || !copy_token(spec->app_id, sizeof spec->app_id, val)) {
                            fail_line(line_no, "window.app_id missing, repeated, or too long");
                            return -1;
                        }
                        got_app = true;
                    } else if (strcmp(key, "cursor.shape") == 0) {
                        uint32_t shape = 0;
                        if (got_cursor || !chrome_cursor_shape(val, &shape)) {
                            fail_line(line_no, "cursor.shape missing, repeated, or unknown");
                            return -1;
                        }
                        spec->cursor_shape = shape;
                        got_cursor = true;
                    } else if (strcmp(key, "resize") == 0) {
                        if (got_resize || (strcmp(val, "on") != 0 && strcmp(val, "off") != 0)) {
                            fail_line(line_no, "resize must be on or off, once");
                            return -1;
                        }
                        spec->resize = strcmp(val, "on") == 0;
                        got_resize = true;
                    } else if (strcmp(key, "resize.band") == 0) {
                        uint32_t v = 0;
                        if (got_band || !parse_u32(val, &v) || v == 0) {
                            fail_line(line_no, "resize.band must be a positive integer, once");
                            return -1;
                        }
                        spec->resize_band = v;
                        got_band = true;
                    } else {
                        fail_line(line_no, "unknown key");
                        return -1;
                    }
                } else if (section == SEC_LAYER) {
                    if (!take_layer_key(&layer, &ln, &le, &lt, &ls, &lc, key, val, line_no)) {
                        return -1;
                    }
                } else if (!take_control_key(&ctl, &cn, &cl, &ca, &cal, &cs, &cc, &cg, &cu,
                                             key, val, line_no)) {
                    return -1;
                }
            }
        }
        cursor = (nl != nullptr) ? nl + 1 : nullptr;
    }

    if (section == SEC_LAYER) {
        if (!layer_complete(&layer, ln, le, lt, ls, lc, line_no)) return -1;
        if (spec->nlayers >= ENG_CHROME_LAYERS) {
            fail_line(line_no, "too many layers for ENG_CHROME_LAYERS");
            return -1;
        }
        spec->layers[spec->nlayers++] = layer;
    } else if (section == SEC_CONTROL) {
        if (!control_complete(&ctl, cn, cl, ca, cal, cs, cc, cg, line_no)) return -1;
        if (spec->ncontrols >= ENG_CHROME_CONTROLS) {
            fail_line(line_no, "too many controls for ENG_CHROME_CONTROLS");
            return -1;
        }
        spec->controls[spec->ncontrols++] = ctl;
    }

    if (!got_title || !got_app || !got_cursor || !got_resize || !got_band) {
        snprintf(s_err, sizeof s_err,
                 "spec requires window.title, window.app_id, cursor.shape, resize, and resize.band");
        return -1;
    }

    for (uint32_t i = 0; i < spec->nlayers; ++i) {
        for (uint32_t j = i + 1u; j < spec->nlayers; ++j) {
            if (strcmp(spec->layers[i].name, spec->layers[j].name) == 0) {
                snprintf(s_err, sizeof s_err, "duplicate layer name '%s'", spec->layers[i].name);
                return -1;
            }
        }
    }
    for (uint32_t i = 0; i < spec->ncontrols; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < spec->nlayers; ++j) {
            if (strcmp(spec->controls[i].layer, spec->layers[j].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(s_err, sizeof s_err, "control '%s' names unknown layer '%s'",
                     spec->controls[i].name, spec->controls[i].layer);
            return -1;
        }
    }
    return 0;
}

int chrome_load(void) {
    s_loaded = false;
    s_spec = (ChromeSpec){0};
    s_err[0] = '\0';
    char path[256];
    if (!resolve_path(path, sizeof path)) {
        fprintf(stderr, "[Chrome Error] %s\n", s_err[0] != '\0' ? s_err : "spec path");
        return -1;
    }
    char text[k_spec_max_bytes];
    uint32_t len = 0;
    if (!read_file(path, text, sizeof text, &len)) {
        fprintf(stderr, "[Chrome Error] Cannot read '%s'%s%s\n",
                path, s_err[0] != '\0' ? ": " : "", s_err);
        return -1;
    }
    if (parse_spec(text, &s_spec) != 0) {
        fprintf(stderr, "[Chrome Error] %s (%s)\n", s_err, path);
        return -1;
    }
    memcpy(s_spec.path, path, strlen(path) + 1u);
    s_loaded = true;
    printf("[Chrome] Spec %s | layers=%u controls=%u resize=%s band=%u cursor=%u\n",
           s_spec.path,
           s_spec.nlayers,
           s_spec.ncontrols,
           s_spec.resize ? "on" : "off",
           s_spec.resize_band,
           s_spec.cursor_shape);
    return 0;
}