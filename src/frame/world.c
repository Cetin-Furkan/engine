#include "frame/world.h"
#include "frame/cube.h"
#include "frame/shape.h"
#include <math.h>
#include <stdio.h>

// The cube's still pose is 38 degrees. One extra revolution is 360000
// milli-degrees over 5000 ms, which is 72 milli-degrees per millisecond.
// A closed quad uses the same clock and rests at angle 0.
enum : int32_t {
    k_yaw_start_milli = 38000,
};
enum : uint32_t {
    k_turn_period_ms = 5000u,
    k_milli_per_ms   = 72u,
    k_quad_cap       = 32u,
    k_build_cap      = 4u,
};

// The pitched cube stays inside this radius. A new corner has to miss it,
// miss a finished quad, and miss a corner already placed.
static const float k_hit_radius_px = 200.0f;
static const float k_near_px       = 8.0f;
static const float k_stroke_px     = 3.5f;
static const float k_mark_px       = 5.0f;
static const float k_min_area      = 16.0f;
static const float k_deg_to_rad    = 0.01745329251f;

typedef struct Fillet {
    int32_t  shown_milli;
    int32_t  live_milli;
    uint32_t t0;
    bool     have_t0;
    bool     spinning;
} Fillet;

typedef struct Quad {
    float    x[4];
    float    y[4];
    float    cx;
    float    cy;
    int32_t  shown_milli;
    int32_t  live_milli;
    uint32_t t0;
    bool     have_t0;
    bool     spinning;
} Quad;

typedef struct Point {
    float x;
    float y;
} Point;

static Fillet   s_cube;
static Quad     s_quad[k_quad_cap];
static uint32_t s_quad_count = 0;
static uint32_t s_shown_quads = 0;
static Point    s_build[k_build_cap];
static uint32_t s_build_n = 0;
static Point    s_shown_build[k_build_cap];
static uint32_t s_shown_build_n = 0;
static bool     s_logged_start = false;
static bool     s_logged_full = false;
static bool     s_spun = false;

void world_reset(void) {
    s_cube = (Fillet){
        .shown_milli = k_yaw_start_milli,
        .live_milli  = k_yaw_start_milli,
        .spinning    = true,
    };
    s_quad_count = 0;
    s_shown_quads = 0;
    s_build_n = 0;
    s_shown_build_n = 0;
    s_logged_start = false;
    s_logged_full = false;
    s_spun = true;
}

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

// Angle around the centroid. A rectangle's four corners, in any click order,
// come out as that rectangle. A crossing pair is rejected by the caller.
static void order_quad(float x[4], float y[4], float *cx, float *cy) {
    *cx = (x[0] + x[1] + x[2] + x[3]) * 0.25f;
    *cy = (y[0] + y[1] + y[2] + y[3]) * 0.25f;
    for (int i = 1; i < 4; ++i) {
        float px = x[i];
        float py = y[i];
        float pa = atan2f(py - *cy, px - *cx);
        int j = i;
        while (j > 0) {
            float qa = atan2f(y[j - 1] - *cy, x[j - 1] - *cx);
            if (qa <= pa) {
                break;
            }
            x[j] = x[j - 1];
            y[j] = y[j - 1];
            --j;
        }
        x[j] = px;
        y[j] = py;
    }
}

static bool quad_solid(const float x[4], const float y[4]) {
    float area = 0.0f;
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        area += x[i] * y[j] - x[j] * y[i];
    }
    if (area < 0.0f) {
        area = -area;
    }
    area *= 0.5f;
    if (area < k_min_area) {
        return false;
    }
    if (segments_cross(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3])) {
        return false;
    }
    if (segments_cross(x[1], y[1], x[2], y[2], x[3], y[3], x[0], y[0])) {
        return false;
    }
    return true;
}

static float rad_of(int32_t milli) {
    return ((float)milli / 1000.0f) * k_deg_to_rad;
}

static void placed_corners(const Quad *q, float x[4], float y[4]) {
    float ang = rad_of(q->shown_milli);
    float c = cosf(ang);
    float s = sinf(ang);
    for (int i = 0; i < 4; ++i) {
        float dx = q->x[i] - q->cx;
        float dy = q->y[i] - q->cy;
        x[i] = q->cx + c * dx - s * dy;
        y[i] = q->cy + s * dx + c * dy;
    }
}

static bool inside_ccw(const float x[4], const float y[4], float px, float py) {
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        if (cross2(x[i], y[i], x[j], y[j], px, py) < 0.0f) {
            return false;
        }
    }
    return true;
}

static float dist2_seg(float x0, float y0, float x1, float y1, float px, float py) {
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

static bool on_quad(const Quad *q, float px, float py) {
    float x[4];
    float y[4];
    placed_corners(q, x, y);
    if (inside_ccw(x, y, px, py)) {
        return true;
    }
    const float r2 = k_near_px * k_near_px;
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) & 3;
        if (dist2_seg(x[i], y[i], x[j], y[j], px, py) < r2) {
            return true;
        }
    }
    return false;
}

static bool near_build(float x, float y) {
    const float r2 = k_near_px * k_near_px;
    for (uint32_t i = 0; i < s_build_n; ++i) {
        float dx = x - s_build[i].x;
        float dy = y - s_build[i].y;
        if (dx * dx + dy * dy < r2) {
            return true;
        }
    }
    return false;
}

static bool blocked(float x, float y) {
    if (x * x + y * y < k_hit_radius_px * k_hit_radius_px) {
        return true;
    }
    for (uint32_t i = 0; i < s_quad_count; ++i) {
        if (on_quad(&s_quad[i], x, y)) {
            return true;
        }
    }
    return near_build(x, y);
}

bool world_spawn(float origin_x, float origin_y) {
    if (blocked(origin_x, origin_y)) {
        return false;
    }
    if (s_quad_count >= k_quad_cap && s_build_n == 0) {
        if (!s_logged_full) {
            fprintf(stderr, "[Engine] Rectangle list is full (%u)\n", k_quad_cap);
            s_logged_full = true;
        }
        return false;
    }
    if (s_build_n < 3u) {
        s_build[s_build_n].x = origin_x;
        s_build[s_build_n].y = origin_y;
        s_build_n++;
        return true;
    }

    float x[4] = { s_build[0].x, s_build[1].x, s_build[2].x, origin_x };
    float y[4] = { s_build[0].y, s_build[1].y, s_build[2].y, origin_y };
    float cx = 0.0f;
    float cy = 0.0f;
    order_quad(x, y, &cx, &cy);
    if (!quad_solid(x, y) || s_quad_count >= k_quad_cap) {
        return false;
    }

    Quad *q = &s_quad[s_quad_count];
    *q = (Quad){
        .cx       = cx,
        .cy       = cy,
        .spinning = true,
    };
    for (int i = 0; i < 4; ++i) {
        q->x[i] = x[i];
        q->y[i] = y[i];
    }
    s_quad_count++;
    s_build_n = 0;
    s_spun = true;
    printf("[Engine] Rectangle %u closed at %+.0f,%+.0f. One revolution, then it holds.\n",
           s_quad_count, (double)cx, (double)cy);
    fflush(stdout);
    return true;
}

static bool build_differs(void) {
    if (s_build_n != s_shown_build_n) {
        return true;
    }
    for (uint32_t i = 0; i < s_build_n; ++i) {
        if (s_build[i].x != s_shown_build[i].x || s_build[i].y != s_shown_build[i].y) {
            return true;
        }
    }
    return false;
}

static bool advance_one(int32_t rest, int32_t *live, int32_t shown,
                        uint32_t *t0, bool *have, bool *spinning,
                        uint32_t time_ms, bool log_start) {
    if (!*spinning) {
        return false;
    }
    if (!*have) {
        *t0 = time_ms;
        *have = true;
        *live = shown;
        if (log_start && !s_logged_start) {
            printf("[Engine] Turn started. One revolution, wl_surface.frame clock, %u ms.\n",
                   k_turn_period_ms);
            fflush(stdout);
            s_logged_start = true;
        }
        return false;
    }
    uint32_t dt = time_ms - *t0;
    int32_t milli = rest;
    if (dt < k_turn_period_ms) {
        milli += (int32_t)(k_milli_per_ms * dt);
    }
    *live = milli;
    if (milli == shown) {
        if (dt >= k_turn_period_ms) {
            *spinning = false;
        }
        return false;
    }
    return true;
}

bool world_busy(void) {
    if (s_cube.spinning || s_cube.live_milli != s_cube.shown_milli) {
        return true;
    }
    if (s_quad_count != s_shown_quads || build_differs()) {
        return true;
    }
    for (uint32_t i = 0; i < s_quad_count; ++i) {
        if (s_quad[i].spinning || s_quad[i].live_milli != s_quad[i].shown_milli) {
            return true;
        }
    }
    return false;
}

bool world_advance(uint32_t time_ms) {
    bool changed = s_quad_count != s_shown_quads || build_differs();
    if (advance_one(k_yaw_start_milli, &s_cube.live_milli, s_cube.shown_milli,
                    &s_cube.t0, &s_cube.have_t0, &s_cube.spinning, time_ms, true)) {
        changed = true;
    }
    for (uint32_t i = 0; i < s_quad_count; ++i) {
        Quad *q = &s_quad[i];
        if (advance_one(0, &q->live_milli, q->shown_milli,
                        &q->t0, &q->have_t0, &q->spinning, time_ms, false)) {
            changed = true;
        }
    }
    return changed;
}

void world_commit(void) {
    s_cube.shown_milli = s_cube.live_milli;
    for (uint32_t i = 0; i < s_quad_count; ++i) {
        s_quad[i].shown_milli = s_quad[i].live_milli;
    }
    s_shown_quads = s_quad_count;
    s_shown_build_n = s_build_n;
    for (uint32_t i = 0; i < s_build_n; ++i) {
        s_shown_build[i] = s_build[i];
    }
}

bool world_consume_spin(void) {
    if (world_busy() || !s_spun) {
        return false;
    }
    s_spun = false;
    return true;
}

static void stroke_corners(float x0, float y0, float x1, float y1, float half,
                           float x[4], float y[4]) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float nx = 0.0f;
    float ny = 0.0f;
    if (len > 0.001f) {
        nx = -dy / len * half;
        ny = dx / len * half;
    }
    x[0] = x0 - nx;
    y[0] = y0 - ny;
    x[1] = x1 - nx;
    y[1] = y1 - ny;
    x[2] = x1 + nx;
    y[2] = y1 + ny;
    x[3] = x0 + nx;
    y[3] = y0 + ny;
}

static void mark_corners(float px, float py, float half, float x[4], float y[4]) {
    x[0] = px - half;
    y[0] = py - half;
    x[1] = px + half;
    y[1] = py - half;
    x[2] = px + half;
    y[2] = py + half;
    x[3] = px - half;
    y[3] = py + half;
}

static void draw_quads(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    uint32_t n = live ? s_quad_count : s_shown_quads;
    const float er = 0.98f;
    const float eg = 0.96f;
    const float eb = 0.90f;
    for (uint32_t i = 0; i < n; ++i) {
        const Quad *q = &s_quad[i];
        int32_t milli = live ? q->live_milli : q->shown_milli;
        float ang = rad_of(milli);
        float base = 0.30f - (float)i * (1.0f / 4096.0f);
        shape_draw(cmd, width, height, q->x, q->y, q->cx, q->cy, ang, base,
                   0.96f, 0.71f, 0.28f, 1.0f);
        for (int e = 0; e < 4; ++e) {
            int j = (e + 1) & 3;
            float xs[4];
            float ys[4];
            stroke_corners(q->x[e], q->y[e], q->x[j], q->y[j], k_stroke_px, xs, ys);
            shape_draw(cmd, width, height, xs, ys, q->cx, q->cy, ang, base - 0.01f,
                       er, eg, eb, 1.0f);
        }
        for (int v = 0; v < 4; ++v) {
            float xs[4];
            float ys[4];
            mark_corners(q->x[v], q->y[v], k_mark_px, xs, ys);
            shape_draw(cmd, width, height, xs, ys, q->cx, q->cy, ang, base - 0.02f,
                       er, eg, eb, 1.0f);
        }
    }
}

static void draw_preview(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const Point *pts = live ? s_build : s_shown_build;
    uint32_t n = live ? s_build_n : s_shown_build_n;
    const float r = 0.98f;
    const float g = 0.96f;
    const float b = 0.90f;
    for (uint32_t i = 1; i < n; ++i) {
        float dx = pts[i].x - pts[i - 1].x;
        float dy = pts[i].y - pts[i - 1].y;
        if (dx * dx + dy * dy < 0.25f) {
            continue;
        }
        float xs[4];
        float ys[4];
        stroke_corners(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, k_stroke_px, xs, ys);
        shape_draw(cmd, width, height, xs, ys, 0.0f, 0.0f, 0.0f, 0.10f, r, g, b, 1.0f);
    }
    for (uint32_t i = 0; i < n; ++i) {
        float xs[4];
        float ys[4];
        mark_corners(pts[i].x, pts[i].y, k_mark_px, xs, ys);
        shape_draw(cmd, width, height, xs, ys, 0.0f, 0.0f, 0.0f, 0.08f, r, g, b, 1.0f);
    }
}

void world_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    int32_t milli = live ? s_cube.live_milli : s_cube.shown_milli;
    cube_draw(cmd, width, height, (float)milli / 1000.0f, 0.0f, 0.0f, 0.0f);
    draw_quads(cmd, width, height, live);
    draw_preview(cmd, width, height, live);
}

void world_stats(uint32_t *objects, uint32_t *vertices) {
    if (objects != nullptr) {
        *objects = 1u + s_quad_count;
    }
    if (vertices != nullptr) {
        *vertices = cube_vertex_count();
    }
}
