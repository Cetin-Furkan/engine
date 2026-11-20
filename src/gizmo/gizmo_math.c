#include "gizmo/gizmo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static constexpr float k_pi             = 3.14159265358979323846f;
static constexpr float k_two_pi         = 6.28318530717958647692f;
static constexpr float k_gizmo_px       = 96.0f;
static constexpr float k_snap_translate = 25.0f;
static constexpr float k_snap_rot_rad   = 15.0f * (3.14159265358979323846f / 180.0f);
static constexpr float k_snap_scale     = 0.25f;

static inline float v3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void v3_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static inline float v3_len(const float a[3]) {
    return sqrtf(v3_dot(a, a));
}

static inline bool v3_norm(float a[3]) {
    float l = v3_len(a);
    if (l < 1.0e-7f) {
        return false;
    }
    float inv = 1.0f / l;
    a[0] *= inv;
    a[1] *= inv;
    a[2] *= inv;
    return true;
}

static inline uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8u) | ((uint32_t)b << 16u) | ((uint32_t)a << 24u);
}

static bool ray_plane_hit(const GizmoRay *ray, const float plane_p[3], const float plane_n[3],
                          float out_hit[3]) {
    float denom = v3_dot(ray->dir, plane_n);
    if (fabsf(denom) < 1.0e-5f) {
        return false;
    }
    float rel[3] = {
        plane_p[0] - ray->origin[0],
        plane_p[1] - ray->origin[1],
        plane_p[2] - ray->origin[2],
    };
    float t = v3_dot(rel, plane_n) / denom;
    if (!isfinite(t) || t < 0.0f || t > 1.0e6f) {
        return false;
    }
    out_hit[0] = ray->origin[0] + ray->dir[0] * t;
    out_hit[1] = ray->origin[1] + ray->dir[1] * t;
    out_hit[2] = ray->origin[2] + ray->dir[2] * t;
    return true;
}

// Closest points between ray (ro + t*rd) and line (p0 + s*axis).
// Returns ray-to-line distance and outputs axis parameter s.
static float ray_axis_closest(const GizmoRay *ray, const float p0[3], const float axis[3],
                              float *out_s, float *out_t) {
    float w0[3] = {
        ray->origin[0] - p0[0],
        ray->origin[1] - p0[1],
        ray->origin[2] - p0[2],
    };
    float a = v3_dot(ray->dir, ray->dir);
    float b = v3_dot(ray->dir, axis);
    float c = v3_dot(axis, axis);
    float d = v3_dot(ray->dir, w0);
    float e = v3_dot(axis, w0);
    float denom = a * c - b * b;

    float t = 0.0f;
    float s = 0.0f;
    if (fabsf(denom) < 1.0e-6f) {
        s = e / fmaxf(c, 1.0e-6f);
    } else {
        t = (b * e - c * d) / denom;
        s = (a * e - b * d) / denom;
    }
    if (out_s != nullptr) *out_s = s;
    if (out_t != nullptr) *out_t = t;

    float diff[3] = {
        (ray->origin[0] + ray->dir[0] * t) - (p0[0] + axis[0] * s),
        (ray->origin[1] + ray->dir[1] * t) - (p0[1] + axis[1] * s),
        (ray->origin[2] + ray->dir[2] * t) - (p0[2] + axis[2] * s),
    };
    return v3_len(diff);
}

static float ray_point_dist(const GizmoRay *ray, const float p[3], float *out_t) {
    float rel[3] = {
        p[0] - ray->origin[0],
        p[1] - ray->origin[1],
        p[2] - ray->origin[2],
    };
    float t = v3_dot(rel, ray->dir);
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (out_t != nullptr) *out_t = t;
    float diff[3] = {
        (ray->origin[0] + ray->dir[0] * t) - p[0],
        (ray->origin[1] + ray->dir[1] * t) - p[1],
        (ray->origin[2] + ray->dir[2] * t) - p[2],
    };
    return v3_len(diff);
}

void gizmo_state_init(GizmoState *st) {
    if (st == nullptr) {
        return;
    }
    memset(st, 0, sizeof *st);
    st->mode       = GIZMO_MODE_TRANSLATE;
    st->space      = GIZMO_SPACE_WORLD;
    st->hovered    = GIZMO_HANDLE_NONE;
    st->active     = GIZMO_HANDLE_NONE;
    st->debug_draw = false;
}

void gizmo_set_mode(GizmoState *st, GizmoMode mode) {
    if (st == nullptr || st->dragging) {
        return;
    }
    st->mode    = mode;
    st->hovered = GIZMO_HANDLE_NONE;
}

void gizmo_cycle_mode(GizmoState *st) {
    if (st == nullptr || st->dragging) {
        return;
    }
    switch (st->mode) {
    case GIZMO_MODE_TRANSLATE: st->mode = GIZMO_MODE_ROTATE;    break;
    case GIZMO_MODE_ROTATE:    st->mode = GIZMO_MODE_SCALE;     break;
    case GIZMO_MODE_SCALE:     st->mode = GIZMO_MODE_TRANSLATE; break;
    case GIZMO_MODE_NONE:
    default:                   st->mode = GIZMO_MODE_TRANSLATE; break;
    }
    st->hovered = GIZMO_HANDLE_NONE;
}

void gizmo_toggle_space(GizmoState *st) {
    if (st == nullptr || st->dragging) {
        return;
    }
    st->space = (st->space == GIZMO_SPACE_WORLD) ? GIZMO_SPACE_LOCAL : GIZMO_SPACE_WORLD;
    st->hovered = GIZMO_HANDLE_NONE;
}

bool gizmo_toggle_debug_draw(GizmoState *st) {
    if (st == nullptr) {
        return false;
    }
    st->debug_draw = !st->debug_draw;
    return true;
}

const char *gizmo_mode_name(GizmoMode mode) {
    switch (mode) {
    case GIZMO_MODE_TRANSLATE: return "Move";
    case GIZMO_MODE_ROTATE:    return "Rotate";
    case GIZMO_MODE_SCALE:     return "Scale";
    case GIZMO_MODE_NONE:
    default:                   return "Off";
    }
}

const char *gizmo_space_name(GizmoSpace space) {
    return (space == GIZMO_SPACE_LOCAL) ? "Local" : "World";
}

void gizmo_camera_fill(GizmoCamera *out, const float q[4], const float cam[3],
                       float zoom, float persp) {
    if (out == nullptr) {
        return;
    }
    memcpy(out->q,   q,   4u * sizeof(float));
    memcpy(out->cam, cam, 3u * sizeof(float));
    out->zoom  = fmaxf(zoom, 1.0e-4f);
    out->persp = persp;

    float qc[4];
    ecs_quat_conj(q, qc);
    const float view_back[3] = { 0.0f, 0.0f, 1.0f };
    float back_w[3];
    ecs_quat_rotate_vec3(qc, view_back, back_w);
    (void)v3_norm(back_w);

    out->fwd_w[0] = -back_w[0];
    out->fwd_w[1] = -back_w[1];
    out->fwd_w[2] = -back_w[2];

    float eye_dist = persp > 1.0e-7f ? (1.0f / persp) : 4096.0f;
    out->eye_w[0] = cam[0] + back_w[0] * eye_dist;
    out->eye_w[1] = cam[1] + back_w[1] * eye_dist;
    out->eye_w[2] = cam[2] + back_w[2] * eye_dist;
}

float gizmo_compute_scale(const GizmoCamera *cam, const float pivot_w[3], float target_px) {
    if (cam == nullptr || pivot_w == nullptr) {
        return 100.0f;
    }
    float rel[3] = {
        pivot_w[0] - cam->cam[0],
        pivot_w[1] - cam->cam[1],
        pivot_w[2] - cam->cam[2],
    };
    float vp[3];
    ecs_quat_rotate_vec3(cam->q, rel, vp);

    float w_clip = (cam->persp > 1.0e-7f) ? fmaxf(1.0f - vp[2] * cam->persp, 0.12f) : 1.0f;
    float s = (target_px * w_clip) / fmaxf(cam->zoom, 1.0e-4f);
    return fminf(fmaxf(s, 4.0f), 12000.0f);
}

void gizmo_basis_for_entity(const EcsWorld *w, EcsEntity e, GizmoSpace space,
                            float out_pivot[3], float out_axes[3][3]) {
    out_pivot[0] = 0.0f; out_pivot[1] = 0.0f; out_pivot[2] = 0.0f;
    out_axes[0][0] = 1.0f; out_axes[0][1] = 0.0f; out_axes[0][2] = 0.0f;
    out_axes[1][0] = 0.0f; out_axes[1][1] = 1.0f; out_axes[1][2] = 0.0f;
    out_axes[2][0] = 0.0f; out_axes[2][1] = 0.0f; out_axes[2][2] = 1.0f;

    EcsTrs trs = {0};
    if (!ecs_get_world_trs(w, e, &trs)) {
        return;
    }
    memcpy(out_pivot, trs.pos, 3u * sizeof(float));
    if (space == GIZMO_SPACE_LOCAL) {
        float m3[9];
        ecs_quat_to_mat3(trs.rot, m3);
        out_axes[0][0] = m3[0]; out_axes[0][1] = m3[3]; out_axes[0][2] = m3[6];
        out_axes[1][0] = m3[1]; out_axes[1][1] = m3[4]; out_axes[1][2] = m3[7];
        out_axes[2][0] = m3[2]; out_axes[2][1] = m3[5]; out_axes[2][2] = m3[8];
    }
}

GizmoHandle gizmo_pick_handle(const GizmoState *st, const EcsWorld *w, EcsEntity e,
                              const GizmoCamera *cam, const GizmoRay *ray) {
    if (st == nullptr || st->mode == GIZMO_MODE_NONE || !ecs_alive(w, e) ||
        cam == nullptr || ray == nullptr) {
        return GIZMO_HANDLE_NONE;
    }
    float pivot[3];
    float axes[3][3];
    GizmoSpace use_space = (st->mode == GIZMO_MODE_SCALE) ? GIZMO_SPACE_LOCAL : st->space;
    gizmo_basis_for_entity(w, e, use_space, pivot, axes);
    float sc = gizmo_compute_scale(cam, pivot, k_gizmo_px);

    GizmoHandle best   = GIZMO_HANDLE_NONE;
    float       best_t = 1.0e30f;

    // 1. Center handle (free screen-plane translate / uniform scale)
    if (st->mode == GIZMO_MODE_TRANSLATE || st->mode == GIZMO_MODE_SCALE) {
        float ct = 0.0f;
        float cd = ray_point_dist(ray, pivot, &ct);
        if (cd <= sc * 0.16f) {
            return GIZMO_HANDLE_CENTER;
        }
    }

    if (st->mode == GIZMO_MODE_TRANSLATE || st->mode == GIZMO_MODE_SCALE) {
        // 2. Planar handles for Translate (XY, XZ, YZ)
        if (st->mode == GIZMO_MODE_TRANSLATE) {
            static constexpr int k_planes[3][3] = {
                { 0, 1, 2 }, // XY (normal Z)
                { 0, 2, 1 }, // XZ (normal Y)
                { 1, 2, 0 }, // YZ (normal X)
            };
            static constexpr GizmoHandle k_ph[3] = {
                GIZMO_HANDLE_XY, GIZMO_HANDLE_XZ, GIZMO_HANDLE_YZ,
            };
            for (int p = 0; p < 3; ++p) {
                float hit[3];
                if (!ray_plane_hit(ray, pivot, axes[k_planes[p][2]], hit)) {
                    continue;
                }
                float rel[3] = { hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2] };
                float u = v3_dot(rel, axes[k_planes[p][0]]) / sc;
                float v = v3_dot(rel, axes[k_planes[p][1]]) / sc;
                if (u >= 0.22f && u <= 0.50f && v >= 0.22f && v <= 0.50f) {
                    float d_ray[3] = {
                        hit[0] - ray->origin[0], hit[1] - ray->origin[1], hit[2] - ray->origin[2],
                    };
                    float t = v3_dot(d_ray, ray->dir);
                    if (t < best_t) {
                        best_t = t;
                        best   = k_ph[p];
                    }
                }
            }
        }

        // 3. Axis shafts (X, Y, Z)
        const float pick_r = sc * 0.12f;
        for (int a = 0; a < 3; ++a) {
            float s = 0.0f;
            float t = 0.0f;
            float d = ray_axis_closest(ray, pivot, axes[a], &s, &t);
            if (s >= sc * 0.14f && s <= sc * 1.15f && d <= pick_r && t < best_t) {
                best_t = t;
                best   = (GizmoHandle)(GIZMO_HANDLE_X + a);
            }
        }
        return best;
    }

    // Rotate mode: 3D torus ring annulus hit test per axis (X, Y, Z) + outer screen roll ring
    const float ring_r = sc * 0.92f;
    const float band   = sc * 0.13f;
    for (int a = 0; a < 3; ++a) {
        float hit[3];
        if (!ray_plane_hit(ray, pivot, axes[a], hit)) {
            continue;
        }
        float rel[3] = { hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2] };
        float r = v3_len(rel);
        if (fabsf(r - ring_r) <= band) {
            float d_ray[3] = {
                hit[0] - ray->origin[0], hit[1] - ray->origin[1], hit[2] - ray->origin[2],
            };
            float t = v3_dot(d_ray, ray->dir);
            if (t < best_t) {
                best_t = t;
                best   = (GizmoHandle)(GIZMO_HANDLE_X + a);
            }
        }
    }
    if (best == GIZMO_HANDLE_NONE) {
        float hit[3];
        if (ray_plane_hit(ray, pivot, cam->fwd_w, hit)) {
            float rel[3] = { hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2] };
            float r = v3_len(rel);
            if (fabsf(r - sc * 1.10f) <= band) {
                best = GIZMO_HANDLE_CENTER;
            }
        }
    }
    return best;
}

bool gizmo_hover(GizmoState *st, const EcsWorld *w, EcsEntity e,
                 const GizmoCamera *cam, const GizmoRay *ray) {
    if (st == nullptr || st->dragging) {
        return false;
    }
    GizmoHandle h = gizmo_pick_handle(st, w, e, cam, ray);
    bool changed = (h != st->hovered);
    st->hovered = h;
    return changed;
}

static float ring_angle_on_plane(const float pivot[3], const float axis[3],
                                 const float hit[3]) {
    float rel[3] = { hit[0] - pivot[0], hit[1] - pivot[1], hit[2] - pivot[2] };
    float ref[3] = { 1.0f, 0.0f, 0.0f };
    if (fabsf(v3_dot(axis, ref)) > 0.90f) {
        ref[0] = 0.0f; ref[1] = 1.0f; ref[2] = 0.0f;
    }
    float u[3], v[3];
    v3_cross(axis, ref, u);
    (void)v3_norm(u);
    v3_cross(axis, u, v);
    (void)v3_norm(v);
    return atan2f(v3_dot(rel, v), v3_dot(rel, u));
}

bool gizmo_begin_drag(GizmoState *st, const EcsWorld *w, EcsEntity e,
                      const GizmoCamera *cam, const GizmoRay *ray) {
    if (st == nullptr || !ecs_alive(w, e) || cam == nullptr || ray == nullptr) {
        return false;
    }
    GizmoHandle h = gizmo_pick_handle(st, w, e, cam, ray);
    if (h == GIZMO_HANDLE_NONE) {
        return false;
    }

    EcsTrs trs = {0};
    if (!ecs_get_world_trs(w, e, &trs)) {
        return false;
    }
    GizmoSpace use_space = (st->mode == GIZMO_MODE_SCALE) ? GIZMO_SPACE_LOCAL : st->space;
    gizmo_basis_for_entity(w, e, use_space, st->start_pivot, st->start_axes);

    st->active           = h;
    st->hovered          = h;
    st->drag_entity      = e;
    st->dragging         = true;
    st->drag_world_scale = gizmo_compute_scale(cam, st->start_pivot, k_gizmo_px);
    memcpy(st->start_pos,   trs.pos,   sizeof st->start_pos);
    memcpy(st->start_rot,   trs.rot,   sizeof st->start_rot);
    memcpy(st->start_scale, trs.scale, sizeof st->start_scale);
    st->drag_start_angle = 0.0f;
    st->drag_curr_angle  = 0.0f;

    if (st->mode == GIZMO_MODE_TRANSLATE || st->mode == GIZMO_MODE_SCALE) {
        if (h == GIZMO_HANDLE_X || h == GIZMO_HANDLE_Y || h == GIZMO_HANDLE_Z) {
            int ai = (int)(h - GIZMO_HANDLE_X);
            float s0 = 0.0f;
            (void)ray_axis_closest(ray, st->start_pivot, st->start_axes[ai], &s0, nullptr);
            st->drag_axis_t0 = s0;
        } else {
            if (h == GIZMO_HANDLE_XY)      memcpy(st->drag_plane_n, st->start_axes[2], 12u);
            else if (h == GIZMO_HANDLE_XZ) memcpy(st->drag_plane_n, st->start_axes[1], 12u);
            else if (h == GIZMO_HANDLE_YZ) memcpy(st->drag_plane_n, st->start_axes[0], 12u);
            else                           memcpy(st->drag_plane_n, cam->fwd_w,        12u);

            if (!ray_plane_hit(ray, st->start_pivot, st->drag_plane_n, st->drag_start_hit)) {
                memcpy(st->drag_start_hit, st->start_pivot, 12u);
            }
        }
    } else if (st->mode == GIZMO_MODE_ROTATE) {
        if (h == GIZMO_HANDLE_X || h == GIZMO_HANDLE_Y || h == GIZMO_HANDLE_Z) {
            memcpy(st->drag_plane_n, st->start_axes[h - GIZMO_HANDLE_X], 12u);
        } else {
            st->drag_plane_n[0] = -cam->fwd_w[0];
            st->drag_plane_n[1] = -cam->fwd_w[1];
            st->drag_plane_n[2] = -cam->fwd_w[2];
        }
        float hit[3];
        if (ray_plane_hit(ray, st->start_pivot, st->drag_plane_n, hit)) {
            st->drag_start_angle = ring_angle_on_plane(st->start_pivot, st->drag_plane_n, hit);
            st->drag_curr_angle  = st->drag_start_angle;
        }
    }
    return true;
}

bool gizmo_update_drag(GizmoState *st, EcsWorld *w,
                       const GizmoCamera *cam, const GizmoRay *ray,
                       bool snap_enabled) {
    if (st == nullptr || !st->dragging || !ecs_alive(w, st->drag_entity) ||
        cam == nullptr || ray == nullptr) {
        return false;
    }
    st->snap_active = snap_enabled;
    GizmoHandle h   = st->active;

    if (st->mode == GIZMO_MODE_TRANSLATE) {
        float new_pos[3] = { st->start_pos[0], st->start_pos[1], st->start_pos[2] };
        if (h == GIZMO_HANDLE_X || h == GIZMO_HANDLE_Y || h == GIZMO_HANDLE_Z) {
            int ai = (int)(h - GIZMO_HANDLE_X);
            float s = 0.0f;
            (void)ray_axis_closest(ray, st->start_pivot, st->start_axes[ai], &s, nullptr);
            float ds = s - st->drag_axis_t0;
            if (snap_enabled) {
                ds = roundf(ds / k_snap_translate) * k_snap_translate;
            }
            new_pos[0] += st->start_axes[ai][0] * ds;
            new_pos[1] += st->start_axes[ai][1] * ds;
            new_pos[2] += st->start_axes[ai][2] * ds;
        } else {
            float hit[3];
            if (!ray_plane_hit(ray, st->start_pivot, st->drag_plane_n, hit)) {
                return false;
            }
            float d[3] = {
                hit[0] - st->drag_start_hit[0],
                hit[1] - st->drag_start_hit[1],
                hit[2] - st->drag_start_hit[2],
            };
            if (snap_enabled) {
                d[0] = roundf(d[0] / k_snap_translate) * k_snap_translate;
                d[1] = roundf(d[1] / k_snap_translate) * k_snap_translate;
                d[2] = roundf(d[2] / k_snap_translate) * k_snap_translate;
            }
            new_pos[0] += d[0];
            new_pos[1] += d[1];
            new_pos[2] += d[2];
        }
        ecs_set_world_pos(w, st->drag_entity, new_pos[0], new_pos[1], new_pos[2]);
        ecs_propagate_transforms(w);
        return true;
    }

    if (st->mode == GIZMO_MODE_ROTATE) {
        float hit[3];
        if (!ray_plane_hit(ray, st->start_pivot, st->drag_plane_n, hit)) {
            return false;
        }
        float ang = ring_angle_on_plane(st->start_pivot, st->drag_plane_n, hit);
        float delta = ang - st->drag_start_angle;
        if (delta > k_pi)  delta -= k_two_pi;
        if (delta < -k_pi) delta += k_two_pi;
        if (snap_enabled) {
            delta = roundf(delta / k_snap_rot_rad) * k_snap_rot_rad;
        }
        st->drag_curr_angle = st->drag_start_angle + delta;

        float q_delta[4];
        float q_new[4];
        ecs_quat_from_axis_angle(st->drag_plane_n, delta, q_delta);
        ecs_quat_mul(q_delta, st->start_rot, q_new);
        ecs_quat_norm(q_new);
        ecs_set_world_rot(w, st->drag_entity, q_new);
        ecs_propagate_transforms(w);
        return true;
    }

    if (st->mode == GIZMO_MODE_SCALE) {
        float new_scl[3] = { st->start_scale[0], st->start_scale[1], st->start_scale[2] };
        if (h == GIZMO_HANDLE_X || h == GIZMO_HANDLE_Y || h == GIZMO_HANDLE_Z) {
            int ai = (int)(h - GIZMO_HANDLE_X);
            float s = 0.0f;
            (void)ray_axis_closest(ray, st->start_pivot, st->start_axes[ai], &s, nullptr);
            float ratio = 1.0f + (s - st->drag_axis_t0) / fmaxf(st->drag_world_scale, 1.0f);
            ratio = fminf(fmaxf(ratio, 0.10f), 8.0f);
            float target = st->start_scale[ai] * ratio;
            if (snap_enabled) {
                target = fmaxf(roundf(target / k_snap_scale) * k_snap_scale, 0.25f);
            }
            new_scl[ai] = fminf(fmaxf(target, 0.10f), 8.0f);
        } else {
            float hit[3];
            if (!ray_plane_hit(ray, st->start_pivot, st->drag_plane_n, hit)) {
                return false;
            }
            float d0 = v3_len((const float[3]){
                st->drag_start_hit[0] - st->start_pivot[0],
                st->drag_start_hit[1] - st->start_pivot[1],
                st->drag_start_hit[2] - st->start_pivot[2],
            });
            float d1 = v3_len((const float[3]){
                hit[0] - st->start_pivot[0],
                hit[1] - st->start_pivot[1],
                hit[2] - st->start_pivot[2],
            });
            float ratio = 1.0f + (d1 - d0) / fmaxf(st->drag_world_scale, 1.0f);
            ratio = fminf(fmaxf(ratio, 0.10f), 8.0f);
            for (int i = 0; i < 3; ++i) {
                float target = st->start_scale[i] * ratio;
                if (snap_enabled) {
                    target = fmaxf(roundf(target / k_snap_scale) * k_snap_scale, 0.25f);
                }
                new_scl[i] = fminf(fmaxf(target, 0.10f), 8.0f);
            }
        }
        ecs_set_world_scale(w, st->drag_entity, new_scl[0], new_scl[1], new_scl[2]);
        ecs_propagate_transforms(w);
        return true;
    }

    return false;
}

void gizmo_end_drag(GizmoState *st) {
    if (st == nullptr) {
        return;
    }
    st->dragging    = false;
    st->active      = GIZMO_HANDLE_NONE;
    st->drag_entity = ECS_NULL_ENTITY;
    st->snap_active = false;
}

// ----------------------------------------------------------------------------
// 3D Geometry Emitter (Camera-Facing Ribbons, Cones, Cubes, Torus Rings, AABBs)
// ----------------------------------------------------------------------------

typedef struct VertBuf {
    GizmoVertex *v;
    uint32_t     count;
    uint32_t     cap;
} VertBuf;

static void emit_tri(VertBuf *vb, const float a[3], const float b[3], const float c[3], uint32_t rgba) {
    if (vb->count + 3u > vb->cap) {
        return;
    }
    vb->v[vb->count++] = (GizmoVertex){ a[0], a[1], a[2], rgba };
    vb->v[vb->count++] = (GizmoVertex){ b[0], b[1], b[2], rgba };
    vb->v[vb->count++] = (GizmoVertex){ c[0], c[1], c[2], rgba };
}

static void emit_quad(VertBuf *vb, const float a[3], const float b[3],
                      const float c[3], const float d[3], uint32_t rgba) {
    emit_tri(vb, a, b, c, rgba);
    emit_tri(vb, a, c, d, rgba);
}

static void emit_ribbon(VertBuf *vb, const GizmoCamera *cam,
                        const float a[3], const float b[3], float half_w, uint32_t rgba) {
    float dir[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    if (!v3_norm(dir)) {
        return;
    }
    float mid[3] = { 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]) };
    float to_cam[3] = {
        cam->eye_w[0] - mid[0], cam->eye_w[1] - mid[1], cam->eye_w[2] - mid[2],
    };
    if (!v3_norm(to_cam)) {
        to_cam[0] = -cam->fwd_w[0]; to_cam[1] = -cam->fwd_w[1]; to_cam[2] = -cam->fwd_w[2];
    }
    float side[3];
    v3_cross(dir, to_cam, side);
    if (!v3_norm(side)) {
        float fallback[3] = { 0.0f, 0.0f, 1.0f };
        if (fabsf(dir[2]) > 0.9f) { fallback[0] = 1.0f; fallback[2] = 0.0f; }
        v3_cross(dir, fallback, side);
        (void)v3_norm(side);
    }
    float ox = side[0] * half_w;
    float oy = side[1] * half_w;
    float oz = side[2] * half_w;
    float p0[3] = { a[0] - ox, a[1] - oy, a[2] - oz };
    float p1[3] = { b[0] - ox, b[1] - oy, b[2] - oz };
    float p2[3] = { b[0] + ox, b[1] + oy, b[2] + oz };
    float p3[3] = { a[0] + ox, a[1] + oy, a[2] + oz };
    emit_quad(vb, p0, p1, p2, p3, rgba);
}

static void emit_cone(VertBuf *vb, const float base[3], const float axis[3],
                      float length, float radius, uint32_t rgba_lit, uint32_t rgba_dark) {
    float ref[3] = { 1.0f, 0.0f, 0.0f };
    if (fabsf(v3_dot(axis, ref)) > 0.9f) { ref[0] = 0.0f; ref[1] = 1.0f; }
    float u[3], v[3];
    v3_cross(axis, ref, u); (void)v3_norm(u);
    v3_cross(axis, u, v);   (void)v3_norm(v);

    float tip[3] = {
        base[0] + axis[0] * length,
        base[1] + axis[1] * length,
        base[2] + axis[2] * length,
    };
    enum { k_segs = 10 };
    for (int i = 0; i < k_segs; ++i) {
        float a0 = k_two_pi * (float)i / (float)k_segs;
        float a1 = k_two_pi * (float)(i + 1) / (float)k_segs;
        float c0 = cosf(a0) * radius, s0 = sinf(a0) * radius;
        float c1 = cosf(a1) * radius, s1 = sinf(a1) * radius;
        float p0[3] = { base[0] + u[0] * c0 + v[0] * s0, base[1] + u[1] * c0 + v[1] * s0, base[2] + u[2] * c0 + v[2] * s0 };
        float p1[3] = { base[0] + u[0] * c1 + v[0] * s1, base[1] + u[1] * c1 + v[1] * s1, base[2] + u[2] * c1 + v[2] * s1 };
        emit_tri(vb, p0, p1, tip, (i & 1) ? rgba_lit : rgba_dark);
    }
}

static void emit_cube(VertBuf *vb, const float center[3], const float axes[3][3],
                      float half, uint32_t rgba_top, uint32_t rgba_side) {
    float pts[8][3];
    for (int i = 0; i < 8; ++i) {
        float sx = (i & 1) ? half : -half;
        float sy = (i & 2) ? half : -half;
        float sz = (i & 4) ? half : -half;
        pts[i][0] = center[0] + axes[0][0] * sx + axes[1][0] * sy + axes[2][0] * sz;
        pts[i][1] = center[1] + axes[0][1] * sx + axes[1][1] * sy + axes[2][1] * sz;
        pts[i][2] = center[2] + axes[0][2] * sx + axes[1][2] * sy + axes[2][2] * sz;
    }
    static constexpr int k_faces[6][4] = {
        { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 },
        { 2, 3, 7, 6 }, { 1, 5, 7, 3 }, { 0, 2, 6, 4 },
    };
    for (int f = 0; f < 6; ++f) {
        uint32_t c = (f == 1 || f == 4) ? rgba_top : rgba_side;
        emit_quad(vb, pts[k_faces[f][0]], pts[k_faces[f][1]], pts[k_faces[f][2]], pts[k_faces[f][3]], c);
    }
}

static void emit_aabb_wire(VertBuf *vb, const GizmoCamera *cam,
                           const float mn[3], const float mx[3], float half_w, uint32_t rgba) {
    float c[8][3] = {
        { mn[0], mn[1], mn[2] }, { mx[0], mn[1], mn[2] },
        { mx[0], mx[1], mn[2] }, { mn[0], mx[1], mn[2] },
        { mn[0], mn[1], mx[2] }, { mx[0], mn[1], mx[2] },
        { mx[0], mx[1], mx[2] }, { mn[0], mx[1], mx[2] },
    };
    static constexpr int k_edges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };
    for (int e = 0; e < 12; ++e) {
        emit_ribbon(vb, cam, c[k_edges[e][0]], c[k_edges[e][1]], half_w, rgba);
    }
}

static uint32_t axis_color(int axis, bool hot, bool active, uint8_t alpha) {
    if (active) return pack_rgba(255u, 232u, 96u,  alpha);
    if (hot)    return pack_rgba(255u, 214u, 72u,  alpha);
    if (axis == 0) return pack_rgba(242u, 72u,  72u,  alpha);
    if (axis == 1) return pack_rgba(74u,  224u, 110u, alpha);
    return pack_rgba(76u, 148u, 255u, alpha);
}

static uint32_t axis_color_dark(int axis, bool hot, bool active, uint8_t alpha) {
    if (active) return pack_rgba(225u, 186u, 44u, alpha);
    if (hot)    return pack_rgba(220u, 174u, 36u, alpha);
    if (axis == 0) return pack_rgba(182u, 42u,  42u, alpha);
    if (axis == 1) return pack_rgba(44u,  166u, 74u, alpha);
    return pack_rgba(46u, 102u, 206u, alpha);
}

uint32_t gizmo_build_vertices(const GizmoState *st,
                              const EcsWorld *w,
                              EcsEntity selected,
                              const GizmoCamera *cam,
                              GizmoVertex *out_verts,
                              uint32_t max_verts) {
    if (st == nullptr || w == nullptr || cam == nullptr || out_verts == nullptr || max_verts < 64u) {
        return 0u;
    }
    VertBuf vb = { .v = out_verts, .count = 0u, .cap = max_verts };

    // 1. Optional 3D Debug Overlays: Arvo World AABBs, Parent->Child Arrows, Local Basis Triads
    if (st->debug_draw) {
        uint32_t n = ecs_entity_count(w);
        for (uint32_t i = 0u; i < n; ++i) {
            EcsEntity e = ecs_entity_at(w, i);
            EcsAabb aabb = {0};
            EcsTrs  trs  = {0};
            if (!ecs_get_world_aabb(w, e, &aabb) || !ecs_get_world_trs(w, e, &trs)) {
                continue;
            }
            bool is_sel = (e == selected);
            float lw = gizmo_compute_scale(cam, trs.pos, is_sel ? 1.8f : 1.15f);
            uint32_t box_col = is_sel
                ? pack_rgba(255u, 218u, 78u, 220u)
                : pack_rgba(72u,  212u, 232u, 130u);
            emit_aabb_wire(&vb, cam, aabb.min_v, aabb.max_v, lw, box_col);

            // Local XYZ orientation triad at entity center
            float triad_len = gizmo_compute_scale(cam, trs.pos, 26.0f);
            float m3[9];
            ecs_quat_to_mat3(trs.rot, m3);
            for (int a = 0; a < 3; ++a) {
                float tip[3] = {
                    trs.pos[0] + m3[a]     * triad_len,
                    trs.pos[1] + m3[3 + a] * triad_len,
                    trs.pos[2] + m3[6 + a] * triad_len,
                };
                emit_ribbon(&vb, cam, trs.pos, tip, lw * 0.9f, axis_color(a, false, false, 185u));
            }

            // Parent -> Child directed hierarchy connector arrow
            EcsEntity parent = ecs_get_parent(w, e);
            if (parent != ECS_NULL_ENTITY) {
                EcsTrs ptrs = {0};
                if (ecs_get_world_trs(w, parent, &ptrs)) {
                    uint32_t link_col = pack_rgba(246u, 118u, 238u, 225u);
                    float link_w = gizmo_compute_scale(cam, trs.pos, 1.9f);
                    emit_ribbon(&vb, cam, ptrs.pos, trs.pos, link_w, link_col);
                    float dir[3] = {
                        trs.pos[0] - ptrs.pos[0], trs.pos[1] - ptrs.pos[1], trs.pos[2] - ptrs.pos[2],
                    };
                    float dist = v3_len(dir);
                    if (dist > 1.0e-3f && v3_norm(dir)) {
                        float head_len = fminf(dist * 0.25f, gizmo_compute_scale(cam, trs.pos, 16.0f));
                        float mid_b[3] = {
                            ptrs.pos[0] + dir[0] * (dist * 0.55f - head_len * 0.5f),
                            ptrs.pos[1] + dir[1] * (dist * 0.55f - head_len * 0.5f),
                            ptrs.pos[2] + dir[2] * (dist * 0.55f - head_len * 0.5f),
                        };
                        emit_cone(&vb, mid_b, dir, head_len, head_len * 0.42f,
                                  pack_rgba(255u, 152u, 248u, 240u), pack_rgba(195u, 72u, 190u, 240u));
                    }
                }
            }
        }
    }

    // 2. Interactive 3D Transform Gizmo on selected entity
    if (st->mode == GIZMO_MODE_NONE || !ecs_alive(w, selected)) {
        return vb.count;
    }

    float pivot[3];
    float axes[3][3];
    GizmoSpace use_space = (st->mode == GIZMO_MODE_SCALE) ? GIZMO_SPACE_LOCAL : st->space;
    gizmo_basis_for_entity(w, selected, use_space, pivot, axes);
    float sc = gizmo_compute_scale(cam, pivot, k_gizmo_px);

    // Back-to-front painter sort of the 3 axes by view depth so closer arrows draw last
    int order[3] = { 0, 1, 2 };
    float depth[3];
    for (int a = 0; a < 3; ++a) {
        float tip[3] = {
            pivot[0] + axes[a][0] * sc, pivot[1] + axes[a][1] * sc, pivot[2] + axes[a][2] * sc,
        };
        float d[3] = { tip[0] - cam->eye_w[0], tip[1] - cam->eye_w[1], tip[2] - cam->eye_w[2] };
        depth[a] = v3_dot(d, cam->fwd_w);
    }
    for (int i = 0; i < 2; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            if (depth[order[i]] < depth[order[j]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    if (st->mode == GIZMO_MODE_TRANSLATE) {
        // Planar drag quads (XY, XZ, YZ)
        static constexpr int k_planes[3][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 2, 0 } };
        static constexpr GizmoHandle k_ph[3] = { GIZMO_HANDLE_XY, GIZMO_HANDLE_XZ, GIZMO_HANDLE_YZ };
        for (int p = 0; p < 3; ++p) {
            bool act = (st->active == k_ph[p]);
            bool hot = (st->hovered == k_ph[p]);
            int u = k_planes[p][0], v = k_planes[p][1], n_ax = k_planes[p][2];
            float p0[3], p1[3], p2[3], p3[3];
            for (int k = 0; k < 3; ++k) {
                p0[k] = pivot[k] + (axes[u][k] * 0.24f + axes[v][k] * 0.24f) * sc;
                p1[k] = pivot[k] + (axes[u][k] * 0.48f + axes[v][k] * 0.24f) * sc;
                p2[k] = pivot[k] + (axes[u][k] * 0.48f + axes[v][k] * 0.48f) * sc;
                p3[k] = pivot[k] + (axes[u][k] * 0.24f + axes[v][k] * 0.48f) * sc;
            }
            emit_quad(&vb, p0, p1, p2, p3, axis_color(n_ax, hot, act, act ? 190u : (hot ? 150u : 82u)));
            uint32_t edge = axis_color(n_ax, hot, act, 235u);
            emit_ribbon(&vb, cam, p0, p1, sc * 0.014f, edge);
            emit_ribbon(&vb, cam, p1, p2, sc * 0.014f, edge);
            emit_ribbon(&vb, cam, p2, p3, sc * 0.014f, edge);
            emit_ribbon(&vb, cam, p3, p0, sc * 0.014f, edge);
        }

        // X, Y, Z shafts + conical arrowheads
        for (int idx = 0; idx < 3; ++idx) {
            int a = order[idx];
            GizmoHandle ah = (GizmoHandle)(GIZMO_HANDLE_X + a);
            bool act = (st->active == ah);
            bool hot = (st->hovered == ah);
            float base[3] = {
                pivot[0] + axes[a][0] * sc * 0.14f,
                pivot[1] + axes[a][1] * sc * 0.14f,
                pivot[2] + axes[a][2] * sc * 0.14f,
            };
            float shaft_end[3] = {
                pivot[0] + axes[a][0] * sc * 0.86f,
                pivot[1] + axes[a][1] * sc * 0.86f,
                pivot[2] + axes[a][2] * sc * 0.86f,
            };
            emit_ribbon(&vb, cam, base, shaft_end, sc * (act || hot ? 0.036f : 0.025f),
                        axis_color(a, hot, act, 250u));
            emit_cone(&vb, shaft_end, axes[a], sc * 0.24f, sc * (act || hot ? 0.088f : 0.072f),
                      axis_color(a, hot, act, 255u), axis_color_dark(a, hot, act, 255u));
        }

        // Center free-translate cube
        bool c_act = (st->active == GIZMO_HANDLE_CENTER);
        bool c_hot = (st->hovered == GIZMO_HANDLE_CENTER);
        emit_cube(&vb, pivot, axes, sc * 0.085f,
                  c_act || c_hot ? pack_rgba(255u, 232u, 96u, 255u) : pack_rgba(246u, 248u, 252u, 245u),
                  c_act || c_hot ? pack_rgba(220u, 178u, 44u, 255u) : pack_rgba(182u, 190u, 204u, 245u));
    } else if (st->mode == GIZMO_MODE_ROTATE) {
        enum { k_ring_segs = 48 };
        const float ring_r = sc * 0.92f;
        for (int idx = 0; idx < 3; ++idx) {
            int a = order[idx];
            GizmoHandle ah = (GizmoHandle)(GIZMO_HANDLE_X + a);
            bool act = (st->active == ah);
            bool hot = (st->hovered == ah);
            float ref[3] = { 1.0f, 0.0f, 0.0f };
            if (fabsf(v3_dot(axes[a], ref)) > 0.9f) { ref[0] = 0.0f; ref[1] = 1.0f; }
            float u[3], v[3];
            v3_cross(axes[a], ref, u); (void)v3_norm(u);
            v3_cross(axes[a], u, v);   (void)v3_norm(v);

            float rw = sc * (act || hot ? 0.036f : 0.024f);
            for (int s = 0; s < k_ring_segs; ++s) {
                float a0 = k_two_pi * (float)s / (float)k_ring_segs;
                float a1 = k_two_pi * (float)(s + 1) / (float)k_ring_segs;
                float c0 = cosf(a0) * ring_r, s0 = sinf(a0) * ring_r;
                float c1 = cosf(a1) * ring_r, s1 = sinf(a1) * ring_r;
                float p0[3] = { pivot[0] + u[0] * c0 + v[0] * s0, pivot[1] + u[1] * c0 + v[1] * s0, pivot[2] + u[2] * c0 + v[2] * s0 };
                float p1[3] = { pivot[0] + u[0] * c1 + v[0] * s1, pivot[1] + u[1] * c1 + v[1] * s1, pivot[2] + u[2] * c1 + v[2] * s1 };
                float mid[3] = { 0.5f * (p0[0] + p1[0]) - pivot[0], 0.5f * (p0[1] + p1[1]) - pivot[1], 0.5f * (p0[2] + p1[2]) - pivot[2] };
                // Subtle back-hemisphere fade so overlapping rings read cleanly
                bool front = (v3_dot(mid, cam->fwd_w) <= 0.12f * ring_r) || act;
                uint8_t alpha = front ? 245u : 95u;
                emit_ribbon(&vb, cam, p0, p1, rw, axis_color(a, hot, act, alpha));
            }
        }

        // Live swept angle pie wedge while rotating
        if (st->dragging && fabsf(st->drag_curr_angle - st->drag_start_angle) > 1.0e-3f) {
            float ref[3] = { 1.0f, 0.0f, 0.0f };
            if (fabsf(v3_dot(st->drag_plane_n, ref)) > 0.9f) { ref[0] = 0.0f; ref[1] = 1.0f; }
            float u[3], v[3];
            v3_cross(st->drag_plane_n, ref, u); (void)v3_norm(u);
            v3_cross(st->drag_plane_n, u, v);   (void)v3_norm(v);
            float da = st->drag_curr_angle - st->drag_start_angle;
            int steps = 16;
            uint32_t pie_c = pack_rgba(255u, 220u, 82u, 115u);
            for (int s = 0; s < steps; ++s) {
                float a0 = st->drag_start_angle + da * ((float)s / (float)steps);
                float a1 = st->drag_start_angle + da * ((float)(s + 1) / (float)steps);
                float p0[3] = { pivot[0] + (u[0] * cosf(a0) + v[0] * sinf(a0)) * ring_r,
                                pivot[1] + (u[1] * cosf(a0) + v[1] * sinf(a0)) * ring_r,
                                pivot[2] + (u[2] * cosf(a0) + v[2] * sinf(a0)) * ring_r };
                float p1[3] = { pivot[0] + (u[0] * cosf(a1) + v[0] * sinf(a1)) * ring_r,
                                pivot[1] + (u[1] * cosf(a1) + v[1] * sinf(a1)) * ring_r,
                                pivot[2] + (u[2] * cosf(a1) + v[2] * sinf(a1)) * ring_r };
                emit_tri(&vb, pivot, p0, p1, pie_c);
            }
        }
    } else if (st->mode == GIZMO_MODE_SCALE) {
        // X, Y, Z shafts tipped with solid 3D cubes + white center uniform scale cube
        for (int idx = 0; idx < 3; ++idx) {
            int a = order[idx];
            GizmoHandle ah = (GizmoHandle)(GIZMO_HANDLE_X + a);
            bool act = (st->active == ah);
            bool hot = (st->hovered == ah);
            float base[3] = {
                pivot[0] + axes[a][0] * sc * 0.14f,
                pivot[1] + axes[a][1] * sc * 0.14f,
                pivot[2] + axes[a][2] * sc * 0.14f,
            };
            float tip[3] = {
                pivot[0] + axes[a][0] * sc * 0.94f,
                pivot[1] + axes[a][1] * sc * 0.94f,
                pivot[2] + axes[a][2] * sc * 0.94f,
            };
            emit_ribbon(&vb, cam, base, tip, sc * (act || hot ? 0.034f : 0.024f),
                        axis_color(a, hot, act, 250u));
            emit_cube(&vb, tip, axes, sc * (act || hot ? 0.095f : 0.078f),
                      axis_color(a, hot, act, 255u), axis_color_dark(a, hot, act, 255u));
        }
        bool c_act = (st->active == GIZMO_HANDLE_CENTER);
        bool c_hot = (st->hovered == GIZMO_HANDLE_CENTER);
        emit_cube(&vb, pivot, axes, sc * 0.105f,
                  c_act || c_hot ? pack_rgba(255u, 232u, 96u, 255u) : pack_rgba(246u, 248u, 252u, 250u),
                  c_act || c_hot ? pack_rgba(220u, 178u, 44u, 255u) : pack_rgba(182u, 190u, 204u, 250u));
    }

    return vb.count;
}
