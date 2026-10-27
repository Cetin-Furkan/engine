#ifndef ENGINE_TEXT_INT_H
#define ENGINE_TEXT_INT_H

#include "frame/text.h"
#include <stdint.h>

// One screen quad. Pixel y grows down. mode 0 is solid and mode 1 samples
// the SDF atlas. The box is a pixel clip. The vertex shader expands the
// six corners. Twenty floats, three of them padding, so the stride is 80.

typedef struct TextQuad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float r, g, b, a;
    float bx0, by0, bx1, by1;
    float mode;
    float pad0, pad1, pad2;
} TextQuad;

static_assert(sizeof(TextQuad) == 80u, "text quad");

void text_place_boot(void);
uint32_t text_fill(TextQuad *out, uint32_t cap, uint32_t fb_w, uint32_t fb_h);

#endif /* ENGINE_TEXT_INT_H */
