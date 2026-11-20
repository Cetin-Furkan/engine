#include "gltf/json.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum : size_t {
    k_json_block_bytes = 65536u,
    k_json_max_depth   = 128u,
};

struct JsonArenaBlock {
    JsonArenaBlock *next;
    size_t          used;
    size_t          cap;
    alignas(16) uint8_t data[];
};

typedef struct JsonParser {
    const char *src;
    size_t      len;
    size_t      pos;
    uint32_t    depth;
    JsonDoc    *doc;
    bool        oom;
} JsonParser;

static void *arena_alloc(JsonDoc *doc, size_t bytes, size_t align) {
    if (align < 8u) {
        align = 8u;
    }
    JsonArenaBlock *blk = doc->blocks;
    if (blk != nullptr) {
        size_t off = (blk->used + align - 1u) & ~(align - 1u);
        if (off + bytes <= blk->cap) {
            blk->used = off + bytes;
            return blk->data + off;
        }
    }
    size_t cap = bytes + align > k_json_block_bytes ? (bytes + align) : k_json_block_bytes;
    JsonArenaBlock *nb = malloc(sizeof(JsonArenaBlock) + cap);
    if (nb == nullptr) {
        return nullptr;
    }
    nb->next    = doc->blocks;
    nb->used    = bytes;
    nb->cap     = cap;
    doc->blocks = nb;
    return nb->data;
}

static void skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else {
            break;
        }
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void append_utf8(char *dst, size_t *len, uint32_t cp) {
    if (cp <= 0x7Fu) {
        dst[(*len)++] = (char)cp;
    } else if (cp <= 0x7FFu) {
        dst[(*len)++] = (char)(0xC0u | (cp >> 6u));
        dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        dst[(*len)++] = (char)(0xE0u | (cp >> 12u));
        dst[(*len)++] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
        dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0x10FFFFu) {
        dst[(*len)++] = (char)(0xF0u | (cp >> 18u));
        dst[(*len)++] = (char)(0x80u | ((cp >> 12u) & 0x3Fu));
        dst[(*len)++] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
        dst[(*len)++] = (char)(0x80u | (cp & 0x3Fu));
    }
}

static bool parse_hex4(JsonParser *p, uint32_t *out) {
    if (p->pos + 4u > p->len) {
        return false;
    }
    uint32_t v = 0u;
    for (int i = 0; i < 4; ++i) {
        int h = hex_val(p->src[p->pos++]);
        if (h < 0) {
            return false;
        }
        v = (v << 4u) | (uint32_t)h;
    }
    *out = v;
    return true;
}

static char *parse_string(JsonParser *p, size_t *out_len) {
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        return nullptr;
    }
    p->pos++; // skip '"'
    size_t start = p->pos;
    // Measure upper bound (unescaped length <= raw length)
    size_t scan = start;
    bool has_escape = false;
    while (scan < p->len && p->src[scan] != '"') {
        if (p->src[scan] == '\\') {
            has_escape = true;
            scan += 2u;
            continue;
        }
        scan++;
    }
    if (scan >= p->len || p->src[scan] != '"') {
        return nullptr;
    }

    size_t raw_len = scan - start;
    char *dst = arena_alloc(p->doc, raw_len + 1u, 1u);
    if (dst == nullptr) {
        p->oom = true;
        return nullptr;
    }
    if (!has_escape) {
        memcpy(dst, p->src + start, raw_len);
        dst[raw_len] = '\0';
        p->pos = scan + 1u;
        if (out_len != nullptr) {
            *out_len = raw_len;
        }
        return dst;
    }

    size_t out = 0u;
    while (p->pos < p->len && p->src[p->pos] != '"') {
        char c = p->src[p->pos++];
        if (c != '\\') {
            dst[out++] = c;
            continue;
        }
        if (p->pos >= p->len) {
            return nullptr;
        }
        char esc = p->src[p->pos++];
        switch (esc) {
        case '"':  dst[out++] = '"';  break;
        case '\\': dst[out++] = '\\'; break;
        case '/':  dst[out++] = '/';  break;
        case 'b':  dst[out++] = '\b'; break;
        case 'f':  dst[out++] = '\f'; break;
        case 'n':  dst[out++] = '\n'; break;
        case 'r':  dst[out++] = '\r'; break;
        case 't':  dst[out++] = '\t'; break;
        case 'u': {
            uint32_t cp = 0u;
            if (!parse_hex4(p, &cp)) {
                return nullptr;
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu &&
                p->pos + 6u <= p->len && p->src[p->pos] == '\\' && p->src[p->pos + 1u] == 'u') {
                p->pos += 2u;
                uint32_t low = 0u;
                if (!parse_hex4(p, &low)) {
                    return nullptr;
                }
                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    cp = 0x10000u + (((cp - 0xD800u) << 10u) | (low - 0xDC00u));
                }
            }
            append_utf8(dst, &out, cp);
            break;
        }
        default:
            return nullptr;
        }
    }
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        return nullptr;
    }
    p->pos++; // skip closing '"'
    dst[out] = '\0';
    if (out_len != nullptr) {
        *out_len = out;
    }
    return dst;
}

static JsonVal *alloc_val(JsonParser *p, JsonType type) {
    JsonVal *v = arena_alloc(p->doc, sizeof(JsonVal), alignof(JsonVal));
    if (v == nullptr) {
        p->oom = true;
        return nullptr;
    }
    *v = (JsonVal){ .type = type };
    return v;
}

static void add_child(JsonVal *parent, JsonVal *child) {
    if (parent->first_child == nullptr) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
    parent->child_count++;
}

static JsonVal *parse_value(JsonParser *p);

static JsonVal *parse_array(JsonParser *p) {
    p->pos++; // skip '['
    JsonVal *arr = alloc_val(p, JSON_ARRAY);
    if (arr == nullptr) {
        return nullptr;
    }
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') {
        p->pos++;
        return arr;
    }
    while (p->pos < p->len) {
        JsonVal *elem = parse_value(p);
        if (elem == nullptr) {
            return nullptr;
        }
        add_child(arr, elem);
        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->src[p->pos] == ']') {
            p->pos++;
            return arr;
        }
        return nullptr;
    }
    return nullptr;
}

static JsonVal *parse_object(JsonParser *p) {
    p->pos++; // skip '{'
    JsonVal *obj = alloc_val(p, JSON_OBJECT);
    if (obj == nullptr) {
        return nullptr;
    }
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
        p->pos++;
        return obj;
    }
    while (p->pos < p->len) {
        skip_ws(p);
        char *key = parse_string(p, nullptr);
        if (key == nullptr) {
            return nullptr;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            return nullptr;
        }
        p->pos++;
        JsonVal *val = parse_value(p);
        if (val == nullptr) {
            return nullptr;
        }
        val->key = key;
        add_child(obj, val);
        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->src[p->pos] == '}') {
            p->pos++;
            return obj;
        }
        return nullptr;
    }
    return nullptr;
}

static JsonVal *parse_number(JsonParser *p) {
    size_t start = p->pos;
    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;
    if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
        return nullptr;
    }
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    size_t nlen = p->pos - start;
    char tmp[128];
    if (nlen >= sizeof tmp) {
        return nullptr;
    }
    memcpy(tmp, p->src + start, nlen);
    tmp[nlen] = '\0';
    char *end = nullptr;
    double val = strtod(tmp, &end);
    if (end == tmp || !isfinite(val)) {
        return nullptr;
    }
    JsonVal *v = alloc_val(p, JSON_NUMBER);
    if (v != nullptr) {
        v->num = val;
    }
    return v;
}

static JsonVal *parse_value(JsonParser *p) {
    if (p->depth >= k_json_max_depth) {
        return nullptr;
    }
    skip_ws(p);
    if (p->pos >= p->len) {
        return nullptr;
    }
    char c = p->src[p->pos];
    if (c == '{') {
        p->depth++;
        JsonVal *v = parse_object(p);
        p->depth--;
        return v;
    }
    if (c == '[') {
        p->depth++;
        JsonVal *v = parse_array(p);
        p->depth--;
        return v;
    }
    if (c == '"') {
        size_t slen = 0u;
        char *s = parse_string(p, &slen);
        if (s == nullptr) {
            return nullptr;
        }
        JsonVal *v = alloc_val(p, JSON_STRING);
        if (v != nullptr) {
            v->str     = s;
            v->str_len = slen;
        }
        return v;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number(p);
    }
    if (p->pos + 4u <= p->len && memcmp(p->src + p->pos, "true", 4u) == 0) {
        p->pos += 4u;
        JsonVal *v = alloc_val(p, JSON_BOOL);
        if (v != nullptr) v->boolean = true;
        return v;
    }
    if (p->pos + 5u <= p->len && memcmp(p->src + p->pos, "false", 5u) == 0) {
        p->pos += 5u;
        JsonVal *v = alloc_val(p, JSON_BOOL);
        if (v != nullptr) v->boolean = false;
        return v;
    }
    if (p->pos + 4u <= p->len && memcmp(p->src + p->pos, "null", 4u) == 0) {
        p->pos += 4u;
        return alloc_val(p, JSON_NULL);
    }
    return nullptr;
}

int json_parse(const char *text, size_t len, JsonDoc *out_doc) {
    if (text == nullptr || len == 0u || out_doc == nullptr) {
        return -1;
    }
    *out_doc = (JsonDoc){0};
    JsonParser p = {
        .src = text,
        .len = len,
        .doc = out_doc,
    };
    out_doc->root = parse_value(&p);
    if (out_doc->root == nullptr || p.oom) {
        json_doc_free(out_doc);
        return -1;
    }
    return 0;
}

void json_doc_free(JsonDoc *doc) {
    if (doc == nullptr) {
        return;
    }
    JsonArenaBlock *blk = doc->blocks;
    while (blk != nullptr) {
        JsonArenaBlock *next = blk->next;
        free(blk);
        blk = next;
    }
    *doc = (JsonDoc){0};
}

const JsonVal *json_obj_get(const JsonVal *obj, const char *key) {
    if (obj == nullptr || obj->type != JSON_OBJECT || key == nullptr) {
        return nullptr;
    }
    for (const JsonVal *c = obj->first_child; c != nullptr; c = c->next_sibling) {
        if (c->key != nullptr && strcmp(c->key, key) == 0) {
            return c;
        }
    }
    return nullptr;
}

const JsonVal *json_arr_at(const JsonVal *arr, uint32_t index) {
    if (arr == nullptr || arr->type != JSON_ARRAY || index >= arr->child_count) {
        return nullptr;
    }
    const JsonVal *c = arr->first_child;
    for (uint32_t i = 0u; i < index && c != nullptr; ++i) {
        c = c->next_sibling;
    }
    return c;
}

const char *json_as_str(const JsonVal *v, const char *fallback) {
    return (v != nullptr && v->type == JSON_STRING && v->str != nullptr) ? v->str : fallback;
}

double json_as_num(const JsonVal *v, double fallback) {
    return (v != nullptr && v->type == JSON_NUMBER) ? v->num : fallback;
}

int32_t json_as_i32(const JsonVal *v, int32_t fallback) {
    return (v != nullptr && v->type == JSON_NUMBER) ? (int32_t)llround(v->num) : fallback;
}

bool json_as_bool(const JsonVal *v, bool fallback) {
    return (v != nullptr && v->type == JSON_BOOL) ? v->boolean : fallback;
}

bool json_read_f32_arr(const JsonVal *arr, float *out, uint32_t count) {
    if (arr == nullptr || arr->type != JSON_ARRAY || out == nullptr || arr->child_count < count) {
        return false;
    }
    const JsonVal *c = arr->first_child;
    for (uint32_t i = 0u; i < count; ++i) {
        if (c == nullptr || c->type != JSON_NUMBER) {
            return false;
        }
        out[i] = (float)c->num;
        c = c->next_sibling;
    }
    return true;
}
