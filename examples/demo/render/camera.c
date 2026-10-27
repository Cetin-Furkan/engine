#include "frame/camera.h"
#include "frame/material.h"
#include "input/pointer.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>

// Body-frame increment, then project back onto yaw and pitch so the
// plane's up does not roll. Pitch stops before the look axis lies in
// the plane, where the unproject ray would run along z = 0.

enum : uint32_t {
    k_gap_ms = 1000u,
};

static const float k_pitch_limit = 1.15f;
static const float k_yaw_rate    = 1.15f;
static const float k_pitch_rate  = 0.85f;
static const float k_zoom_rate   = 1.35f;
static const float k_move_speed  = 720.0f;
static const float k_zoom_min    = 0.0001f;
static const float k_zoom_max    = 10000.0f;

typedef struct Vec3 {
    float x;
    float y;
    float z;
} Vec3;

typedef struct Quat {
    float x;
    float y;
    float z;
    float w;
} Quat;

typedef struct Pose {
    float qx;
    float qy;
    float qz;
    float qw;
    float x;
    float y;
    float z;
    float zoom;
} Pose;

static Pose     s_live;
static Pose     s_shown;
static bool     s_bind_live = false;
static uint32_t s_t0 = 0;
static bool     s_have_t0 = false;
static float    s_held_yaw = 0.0f;
static bool     s_logged = false;

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static Vec3 cross3(Vec3 a, Vec3 b) {
    return (Vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static Quat quat_of(const Pose *p) {
    return (Quat){ p->qx, p->qy, p->qz, p->qw };
}

static void quat_store(Pose *p, Quat q) {
    p->qx = q.x;
    p->qy = q.y;
    p->qz = q.z;
    p->qw = q.w;
}

static Quat conjugate(Quat q) {
    return (Quat){ -q.x, -q.y, -q.z, q.w };
}

static Quat mul_q(Quat a, Quat b) {
    return (Quat){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

static Quat normalize_q(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1.0e-8f) {
        return (Quat){ 0.0f, 0.0f, 0.0f, 1.0f };
    }
    float s = 1.0f / n;
    return (Quat){ q.x * s, q.y * s, q.z * s, q.w * s };
}

// Same formula as view_rotate in shaders/camera.inc.
static Vec3 rotate_q(Quat q, Vec3 v) {
    Vec3 u = { q.x, q.y, q.z };
    Vec3 t = cross3(u, v);
    t.x *= 2.0f;
    t.y *= 2.0f;
    t.z *= 2.0f;
    Vec3 c = cross3(u, t);
    return (Vec3){
        v.x + q.w * t.x + c.x,
        v.y + q.w * t.y + c.y,
        v.z + q.w * t.z + c.z,
    };
}

// Columns of the inverse view: camera right, camera up, and the look
// direction. Pan and the click ray both use this, so an arrow and a
// pixel stay on the same axes.
static void view_basis(Quat q, Vec3 *right, Vec3 *up, Vec3 *look) {
    Quat inv = conjugate(q);
    *right = rotate_q(inv, (Vec3){ 1.0f, 0.0f, 0.0f });
    *up = rotate_q(inv, (Vec3){ 0.0f, 1.0f, 0.0f });
    *look = rotate_q(inv, (Vec3){ 0.0f, 0.0f, -1.0f });
}

static Vec3 look_dir(Quat q) {
    Vec3 right;
    Vec3 up;
    Vec3 look;
    view_basis(q, &right, &up, &look);
    (void)right;
    (void)up;
    return look;
}

static Vec3 unit_or(Vec3 v, Vec3 fallback) {
    float n = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (n < 1.0e-8f) {
        return fallback;
    }
    float s = 1.0f / n;
    return (Vec3){ v.x * s, v.y * s, v.z * s };
}

static float pose_pitch(Quat q) {
    Vec3 fwd = look_dir(q);
    return asinf(clampf(fwd.y, -1.0f, 1.0f));
}

static void strip_roll(Pose *p) {
    Quat q = quat_of(p);
    Vec3 fwd = look_dir(q);
    float pitch = asinf(clampf(fwd.y, -1.0f, 1.0f));
    float horiz = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
    float yaw = s_held_yaw;
    if (horiz > 1.0e-4f) {
        yaw = atan2f(fwd.x, -fwd.z);
        s_held_yaw = yaw;
    }
    float hy = yaw * 0.5f;
    float hp = pitch * 0.5f;
    Quat qy = { 0.0f, sinf(hy), 0.0f, cosf(hy) };
    Quat qp = { sinf(hp), 0.0f, 0.0f, cosf(hp) };
    quat_store(p, normalize_q(mul_q(qy, qp)));
}

static bool pose_same(const Pose *a, const Pose *b) {
    return a->qx == b->qx && a->qy == b->qy && a->qz == b->qz && a->qw == b->qw &&
           a->x == b->x && a->y == b->y && a->z == b->z && a->zoom == b->zoom;
}

static const Pose *bound_pose(void) {
    return s_bind_live ? &s_live : &s_shown;
}

static int axis_of(uint32_t positive, uint32_t negative) {
    int p = pointer_key_down(positive) ? 1 : 0;
    int n = pointer_key_down(negative) ? 1 : 0;
    return p - n;
}

static bool shift_down(void) {
    return pointer_key_down(KEY_LEFTSHIFT) || pointer_key_down(KEY_RIGHTSHIFT);
}

// Equals is plus on a US keyboard. Numpad plus is the key the Turkish
// layout prints as plus. Shift+4 is plus on the Turkish Q number row.
static int zoom_axis(void) {
    int inn = (pointer_key_down(KEY_EQUAL) || pointer_key_down(KEY_KPPLUS) ||
               (shift_down() && pointer_key_down(KEY_4)))
                  ? 1
                  : 0;
    int out = (pointer_key_down(KEY_MINUS) || pointer_key_down(KEY_KPMINUS)) ? 1 : 0;
    return inn - out;
}

static void translate_view(float dt, int move_x, int move_y) {
    if (move_x == 0 && move_y == 0) {
        return;
    }
    Vec3 right;
    Vec3 up;
    Vec3 look;
    view_basis(quat_of(&s_live), &right, &up, &look);
    (void)look;
    float scale = k_move_speed * dt / s_live.zoom;
    float sx = (float)move_x * scale;
    float sy = (float)move_y * scale;
    s_live.x += right.x * sx + up.x * sy;
    s_live.y += right.y * sx + up.y * sy;
    s_live.z += right.z * sx + up.z * sy;
}

static void integrate(float dt) {
    int yaw_axis = axis_of(KEY_E, KEY_Q);
    int pitch_axis = axis_of(KEY_C, KEY_V);
    float pitch = pose_pitch(quat_of(&s_live));
    if (pitch >= k_pitch_limit && pitch_axis > 0) {
        pitch_axis = 0;
    }
    if (pitch <= -k_pitch_limit && pitch_axis < 0) {
        pitch_axis = 0;
    }
    if (yaw_axis != 0 || pitch_axis != 0) {
        float ox = (float)pitch_axis * k_pitch_rate * dt;
        float oy = (float)yaw_axis * k_yaw_rate * dt;
        float theta2 = ox * ox + oy * oy;
        Quat dq = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (theta2 > 1.0e-12f) {
            float theta = sqrtf(theta2);
            float s = sinf(theta * 0.5f) / theta;
            dq.x = ox * s;
            dq.y = oy * s;
            dq.z = 0.0f;
            dq.w = cosf(theta * 0.5f);
        }
        quat_store(&s_live, normalize_q(mul_q(dq, quat_of(&s_live))));
        strip_roll(&s_live);
    }

    int zoom = zoom_axis();
    if (zoom != 0) {
        s_live.zoom *= expf((float)zoom * k_zoom_rate * dt);
        s_live.zoom = clampf(s_live.zoom, k_zoom_min, k_zoom_max);
    }

    translate_view(dt, axis_of(KEY_RIGHT, KEY_LEFT), axis_of(KEY_UP, KEY_DOWN));
}

static void check_identity(void) {
    Quat id = { 0.0f, 0.0f, 0.0f, 1.0f };
    Vec3 r = rotate_q(id, (Vec3){ 3.0f, -2.0f, 7.0f });
    float hy = 0.78539816339f;
    Quat yaw = { 0.0f, sinf(hy), 0.0f, cosf(hy) };
    Vec3 x = rotate_q(yaw, (Vec3){ 1.0f, 0.0f, 0.0f });
    Vec3 right;
    Vec3 up;
    Vec3 look;
    view_basis(id, &right, &up, &look);
    bool basis = fabsf(right.x - 1.0f) < 1.0e-4f && fabsf(right.y) < 1.0e-4f &&
                 fabsf(right.z) < 1.0e-4f && fabsf(up.x) < 1.0e-4f &&
                 fabsf(up.y - 1.0f) < 1.0e-4f && fabsf(up.z) < 1.0e-4f &&
                 fabsf(look.x) < 1.0e-4f && fabsf(look.y) < 1.0e-4f &&
                 fabsf(look.z + 1.0f) < 1.0e-4f;
    bool ok = basis && fabsf(r.x - 3.0f) < 1.0e-4f && fabsf(r.y + 2.0f) < 1.0e-4f &&
              fabsf(r.z - 7.0f) < 1.0e-4f && fabsf(x.x) < 1.0e-4f &&
              fabsf(x.y) < 1.0e-4f && fabsf(x.z + 1.0f) < 1.0e-4f;
    float wx = 0.0f;
    float wy = 0.0f;
    camera_unproject(12.0f, -4.0f, 1280u, 720u, &wx, &wy);
    if (!ok || fabsf(wx - 12.0f) > 1.0e-3f || fabsf(wy + 4.0f) > 1.0e-3f) {
        fprintf(stderr, "[Camera Error] Identity view failed the self-check\n");
    }
}

void camera_reset(void) {
    Pose id = {
        .qw   = 1.0f,
        .zoom = 1.0f,
    };
    s_live = id;
    s_shown = id;
    s_bind_live = false;
    s_t0 = 0;
    s_have_t0 = false;
    s_held_yaw = 0.0f;
    check_identity();
    if (!s_logged) {
        printf("[Camera] Quaternion exponential map, exponential zoom, view-plane move.\n");
        fflush(stdout);
        s_logged = true;
    }
}

void camera_bind(bool live) {
    s_bind_live = live;
}

void camera_bound_xy(float *x, float *y) {
    const Pose *p = bound_pose();
    if (x != nullptr) {
        *x = p->x;
    }
    if (y != nullptr) {
        *y = p->y;
    }
}

void view_push_fill(ViewPush *out, uint32_t mat_index) {
    if (out == nullptr) {
        return;
    }
    const Pose *p = bound_pose();
    *out = (ViewPush){
        .materials = material_address(),
        .mat_index = mat_index,
        .qx        = p->qx,
        .qy        = p->qy,
        .qz        = p->qz,
        .qw        = p->qw,
        .cam_x     = p->x,
        .cam_y     = p->y,
        .cam_z     = p->z,
        .zoom      = p->zoom,
    };
}

bool camera_advance(uint32_t time_ms) {
    if (!s_have_t0) {
        s_t0 = time_ms;
        s_have_t0 = true;
        return !pose_same(&s_live, &s_shown);
    }
    uint32_t step = time_ms - s_t0;
    if (step == 0u) {
        return !pose_same(&s_live, &s_shown);
    }
    if (step > k_gap_ms) {
        step = 16u;
    }
    s_t0 = time_ms;
    integrate((float)step / 1000.0f);
    return !pose_same(&s_live, &s_shown);
}

bool camera_busy(void) {
    return !pose_same(&s_live, &s_shown);
}

void camera_commit(void) {
    s_shown = s_live;
}

bool camera_snap_front(void) {
    bool changed = s_live.qx != 0.0f || s_live.qy != 0.0f || s_live.qz != 0.0f ||
                   s_live.qw != 1.0f || s_live.z != 0.0f;
    s_live.qx = 0.0f;
    s_live.qy = 0.0f;
    s_live.qz = 0.0f;
    s_live.qw = 1.0f;
    s_live.z = 0.0f;
    s_held_yaw = 0.0f;
    if (!changed) {
        return false;
    }
    printf("[Camera] Front view.\n");
    fflush(stdout);
    return true;
}

bool camera_motion_down(void) {
    return pointer_key_down(KEY_LEFT) || pointer_key_down(KEY_RIGHT) ||
           pointer_key_down(KEY_UP) || pointer_key_down(KEY_DOWN) ||
           pointer_key_down(KEY_Q) || pointer_key_down(KEY_E) ||
           pointer_key_down(KEY_C) || pointer_key_down(KEY_V) ||
           zoom_axis() != 0;
}

float camera_zoom_shown(void) {
    float z = s_shown.zoom;
    if (z < k_zoom_min) {
        return k_zoom_min;
    }
    return z;
}

// Far enough on the camera side that a pan across the plane still starts
// in front of the meshes. Float32 at this magnitude still resolves a pixel.
static const float k_ray_near = 65536.0f;

void camera_click_ray(float screen_x, float screen_y, CameraRay *out) {
    if (out == nullptr) {
        return;
    }
    const Pose *p = &s_shown;
    float zoom = p->zoom;
    if (zoom < k_zoom_min) {
        zoom = k_zoom_min;
    }
    float sx = screen_x / zoom;
    float sy = screen_y / zoom;
    Vec3 right;
    Vec3 up;
    Vec3 look;
    view_basis(quat_of(p), &right, &up, &look);
    look = unit_or(look, (Vec3){ 0.0f, 0.0f, -1.0f });
    CameraRay ray = {
        .ox = p->x + sx * right.x + sy * up.x - look.x * k_ray_near,
        .oy = p->y + sx * right.y + sy * up.y - look.y * k_ray_near,
        .oz = p->z + sx * right.z + sy * up.z - look.z * k_ray_near,
        .dx = look.x,
        .dy = look.y,
        .dz = look.z,
    };
    if (fabsf(look.z) > 1.0e-4f) {
        float t = -ray.oz / look.z;
        ray.plane_x = ray.ox + t * look.x;
        ray.plane_y = ray.oy + t * look.y;
    } else {
        ray.plane_x = p->x + sx * right.x + sy * up.x;
        ray.plane_y = p->y + sx * right.y + sy * up.y;
    }
    *out = ray;
}

void camera_unproject(float ox, float oy, uint32_t width, uint32_t height,
                      float *wx, float *wy) {
    if (wx == nullptr || wy == nullptr) {
        return;
    }
    (void)width;
    (void)height;
    CameraRay ray;
    camera_click_ray(ox, oy, &ray);
    *wx = ray.plane_x;
    *wy = ray.plane_y;
}
