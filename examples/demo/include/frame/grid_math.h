#ifndef DEMO_FRAME_GRID_MATH_H
#define DEMO_FRAME_GRID_MATH_H

#include <math.h>
#include <stdint.h>

// Pure math for the universal metric grid (no Vulkan, unit-tested).
// cell_med is the 10^k decade nearest the "medium" target: the decade
// whose cell is about 45% of the smaller screen dimension. level_frac
// is the fractional position inside that decade (0 = just subdivided).
// span is the world-space half-extent of the single ground quad: the
// visible orthographic region on z = 0 lies within the viewport
// half-diagonal (in world units) plus the 2048 depth range the view
// projection maps into [0, 1], so this span covers it at any angle.

typedef struct PlaneGrid {
    float cell_med;
    float level_frac;
    float span;
} PlaneGrid;

static inline PlaneGrid plane_grid_for(uint32_t width, uint32_t height, float zoom) {
    float w = (float)width;
    float h = (float)height;
    float z = zoom > 1.0e-5f ? zoom : 1.0e-5f;
    float min_dim = w < h ? w : h;
    float target_world = (min_dim * 0.45f) / z;
    float log_level = log10f(target_world > 1.0e-6f ? target_world : 1.0e-6f);
    float level_int = floorf(log_level);
    PlaneGrid grid = {
        .cell_med   = powf(10.0f, level_int),
        .level_frac = log_level - level_int,
        .span       = 0.5f * sqrtf(w * w + h * h) / z + 4096.0f,
    };
    return grid;
}

#endif /* DEMO_FRAME_GRID_MATH_H */
