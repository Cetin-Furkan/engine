#ifndef ENGINE_TEXT_SHAPE_H
#define ENGINE_TEXT_SHAPE_H

#include "text/face/face.h"
#include <stdint.h>

// One glyph in box-local pixels. The origin is the top-left of the box.
// +X is right and +Y is up, so a line's y is at or below zero.
// pen_x and pen_y are the baseline point before this glyph. An empty
// bitmap keeps the pen and sets visible to false.

typedef struct TextGlyphQuad {
    float    x0, y0, x1, y1;
    float    pen_x, pen_y;
    uint32_t glyph;
    uint32_t byte;
    bool     visible;
} TextGlyphQuad;

// Greedy cluster wrap. A line that does not fit is broken on the last
// space that fits, and the remainder is shaped again. Lines past the
// box are omitted. The caret is the pen of the cluster at `caret`.
[[nodiscard]] uint32_t text_shape_box(uint32_t face, const char *utf8, uint32_t nbytes,
                                      float box_w, float box_h, float px, uint32_t caret,
                                      TextGlyphQuad *out, uint32_t cap,
                                      float *caret_x, float *caret_y,
                                      float *ascender, float *descender);

// Unwrapped advance of a short string, in draw pixels.
[[nodiscard]] float text_shape_advance(uint32_t face, const char *utf8, float px);

#endif /* ENGINE_TEXT_SHAPE_H */
