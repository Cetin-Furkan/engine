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
    // Coarse decade, cell_med * 10. The grid phase lives in one of these
    // cells. span_steps is how many whole coarse cells fit in span, and
    // span_mod is the remainder in [0, coarse). The fragment rebuilds
    // mod(camera + c * span, coarse) from these so it never adds the pan
    // to a multi-thousand-unit corner.
    float coarse;
    float span_steps;
    float span_mod;
} PlaneGrid;

// GLSL mod, then folded back into [0, period). The camera pan is reduced
// in double so a coordinate past a few thousand world units still keeps
// the coarse-cell phase the float itself can represent.
static inline float plane_grid_wrap(float x, float period) {
    double p = (double)period;
    double v = (double)x;
    if (!(p > 0.0) || !isfinite(v)) {
        return 0.0f;
    }
    double m = v - p * floor(v / p);
    if (m < 0.0) {
        m += p;
    }
    if (m >= p) {
        m -= p;
    }
    return (float)m;
}

static inline PlaneGrid plane_grid_for(uint32_t width, uint32_t height, float zoom) {
    float w = (float)width;
    float h = (float)height;
    float z = zoom > 1.0e-5f ? zoom : 1.0e-5f;
    float min_dim = w < h ? w : h;
    float target_world = (min_dim * 0.45f) / z;
    float log_level = log10f(target_world > 1.0e-6f ? target_world : 1.0e-6f);
    float level_int = floorf(log_level);
    float cell = powf(10.0f, level_int);
    float coarse = cell * 10.0f;
    float span = 0.5f * sqrtf(w * w + h * h) / z + 4096.0f;
    float steps = 0.0f;
    float span_mod = 0.0f;
    if (coarse > 0.0f && isfinite(span)) {
        steps = floorf(span / coarse);
        span_mod = span - steps * coarse;
        if (span_mod < 0.0f) {
            span_mod = 0.0f;
        }
        if (span_mod >= coarse) {
            steps += 1.0f;
            span_mod = 0.0f;
        }
    }
    PlaneGrid grid = {
        .cell_med   = cell,
        .level_frac = log_level - level_int,
        .span       = span,
        .coarse     = coarse,
        .span_steps = steps,
        .span_mod   = span_mod,
    };
    return grid;
}

// Fragment phase for one axis. c is the quad corner in [-1, 1], or the
// interpolated corner inside the quad. Matches shaders/plane.frag.
static inline float plane_grid_phase(float wrapped_cam, float c, const PlaneGrid *grid) {
    if (grid == nullptr || !(grid->coarse > 0.0f)) {
        return 0.0f;
    }
    float turns = c * grid->span_steps;
    float frac_turns = turns - floorf(turns);
    float phase = frac_turns * grid->coarse + c * grid->span_mod;
    return plane_grid_wrap(wrapped_cam + phase, grid->coarse);
}

#endif /* DEMO_FRAME_GRID_MATH_H */
