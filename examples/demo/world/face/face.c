#include "world/world_int.h"
#include "world/face/clip.h"
#include "frame/material.h"
#include "frame/shape.h"
#include <math.h>
#include <stdio.h>

static_assert((uint32_t)32 == (uint32_t)k_mat_faces, "one material slot per face");

enum : uint32_t { k_face_cap = 32u };

static const float k_stroke_px = 3.5f;
static const float k_mark_px   = 5.0f;
static const float k_deg       = 0.01745329251f;

typedef struct FaceData {
    bool     used;
    Coord    p[k_point_cap];
    uint16_t idx[k_tri_cap * 3u];
    uint32_t n;
    uint32_t tri_n;
    Coord    center;
    Spin     spin;
    uint32_t mat;
    uint32_t serial;
    char     name[16];
} FaceData;

static FaceData s_pool[k_face_cap];
static uint32_t s_serial = 0;
static Coord    s_build[k_point_cap];
static uint32_t s_build_n = 0;
static Coord    s_shown_build[k_point_cap];
static uint32_t s_shown_build_n = 0;
static bool     s_logged_full = false;
static bool     s_logged_chain = false;

static const WorldOps s_ops;

static float rad_of(int32_t milli) {
    return ((float)milli / 1000.0f) * k_deg;
}

static uint32_t used_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < k_face_cap; ++i) {
        if (s_pool[i].used) {
            n++;
        }
    }
    return n;
}

static int alloc_slot(void) {
    for (uint32_t i = 0; i < k_face_cap; ++i) {
        if (!s_pool[i].used) {
            return (int)i;
        }
    }
    return -1;
}

static void placed(const FaceData *face, bool shown_pose, float *x, float *y) {
    int32_t milli = shown_pose ? face->spin.shown_milli : face->spin.live_milli;
    float ang = rad_of(milli);
    float c = cosf(ang);
    float s = sinf(ang);
    uint32_t n = face->n;
    if (n > k_point_cap) {
        n = k_point_cap;
    }
    for (uint32_t i = 0; i < n; ++i) {
        float dx = face->p[i].x - face->center.x;
        float dy = face->p[i].y - face->center.y;
        x[i] = face->center.x + c * dx - s * dy;
        y[i] = face->center.y + s * dx + c * dy;
    }
}

static bool on_face(const FaceData *face, float px, float py) {
    if (face->n < 3u) {
        return false;
    }
    float x[k_point_cap];
    float y[k_point_cap];
    placed(face, true, x, y);
    if (clip_inside(x, y, face->n, px, py)) {
        return true;
    }
    float near = coord_near();
    float r2 = near * near;
    for (uint32_t i = 0; i < face->n; ++i) {
        uint32_t j = (i + 1u) % face->n;
        if (clip_dist2_seg(x[i], y[i], x[j], y[j], px, py) < r2) {
            return true;
        }
    }
    return false;
}

static bool near_build(float x, float y) {
    float near = coord_near();
    float r2 = near * near;
    for (uint32_t i = 0; i < s_build_n; ++i) {
        float dx = x - s_build[i].x;
        float dy = y - s_build[i].y;
        if (dx * dx + dy * dy < r2) {
            return true;
        }
    }
    return false;
}

static bool face_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    const FaceData *face = o->data;
    if (face == nullptr || !face->used) {
        return false;
    }
    Coord g;
    float gt = 0.0f;
    if (!coord_ground_hit(ray, &g, &gt)) {
        return false;
    }
    if (!on_face(face, g.x, g.y)) {
        return false;
    }
    if (t != nullptr) {
        *t = gt;
    }
    if (at != nullptr) {
        *at = g;
    }
    return true;
}

static void face_draw(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const FaceData *face = o->data;
    if (face == nullptr || face->n == 0u) {
        return;
    }
    int32_t milli = live ? face->spin.live_milli : face->spin.shown_milli;
    float ang = rad_of(milli);
    bool glow = live ? face->spin.glow : face->spin.shown_glow;
    material_set(face->mat, glow);
    bool hot = table_highlight(o, live);
    float rr = hot ? 0.55f : 0.98f;
    float gg = hot ? 0.82f : 0.96f;
    float bb = hot ? 1.0f : 0.90f;
    uint32_t pn = face->n;
    if (pn > k_point_cap) {
        pn = k_point_cap;
    }
    float xs[k_point_cap];
    float ys[k_point_cap];
    for (uint32_t i = 0; i < pn; ++i) {
        xs[i] = face->p[i].x;
        ys[i] = face->p[i].y;
    }
    float fill_bias = 0.004f + (float)face->serial * 0.00002f;
    shape_draw_polygon(cmd, width, height, xs, ys, pn, face->idx, face->tri_n, true,
                       face->center.x, face->center.y, ang,
                       fill_bias, fill_bias + 0.0015f, fill_bias + 0.003f,
                       0.96f, 0.71f, 0.28f, rr, gg, bb,
                       k_stroke_px, k_mark_px, face->mat);
}

static bool face_advance(WorldObj *o, uint32_t time_ms) {
    FaceData *face = o->data;
    return spin_advance(&face->spin, time_ms, false);
}

static bool face_toggle_spin(WorldObj *o) {
    FaceData *face = o->data;
    if (spin_toggle(&face->spin)) {
        printf("[Engine] Face %u spins.\n", face->serial);
    } else {
        printf("[Engine] Face %u holds.\n", face->serial);
    }
    fflush(stdout);
    return true;
}

static bool face_toggle_glow(WorldObj *o) {
    FaceData *face = o->data;
    face->spin.glow = !face->spin.glow;
    printf("[Material] Face %u glow %s.\n", face->serial, face->spin.glow ? "on" : "off");
    fflush(stdout);
    return true;
}

static bool face_respin(WorldObj *o) {
    FaceData *face = o->data;
    spin_respin(&face->spin);
    table_mark_spun();
    printf("[Engine] Face %u turns again.\n", face->serial);
    fflush(stdout);
    return true;
}

static bool face_remove(WorldObj *o) {
    FaceData *face = o->data;
    o->alive = false;
    face->spin.spinning = false;
    material_set(face->mat, false);
    printf("[Engine] Face %u removed.\n", face->serial);
    fflush(stdout);
    return true;
}

static const char *face_name(const WorldObj *o) {
    const FaceData *face = o->data;
    return face->name;
}

static uint32_t face_vertices(const WorldObj *o) {
    const FaceData *face = o->data;
    return face != nullptr ? face->tri_n * 3u : 0u;
}

static const WorldOps s_ops = {
    .hit         = face_hit,
    .vertices    = face_vertices,
    .draw        = face_draw,
    .advance     = face_advance,
    .toggle_spin = face_toggle_spin,
    .toggle_glow = face_toggle_glow,
    .respin      = face_respin,
    .remove      = face_remove,
    .name        = face_name,
};

const WorldOps *face_ops(void) {
    return &s_ops;
}

void face_reset(void) {
    for (uint32_t i = 0; i < k_face_cap; ++i) {
        s_pool[i] = (FaceData){0};
        material_set(k_mat_face0 + i, false);
    }
    s_serial = 0;
    s_build_n = 0;
    s_shown_build_n = 0;
    s_logged_full = false;
    s_logged_chain = false;
}

void face_self_check(void) {
    clip_self_check();
}

bool face_spawn(Coord at) {
    if (near_build(at.x, at.y)) {
        return false;
    }
    if (used_count() >= k_face_cap && s_build_n == 0u) {
        if (!s_logged_full) {
            fprintf(stderr, "[Engine] Face list is full (%u)\n", k_face_cap);
            s_logged_full = true;
        }
        return false;
    }
    if (s_build_n >= k_point_cap) {
        if (!s_logged_chain) {
            fprintf(stderr, "[Engine] Face chain is full (%u)\n", k_point_cap);
            s_logged_chain = true;
        }
        return false;
    }
    s_build[s_build_n] = at;
    s_build_n++;
    if (s_build_n < k_point_cap) {
        s_logged_chain = false;
    }
    return true;
}

bool face_close(void) {
    if (s_build_n < 3u) {
        return false;
    }
    int slot = alloc_slot();
    if (slot < 0) {
        if (!s_logged_full) {
            fprintf(stderr, "[Engine] Face list is full (%u)\n", k_face_cap);
            s_logged_full = true;
        }
        return false;
    }
    float x[k_point_cap];
    float y[k_point_cap];
    float cx = 0.0f;
    float cy = 0.0f;
    for (uint32_t i = 0; i < s_build_n; ++i) {
        x[i] = s_build[i].x;
        y[i] = s_build[i].y;
        cx += x[i];
        cy += y[i];
    }
    cx /= (float)s_build_n;
    cy /= (float)s_build_n;
    uint16_t idx[k_tri_cap * 3u];
    uint32_t tri_n = 0;
    if (clip_cross(x, y, s_build_n)) {
        printf("[Engine] Face crosses itself. The points stay.\n");
        fflush(stdout);
        return false;
    }
    if (!clip_poly(x, y, s_build_n, idx, &tri_n)) {
        printf("[Engine] Face could not be closed. The points stay.\n");
        fflush(stdout);
        return false;
    }
    FaceData *face = &s_pool[slot];
    *face = (FaceData){0};
    face->used = true;
    face->n = s_build_n;
    face->tri_n = tri_n;
    face->center = (Coord){ cx, cy, s_build[0].z };
    face->spin = spin_start(0);
    face->mat = k_mat_face0 + (uint32_t)slot;
    s_serial++;
    face->serial = s_serial;
    snprintf(face->name, sizeof face->name, "Face %u", face->serial);
    for (uint32_t i = 0; i < s_build_n; ++i) {
        face->p[i] = s_build[i];
    }
    for (uint32_t i = 0; i < tri_n * 3u; ++i) {
        face->idx[i] = idx[i];
    }
    int index = table_add(&s_ops, face, face->center, &face->spin);
    if (index < 0) {
        face->used = false;
        if (!s_logged_full) {
            fprintf(stderr, "[Engine] Face list is full (%u)\n", k_face_cap);
            s_logged_full = true;
        }
        return false;
    }
    table_select_quiet(index);
    s_build_n = 0;
    s_logged_chain = false;
    if (used_count() < k_face_cap) {
        s_logged_full = false;
    }
    table_mark_spun();
    printf("[Engine] Face %u closed at %+.0f,%+.0f. One revolution, then it holds.\n",
           face->serial, (double)cx, (double)cy);
    fflush(stdout);
    return true;
}

bool face_cancel_chain(void) {
    bool had = s_build_n > 0u;
    s_build_n = 0;
    return had;
}

bool face_pop_corner(void) {
    if (s_build_n == 0u) {
        return false;
    }
    s_build_n--;
    s_logged_chain = false;
    printf("[Engine] Corner removed. %u left.\n", s_build_n);
    fflush(stdout);
    return true;
}

static bool chain_differs(void) {
    if (s_build_n != s_shown_build_n) {
        return true;
    }
    for (uint32_t i = 0; i < s_build_n; ++i) {
        if (s_build[i].x != s_shown_build[i].x || s_build[i].y != s_shown_build[i].y ||
            s_build[i].z != s_shown_build[i].z) {
            return true;
        }
    }
    return false;
}

bool face_chain_busy(void) {
    return chain_differs();
}

void face_commit_chain(void) {
    s_shown_build_n = s_build_n;
    for (uint32_t i = 0; i < s_build_n; ++i) {
        s_shown_build[i] = s_build[i];
    }
}

void face_reap(void) {
    bool ref[k_face_cap] = {0};
    int n = table_count();
    for (int i = 0; i < n; ++i) {
        WorldObj *o = table_at(i);
        if (o == nullptr || !o->alive || o->ops != &s_ops || o->data == nullptr) {
            continue;
        }
        FaceData *face = o->data;
        uint32_t slot = face->mat - k_mat_face0;
        if (slot < k_face_cap) {
            ref[slot] = true;
        }
    }
    for (uint32_t i = 0; i < k_face_cap; ++i) {
        if (s_pool[i].used && !ref[i]) {
            s_pool[i].used = false;
            material_set(k_mat_face0 + i, false);
        }
    }
    if (used_count() < k_face_cap) {
        s_logged_full = false;
    }
}

void face_draw_chain(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const Coord *pts = live ? s_build : s_shown_build;
    uint32_t n = live ? s_build_n : s_shown_build_n;
    if (n == 0u) {
        return;
    }
    if (n > k_point_cap) {
        n = k_point_cap;
    }
    float xs[k_point_cap];
    float ys[k_point_cap];
    for (uint32_t i = 0; i < n; ++i) {
        xs[i] = pts[i].x;
        ys[i] = pts[i].y;
    }
    const float r = 0.98f;
    const float g = 0.96f;
    const float b = 0.90f;
    shape_draw_polygon(cmd, width, height, xs, ys, n, nullptr, 0u, false,
                       0.0f, 0.0f, 0.0f, 0.0f, 0.015f, 0.018f,
                       r, g, b, r, g, b, k_stroke_px, k_mark_px, k_mat_none);
}

uint32_t face_chain_count(void) {
    return s_build_n;
}
