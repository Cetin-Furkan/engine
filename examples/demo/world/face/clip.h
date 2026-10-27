#ifndef ENGINE_WORLD_FACE_CLIP_H
#define ENGINE_WORLD_FACE_CLIP_H

#include <stdbool.h>
#include <stdint.h>

enum : uint32_t {
    k_point_cap = 64u,
    k_tri_cap   = 62u,
};

// Click order stays. A crossing outline is refused. An ear clip fills a
// concave outline without sorting around the centroid.

bool clip_cross(const float *x, const float *y, uint32_t n);
bool clip_poly(const float *x, const float *y, uint32_t n,
               uint16_t *idx, uint32_t *tri_n);
bool clip_inside(const float *x, const float *y, uint32_t n, float px, float py);
float clip_dist2_seg(float x0, float y0, float x1, float y1, float px, float py);
void clip_self_check(void);

#endif /* ENGINE_WORLD_FACE_CLIP_H */
