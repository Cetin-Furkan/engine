#include "frame/camera.h"
#include "frame/material.h"
#include "frame/policy.h"
#include "input/pointer.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>

// Yaw and pitch are the pose. The quaternion is Ry(yaw) * Rx(-pitch).
// Positive pitch raises the look direction. The view quaternion rotates
// world into camera, and a positive X rotation points camera -Z downward,
// so the stored X half-angle is the negation of the pitch angle. Pitch
// stops before the look axis lies in the plane, where the unproject ray
// would run along z = 0.

enum : uint32_t {
    k_gap_ms = 1000u,
};

static const float k_pitch_limit = 1.15f;
static const float k_yaw_rate    = 1.15f;
static const float k_pitch_rate  = 0.85f;
static const float k_zoom_rate   = 1.35f;
static const float k_move_speed  = 720.0f;


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
static float    s_held_pitch = 0.0f;
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

// q rotates world into camera. Rx(+pitch) sends the look direction down,
// so the stored X rotation is the opposite of the pitch angle.
static void store_yaw_pitch(Pose *p, float yaw, float pitch) {
    float hy = yaw * 0.5f;
    float hp = -pitch * 0.5f;
    Quat qy = { 0.0f, sinf(hy), 0.0f, cosf(hy) };
    Quat qp = { sinf(hp), 0.0f, 0.0f, cosf(hp) };
    quat_store(p, normalize_q(mul_q(qy, qp)));
}

static float wrap_pi(float a) {
    const float pi = 3.14159265f;
    const float turn = 6.28318531f;
    if (a > pi || a < -pi) {
        a = remainderf(a, turn);
    }
    return a;
}

static bool pose_same(const Pose *a, const Pose *b) {
    return a->qx == b->qx && a->qy == b->qy && a->qz == b->qz && a->qw == b->qw &&
           a->x == b->x && a->y == b->y && a->z == b->z && a->zoom == b->zoom;
}

static const Pose *bound_pose(void) {
    return s_bind_live ? &s_live : &s_shown;
}

static bool shift_down(void) {
    return pointer_key_down(KEY_LEFTSHIFT) || pointer_key_down(KEY_RIGHTSHIFT);
}

static bool cam_key_down(uint16_t key, void *user) {
    (void)user;
    return pointer_key_down(key);
}

typedef struct CamAxes {
    int pan_x;
    int pan_y;
    int yaw;
    int pitch;
    int zoom;
} CamAxes;

// lock_2d_quat forces the yaw and pitch axes to zero even when a key
// is still bound. The permission mask and the pose lock are separate:
// granting yaw on a flat profile does not tilt the plane.
static CamAxes cam_axes(void) {
    const WorkspacePolicy *p = policy_active();
    bool shift = shift_down();
    bool flat = p->lock_2d_quat;
    CamAxes axes = {0};
    if (!flat) {
        axes.yaw = policy_held_axis(p, ACT_YAW_CW, ACT_YAW_CCW, shift, cam_key_down, nullptr);
        axes.pitch = policy_held_axis(p, ACT_PITCH_UP, ACT_PITCH_DOWN, shift, cam_key_down, nullptr);
    }
    axes.pan_x = policy_held_axis(p, ACT_PAN_RIGHT, ACT_PAN_LEFT, shift, cam_key_down, nullptr);
    axes.pan_y = policy_held_axis(p, ACT_PAN_UP, ACT_PAN_DOWN, shift, cam_key_down, nullptr);
    axes.zoom = policy_held_axis(p, ACT_ZOOM_IN, ACT_ZOOM_OUT, shift, cam_key_down, nullptr);
    return axes;
}

static void hold_flat(void) {
    s_live.qx = 0.0f;
    s_live.qy = 0.0f;
    s_live.qz = 0.0f;
    s_live.qw = 1.0f;
    s_live.z = 0.0f;
    s_held_yaw = 0.0f;
    s_held_pitch = 0.0f;
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
    if (policy_active()->lock_2d_quat) {
        hold_flat();
    }
    CamAxes axes = cam_axes();
    if (axes.yaw != 0 || axes.pitch != 0) {
        s_held_yaw = wrap_pi(s_held_yaw + (float)axes.yaw * k_yaw_rate * dt);
        s_held_pitch = clampf(s_held_pitch + (float)axes.pitch * k_pitch_rate * dt,
                              -k_pitch_limit, k_pitch_limit);
        store_yaw_pitch(&s_live, s_held_yaw, s_held_pitch);
    }

    if (axes.zoom != 0) {
        s_live.zoom *= expf((float)axes.zoom * k_zoom_rate * dt);
        s_live.zoom = clampf(s_live.zoom, k_camera_zoom_min, k_camera_zoom_max);
    }

    translate_view(dt, axes.pan_x, axes.pan_y);
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
    Pose pitched = { .qw = 1.0f, .zoom = 1.0f };
    store_yaw_pitch(&pitched, 0.0f, 0.40f);
    Vec3 look_up = look_dir(quat_of(&pitched));
    store_yaw_pitch(&pitched, 0.0f, -0.40f);
    Vec3 look_down = look_dir(quat_of(&pitched));
    store_yaw_pitch(&pitched, 0.70f, 0.40f);
    Vec3 look_both = look_dir(quat_of(&pitched));
    bool pitched_ok = look_up.y > 0.30f && look_down.y < -0.30f &&
                      look_both.y > 0.25f && look_both.x > 0.50f;
    bool ok = basis && pitched_ok && fabsf(r.x - 3.0f) < 1.0e-4f &&
              fabsf(r.y + 2.0f) < 1.0e-4f && fabsf(r.z - 7.0f) < 1.0e-4f &&
              fabsf(x.x) < 1.0e-4f && fabsf(x.y) < 1.0e-4f &&
              fabsf(x.z + 1.0f) < 1.0e-4f;
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
    s_held_pitch = 0.0f;
    check_identity();
    if (!s_logged) {
        printf("[Camera] Yaw and pitch on the view, exponential zoom, view-plane move.\n");
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
    s_held_pitch = 0.0f;
    if (!changed) {
        return false;
    }
    printf("[Camera] Front view.\n");
    fflush(stdout);
    return true;
}

bool camera_motion_down(void) {
    CamAxes axes = cam_axes();
    return axes.pan_x != 0 || axes.pan_y != 0 || axes.yaw != 0 ||
           axes.pitch != 0 || axes.zoom != 0;
}

float camera_zoom_shown(void) {
    float z = s_shown.zoom;
    if (z < k_camera_zoom_min) {
        return k_camera_zoom_min;
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
    if (zoom < k_camera_zoom_min) {
        zoom = k_camera_zoom_min;
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
