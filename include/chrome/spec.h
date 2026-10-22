#ifndef ENGINE_CHROME_SPEC_H
#define ENGINE_CHROME_SPEC_H

#include <stdint.h>

enum : uint32_t {
    ENG_CHROME_LAYERS   = 4,
    ENG_CHROME_CONTROLS = 8,
    ENG_CHROME_NAME     = 32,
    ENG_CHROME_TEXT     = 128,
};

typedef struct ChromeColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} ChromeColor;

typedef struct ChromeLayerSpec {
    char        name[ENG_CHROME_NAME];
    char        edge[8];
    char        span[8];
    uint32_t    thickness;
    ChromeColor color;
} ChromeLayerSpec;

typedef struct ChromeControlSpec {
    char        name[ENG_CHROME_NAME];
    char        layer[ENG_CHROME_NAME];
    char        action[16];
    char        align[8];
    uint32_t    span;
    ChromeColor color;
    ChromeColor glyph;
    uint32_t    cursor_shape;
    bool        has_cursor;
} ChromeControlSpec;

typedef struct ChromeSpec {
    char     path[256];
    char     title[ENG_CHROME_TEXT];
    char     app_id[ENG_CHROME_TEXT];
    uint32_t cursor_shape;
    bool     resize;
    uint32_t resize_band;

    ChromeLayerSpec   layers[ENG_CHROME_LAYERS];
    uint32_t          nlayers;
    ChromeControlSpec controls[ENG_CHROME_CONTROLS];
    uint32_t          ncontrols;
} ChromeSpec;

/**
 * @brief Reads the chrome spec once. Unknown keys, missing required keys, and
 *        unknown cursor names fail. Nothing in the spec is invented here.
 */
[[nodiscard]] int chrome_load(void);

[[nodiscard]] const ChromeSpec *chrome_spec(void);

/** Resolves a wp_cursor_shape_device_v1.shape name. Returns false if unknown. */
[[nodiscard]] bool chrome_cursor_shape(const char *name, uint32_t *out_shape);

#endif /* ENGINE_CHROME_SPEC_H */
