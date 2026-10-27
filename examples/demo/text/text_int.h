#ifndef ENGINE_TEXT_INT_H
#define ENGINE_TEXT_INT_H

#include "frame/text.h"
#include <stdint.h>

// One vertex of a screen quad. Pixel y grows down. mode 0 is solid
// and mode 1 samples the SDF atlas. The box is a pixel clip.

typedef struct TextVert {
    float x, y;
    float u, v;
    float r, g, b, a;
    float bx0, by0, bx1, by1;
    float mode;
    float pad;
} TextVert;

static_assert(sizeof(TextVert) == 56u, "text vertex");

void text_place_boot(void);
uint32_t text_fill(TextVert *out, uint32_t cap, uint32_t fb_w, uint32_t fb_h);

#endif /* ENGINE_TEXT_INT_H */
