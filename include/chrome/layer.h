#ifndef ENGINE_CHROME_LAYER_H
#define ENGINE_CHROME_LAYER_H

#include "chrome/spec.h"
#include "wayland/wayland.h"
#include <stdint.h>

typedef struct ChromeHit {
    uint32_t cursor_shape;
    uint32_t resize_edge;
    bool     close;
    bool     title;
} ChromeHit;

typedef struct ChromePaintControl {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t gr;
    uint8_t gg;
    uint8_t gb;
    bool    draw_glyph;
} ChromePaintControl;

typedef struct ChromePaintLayer {
    int32_t            x;
    int32_t            y;
    int32_t            w;
    int32_t            h;
    uint8_t            r;
    uint8_t            g;
    uint8_t            b;
    uint32_t           ncontrols;
    ChromePaintControl controls[ENG_CHROME_CONTROLS];
} ChromePaintLayer;

/**
 * @brief Remembers the dma-buf size the chrome was painted into.
 * Hit tests use this size.
 */
void chrome_set_buffer_size(uint32_t width, uint32_t height);

/**
 * @brief Smallest window the spec can draw. Height is the top and bottom
 * thicknesses. Width is the widest layer's controls side by side.
 * Empty chrome is 1x1. A zero from the compositor still means "client chooses".
 */
void chrome_min_size(uint32_t *width, uint32_t *height);

/**
 * @brief Raises a positive size up to chrome_min_size. Leaves 0 unchanged.
 */
void chrome_clamp_size(uint32_t *width, uint32_t *height);

/**
 * @brief One spec layer in buffer pixels, including the controls on it.
 * index walks the spec in paint order. Returns false when that layer is absent
 * or thicker than the buffer. The fragment shader fills these rectangles.
 */
bool chrome_paint_layer(uint32_t width, uint32_t height, uint32_t index, ChromePaintLayer *out);

/**
 * @brief Hit-tests toplevel surface coordinates.
 * Corners, then the close control, then the resize band, then the rest of a top bar.
 */
void chrome_hit_test(
    const WaylandContext *w,
    uint32_t              surface,
    int32_t               x,
    int32_t               y,
    ChromeHit            *out
);

#endif /* ENGINE_CHROME_LAYER_H */
