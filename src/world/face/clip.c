#include "world/face/clip.h"
#include <math.h>
#include <stdio.h>

static_assert(k_tri_cap == k_point_cap - 2u, "one triangle per extra point");

static const float k_min_area = 16.0f;

static float cross2(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool segments_cross(float ax, float ay, float bx, float by,
                           float cx, float cy, float dx, float dy) {
    float a1 = cross2(ax, ay, bx, by, cx, cy);
    float a2 = cross2(ax, ay, bx, by, dx, dy);
    float a3 = cross2(cx, cy, dx, dy, ax, ay);
    float a4 = cross2(cx, cy, dx, dy, bx, by);
    return (a1 * a2 < 0.0f) && (a3 * a4 < 0.0f);
}

bool clip_cross(const float *x, const float *y, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t i2 = (i + 1u) % n;
        for (uint32_t j = i + 1u; j < n; ++j) {
            uint32_t j2 = (j + 1u) % n;
            if (i2 == j || i == j2 || i2 == j2) {
                continue;
            }
            if (segments_cross(x[i], y[i], x[i2], y[i2], x[j], y[j], x[j2], y[j2])) {
                return true;
            }
        }
    }
    return false;
}

bool clip_poly(const float *x, const float *y, uint32_t n,
               uint16_t *idx, uint32_t *tri_n) {
    if (n < 3u || n > k_point_cap || idx == nullptr || tri_n == nullptr) {
        return false;
    }
    float area2 = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (i + 1u) % n;
        area2 += x[i] * y[j] - x[j] * y[i];
    }
    float area = area2 * 0.5f;
    if (fabsf(area) < k_min_area || clip_cross(x, y, n)) {
        return false;
    }
    float orient = area > 0.0f ? 1.0f : -1.0f;
    uint32_t v[k_point_cap];
    for (uint32_t i = 0; i < n; ++i) {
        v[i] = i;
    }
    uint32_t left = n;
    uint32_t out = 0;
    uint32_t guard = 0;
    while (left > 3u) {
        if (guard++ > n * n) {
            return false;
        }
        bool clipped = false;
        for (uint32_t i = 0; i < left; ++i) {
            uint32_t ip = (i + left - 1u) % left;
            uint32_t in = (i + 1u) % left;
            uint32_t a = v[ip];
            uint32_t b = v[i];
            uint32_t c = v[in];
            float cr = cross2(x[a], y[a], x[b], y[b], x[c], y[c]);
            if (cr * orient <= 1.0e-3f) {
                continue;
            }
            bool ear = true;
            for (uint32_t k = 0; k < left; ++k) {
                if (k == ip || k == i || k == in) {
                    continue;
                }
                uint32_t p = v[k];
                float c1 = cross2(x[a], y[a], x[b], y[b], x[p], y[p]);
                float c2 = cross2(x[b], y[b], x[c], y[c], x[p], y[p]);
                float c3 = cross2(x[c], y[c], x[a], y[a], x[p], y[p]);
                if (c1 * orient > 1.0e-4f && c2 * orient > 1.0e-4f && c3 * orient > 1.0e-4f) {
                    ear = false;
                    break;
                }
            }
            if (!ear) {
                continue;
            }
            if (out + 3u > k_tri_cap * 3u) {
                return false;
            }
            idx[out++] = (uint16_t)a;
            idx[out++] = (uint16_t)b;
            idx[out++] = (uint16_t)c;
            for (uint32_t k = i + 1u; k < left; ++k) {
                v[k - 1u] = v[k];
            }
            left--;
            clipped = true;
            break;
        }
        if (!clipped) {
            return false;
        }
    }
    if (out + 3u > k_tri_cap * 3u) {
        return false;
    }
    idx[out++] = (uint16_t)v[0];
    idx[out++] = (uint16_t)v[1];
    idx[out++] = (uint16_t)v[2];
    *tri_n = out / 3u;
    return *tri_n == n - 2u;
}

bool clip_inside(const float *x, const float *y, uint32_t n, float px, float py) {
    int wind = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (i + 1u) % n;
        if (y[i] <= py) {
            if (y[j] > py && cross2(x[i], y[i], x[j], y[j], px, py) > 0.0f) {
                wind++;
            }
        } else if (y[j] <= py && cross2(x[i], y[i], x[j], y[j], px, py) < 0.0f) {
            wind--;
        }
    }
    return wind != 0;
}

float clip_dist2_seg(float x0, float y0, float x1, float y1, float px, float py) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = ((px - x0) * dx + (py - y0) * dy) / len2;
        if (t < 0.0f) {
            t = 0.0f;
        }
        if (t > 1.0f) {
            t = 1.0f;
        }
    }
    float sx = x0 + t * dx - px;
    float sy = y0 + t * dy - py;
    return sx * sx + sy * sy;
}

void clip_self_check(void) {
    float sx[4] = { 0.0f, 40.0f, 40.0f, 0.0f };
    float sy[4] = { 0.0f, 0.0f, 40.0f, 40.0f };
    float cx[5] = { 0.0f, 40.0f, 10.0f, 40.0f, 0.0f };
    float cy[5] = { 0.0f, 0.0f, 10.0f, 30.0f, 30.0f };
    float bx[4] = { 0.0f, 40.0f, 0.0f, 40.0f };
    float by[4] = { 0.0f, 40.0f, 40.0f, 0.0f };
    uint16_t idx[k_tri_cap * 3u];
    uint32_t tris = 0;
    bool square = clip_poly(sx, sy, 4u, idx, &tris) && tris == 2u;
    bool concave = clip_poly(cx, cy, 5u, idx, &tris) && tris == 3u;
    bool bowtie = clip_poly(bx, by, 4u, idx, &tris);
    if (!square || !concave || bowtie) {
        fprintf(stderr, "[World Error] Face close self-check failed\n");
    }
}
