#ifndef ENGINE_GLTF_JSON_H
#define ENGINE_GLTF_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum JsonType : uint8_t {
    JSON_NULL   = 0u,
    JSON_BOOL   = 1u,
    JSON_NUMBER = 2u,
    JSON_STRING = 3u,
    JSON_ARRAY  = 4u,
    JSON_OBJECT = 5u,
} JsonType;

typedef struct JsonVal JsonVal;

struct JsonVal {
    JsonType type;
    char    *key;        // Owned by doc string arena (non-null for object members)
    char    *str;        // Owned by doc string arena (for JSON_STRING)
    size_t   str_len;
    double   num;
    bool     boolean;
    uint32_t child_count;
    JsonVal *first_child;
    JsonVal *last_child;
    JsonVal *next_sibling;
};

typedef struct JsonArenaBlock JsonArenaBlock;

typedef struct JsonDoc {
    JsonVal        *root;
    JsonArenaBlock *blocks;
} JsonDoc;

[[nodiscard]] int json_parse(const char *text, size_t len, JsonDoc *out_doc);
void json_doc_free(JsonDoc *doc);

const JsonVal *json_obj_get(const JsonVal *obj, const char *key);
const JsonVal *json_arr_at(const JsonVal *arr, uint32_t index);
const char    *json_as_str(const JsonVal *v, const char *fallback);
double         json_as_num(const JsonVal *v, double fallback);
int32_t        json_as_i32(const JsonVal *v, int32_t fallback);
bool           json_as_bool(const JsonVal *v, bool fallback);
bool           json_read_f32_arr(const JsonVal *arr, float *out, uint32_t count);

#endif /* ENGINE_GLTF_JSON_H */
