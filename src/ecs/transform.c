#include "ecs/ecs.h"
#include <math.h>
#include <string.h>

static constexpr float k_pi      = 3.14159265358979323846f;
static constexpr float k_rad_deg = 57.2957795130823208768f;

void ecs_quat_identity(float out_q[4]) {
    out_q[0] = 0.0f;
    out_q[1] = 0.0f;
    out_q[2] = 0.0f;
    out_q[3] = 1.0f;
}

void ecs_quat_norm(float q[4]) {
    float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!isfinite(n2) || n2 < 1.0e-12f) {
        ecs_quat_identity(q);
        return;
    }
    float inv = 1.0f / sqrtf(n2);
    q[0] *= inv;
    q[1] *= inv;
    q[2] *= inv;
    q[3] *= inv;
}

void ecs_quat_mul(const float a[4], const float b[4], float out_q[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out_q[0] = x;
    out_q[1] = y;
    out_q[2] = z;
    out_q[3] = w;
}

void ecs_quat_conj(const float q[4], float out_q[4]) {
    out_q[0] = -q[0];
    out_q[1] = -q[1];
    out_q[2] = -q[2];
    out_q[3] =  q[3];
}

void ecs_quat_from_axis_angle(const float axis[3], float angle_rad, float out_q[4]) {
    float len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (len < 1.0e-7f) {
        ecs_quat_identity(out_q);
        return;
    }
    float inv = 1.0f / len;
    float half = angle_rad * 0.5f;
    float s = sinf(half);
    out_q[0] = axis[0] * inv * s;
    out_q[1] = axis[1] * inv * s;
    out_q[2] = axis[2] * inv * s;
    out_q[3] = cosf(half);
}

void ecs_quat_from_pose(float pitch_x_rad, float yaw_y_rad, float out_q[4]) {
    float qx[4];
    float qy[4];
    ecs_quat_from_axis_angle((const float[3]){ 1.0f, 0.0f, 0.0f }, pitch_x_rad, qx);
    ecs_quat_from_axis_angle((const float[3]){ 0.0f, 1.0f, 0.0f }, yaw_y_rad,   qy);
    ecs_quat_mul(qy, qx, out_q);
    ecs_quat_norm(out_q);
}

void ecs_quat_to_euler_deg(const float q[4], float out_deg[3]) {
    // Intrinsic XYZ Euler decomposition in degrees for human-readable HUD inspector
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float sinr_cosp = 2.0f * (w * x + y * z);
    float cosr_cosp = 1.0f - 2.0f * (x * x + y * y);
    float rx = atan2f(sinr_cosp, cosr_cosp);

    float sinp = 2.0f * (w * y - z * x);
    float ry = fabsf(sinp) >= 1.0f ? copysignf(k_pi * 0.5f, sinp) : asinf(sinp);

    float siny_cosp = 2.0f * (w * z + x * y);
    float cosy_cosp = 1.0f - 2.0f * (y * y + z * z);
    float rz = atan2f(siny_cosp, cosy_cosp);

    out_deg[0] = rx * k_rad_deg;
    out_deg[1] = ry * k_rad_deg;
    out_deg[2] = rz * k_rad_deg;
}

void ecs_quat_rotate_vec3(const float q[4], const float v[3], float out_v[3]) {
    float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    float cx = q[1] * tz - q[2] * ty;
    float cy = q[2] * tx - q[0] * tz;
    float cz = q[0] * ty - q[1] * tx;
    out_v[0] = v[0] + q[3] * tx + cx;
    out_v[1] = v[1] + q[3] * ty + cy;
    out_v[2] = v[2] + q[3] * tz + cz;
}

void ecs_quat_to_mat3(const float q[4], float out_m3[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    out_m3[0] = 1.0f - 2.0f * (yy + zz);
    out_m3[1] = 2.0f * (xy - wz);
    out_m3[2] = 2.0f * (xz + wy);

    out_m3[3] = 2.0f * (xy + wz);
    out_m3[4] = 1.0f - 2.0f * (xx + zz);
    out_m3[5] = 2.0f * (yz - wx);

    out_m3[6] = 2.0f * (xz - wy);
    out_m3[7] = 2.0f * (yz + wx);
    out_m3[8] = 1.0f - 2.0f * (xx + yy);
}

void ecs_mat3_to_quat(const float m[9], float out_q[4]) {
    float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        out_q[3] = 0.25f * s;
        out_q[0] = (m[7] - m[5]) / s;
        out_q[1] = (m[2] - m[6]) / s;
        out_q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        out_q[3] = (m[7] - m[5]) / s;
        out_q[0] = 0.25f * s;
        out_q[1] = (m[1] + m[3]) / s;
        out_q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        out_q[3] = (m[2] - m[6]) / s;
        out_q[0] = (m[1] + m[3]) / s;
        out_q[1] = 0.25f * s;
        out_q[2] = (m[5] + m[7]) / s;
    } else {
        float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        out_q[3] = (m[3] - m[1]) / s;
        out_q[0] = (m[2] + m[6]) / s;
        out_q[1] = (m[5] + m[7]) / s;
        out_q[2] = 0.25f * s;
    }
    ecs_quat_norm(out_q);
}

void ecs_mat3x4_from_trs(const float pos[3], const float q[4], const float scale[3],
                         float out_m[12]) {
    float r[9];
    ecs_quat_to_mat3(q, r);
    out_m[0]  = r[0] * scale[0];
    out_m[1]  = r[1] * scale[1];
    out_m[2]  = r[2] * scale[2];
    out_m[3]  = pos[0];

    out_m[4]  = r[3] * scale[0];
    out_m[5]  = r[4] * scale[1];
    out_m[6]  = r[5] * scale[2];
    out_m[7]  = pos[1];

    out_m[8]  = r[6] * scale[0];
    out_m[9]  = r[7] * scale[1];
    out_m[10] = r[8] * scale[2];
    out_m[11] = pos[2];
}

void ecs_mat3x4_mul(const float a[12], const float b[12], float out_m[12]) {
    float r[12];
    for (int row = 0; row < 3; ++row) {
        const float *ar = &a[row * 4];
        r[row * 4 + 0] = ar[0] * b[0] + ar[1] * b[4] + ar[2] * b[8];
        r[row * 4 + 1] = ar[0] * b[1] + ar[1] * b[5] + ar[2] * b[9];
        r[row * 4 + 2] = ar[0] * b[2] + ar[1] * b[6] + ar[2] * b[10];
        r[row * 4 + 3] = ar[0] * b[3] + ar[1] * b[7] + ar[2] * b[11] + ar[3];
    }
    memcpy(out_m, r, sizeof r);
}

void ecs_mat3x4_cofactor3(const float m[12], float out_nrm[9]) {
    // Column vectors of linear 3x3
    float c0x = m[0], c0y = m[4], c0z = m[8];
    float c1x = m[1], c1y = m[5], c1z = m[9];
    float c2x = m[2], c2y = m[6], c2z = m[10];

    // Cofactor columns = cross(c1, c2), cross(c2, c0), cross(c0, c1)
    float n0x = c1y * c2z - c1z * c2y;
    float n0y = c1z * c2x - c1x * c2z;
    float n0z = c1x * c2y - c1y * c2x;

    float n1x = c2y * c0z - c2z * c0y;
    float n1y = c2z * c0x - c2x * c0z;
    float n1z = c2x * c0y - c2y * c0x;

    float n2x = c0y * c1z - c0z * c1y;
    float n2y = c0z * c1x - c0x * c1z;
    float n2z = c0x * c1y - c0y * c1x;

    float det = c0x * n0x + c0y * n0y + c0z * n0z;
    float sgn = det < 0.0f ? -1.0f : 1.0f;

    out_nrm[0] = n0x * sgn; out_nrm[1] = n1x * sgn; out_nrm[2] = n2x * sgn;
    out_nrm[3] = n0y * sgn; out_nrm[4] = n1y * sgn; out_nrm[5] = n2y * sgn;
    out_nrm[6] = n0z * sgn; out_nrm[7] = n1z * sgn; out_nrm[8] = n2z * sgn;
}

bool ecs_mat3x4_invert(const float m[12], float out_inv[12]) {
    float c0x = m[0], c0y = m[4], c0z = m[8];
    float c1x = m[1], c1y = m[5], c1z = m[9];
    float c2x = m[2], c2y = m[6], c2z = m[10];

    float r0x = c1y * c2z - c1z * c2y;
    float r0y = c1z * c2x - c1x * c2z;
    float r0z = c1x * c2y - c1y * c2x;

    float det = c0x * r0x + c0y * r0y + c0z * r0z;
    if (!isfinite(det) || fabsf(det) < 1.0e-10f) {
        return false;
    }
    float inv_det = 1.0f / det;

    float r1x = c2y * c0z - c2z * c0y;
    float r1y = c2z * c0x - c2x * c0z;
    float r1z = c2x * c0y - c2y * c0x;

    float r2x = c0y * c1z - c0z * c1y;
    float r2y = c0z * c1x - c0x * c1z;
    float r2z = c0x * c1y - c0y * c1x;

    float inv[12];
    inv[0]  = r0x * inv_det; inv[1]  = r0y * inv_det; inv[2]  = r0z * inv_det;
    inv[4]  = r1x * inv_det; inv[5]  = r1y * inv_det; inv[6]  = r1z * inv_det;
    inv[8]  = r2x * inv_det; inv[9]  = r2y * inv_det; inv[10] = r2z * inv_det;

    float tx = m[3], ty = m[7], tz = m[11];
    inv[3]  = -(inv[0] * tx + inv[1] * ty + inv[2]  * tz);
    inv[7]  = -(inv[4] * tx + inv[5] * ty + inv[6]  * tz);
    inv[11] = -(inv[8] * tx + inv[9] * ty + inv[10] * tz);
    memcpy(out_inv, inv, sizeof inv);
    return true;
}

void ecs_mat3x4_transform_point(const float m[12], const float p[3], float out_p[3]) {
    float x = m[0] * p[0] + m[1] * p[1] + m[2]  * p[2] + m[3];
    float y = m[4] * p[0] + m[5] * p[1] + m[6]  * p[2] + m[7];
    float z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    out_p[0] = x;
    out_p[1] = y;
    out_p[2] = z;
}

void ecs_mat3x4_decompose(const float m[12], float out_pos[3], float out_rot[4], float out_scale[3]) {
    out_pos[0] = m[3];
    out_pos[1] = m[7];
    out_pos[2] = m[11];

    float sx = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8]  * m[8]);
    float sy = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9]  * m[9]);
    float sz = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);

    // Check handedness (negative determinant -> flip sx)
    float cx = m[5] * m[10] - m[9] * m[6];
    float cy = m[9] * m[2]  - m[1] * m[10];
    float cz = m[1] * m[6]  - m[5] * m[2];
    float det = m[0] * cx + m[4] * cy + m[8] * cz;
    if (det < 0.0f) {
        sx = -sx;
    }

    out_scale[0] = sx;
    out_scale[1] = sy;
    out_scale[2] = sz;

    float isx = fabsf(sx) > 1.0e-7f ? (1.0f / sx) : 0.0f;
    float isy = fabsf(sy) > 1.0e-7f ? (1.0f / sy) : 0.0f;
    float isz = fabsf(sz) > 1.0e-7f ? (1.0f / sz) : 0.0f;

    float r[9] = {
        m[0] * isx, m[1] * isy, m[2]  * isz,
        m[4] * isx, m[5] * isy, m[6]  * isz,
        m[8] * isx, m[9] * isy, m[10] * isz,
    };
    ecs_mat3_to_quat(r, out_rot);
}

// ----------------------------------------------------------------------------
// Local & World Transform Accessors
// ----------------------------------------------------------------------------

void ecs_set_local_trs(EcsWorld *w, EcsEntity e, const float pos[3],
                       const float rot_q[4], const float scale[3]) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    if (pos != nullptr) {
        w->lx[d] = pos[0];
        w->ly[d] = pos[1];
        w->lz[d] = pos[2];
    }
    if (rot_q != nullptr) {
        float q[4] = { rot_q[0], rot_q[1], rot_q[2], rot_q[3] };
        ecs_quat_norm(q);
        w->qx[d] = q[0];
        w->qy[d] = q[1];
        w->qz[d] = q[2];
        w->qw[d] = q[3];
    }
    if (scale != nullptr) {
        w->sx[d] = fmaxf(scale[0], 0.05f);
        w->sy[d] = fmaxf(scale[1], 0.05f);
        w->sz[d] = fmaxf(scale[2], 0.05f);
    }
    w->dirty[d] |= (uint8_t)(ECS_DIRTY_LOCAL | ECS_DIRTY_WORLD);
}

void ecs_set_local_pos(EcsWorld *w, EcsEntity e, float x, float y, float z) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    w->lx[d] = x;
    w->ly[d] = y;
    w->lz[d] = z;
    w->dirty[d] |= (uint8_t)(ECS_DIRTY_LOCAL | ECS_DIRTY_WORLD);
}

void ecs_set_local_rot(EcsWorld *w, EcsEntity e, const float rot_q[4]) {
    if (rot_q == nullptr) {
        return;
    }
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    float q[4] = { rot_q[0], rot_q[1], rot_q[2], rot_q[3] };
    ecs_quat_norm(q);
    w->qx[d] = q[0];
    w->qy[d] = q[1];
    w->qz[d] = q[2];
    w->qw[d] = q[3];
    w->dirty[d] |= (uint8_t)(ECS_DIRTY_LOCAL | ECS_DIRTY_WORLD);
}

void ecs_set_local_scale(EcsWorld *w, EcsEntity e, float sx, float sy, float sz) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    w->sx[d] = fmaxf(sx, 0.05f);
    w->sy[d] = fmaxf(sy, 0.05f);
    w->sz[d] = fmaxf(sz, 0.05f);
    w->dirty[d] |= (uint8_t)(ECS_DIRTY_LOCAL | ECS_DIRTY_WORLD);
}

bool ecs_get_local_trs(const EcsWorld *w, EcsEntity e, EcsTrs *out_trs) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE || out_trs == nullptr) {
        return false;
    }
    *out_trs = (EcsTrs){
        .pos   = { w->lx[d], w->ly[d], w->lz[d] },
        .rot   = { w->qx[d], w->qy[d], w->qz[d], w->qw[d] },
        .scale = { w->sx[d], w->sy[d], w->sz[d] },
    };
    return true;
}

void ecs_set_world_trs(EcsWorld *w, EcsEntity e, const float pos[3],
                       const float rot_q[4], const float scale[3]) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    EcsEntity p = w->parent[d];
    if (p == ECS_NULL_ENTITY) {
        ecs_set_local_trs(w, e, pos, rot_q, scale);
        return;
    }
    // Ensure parent's world matrix is fresh before inverting
    ecs_propagate_transforms(w);
    d = ecs_dense_index(w, e);
    uint32_t pd = ecs_dense_index(w, p);
    if (pd == ECS_INVALID_DENSE) {
        ecs_set_local_trs(w, e, pos, rot_q, scale);
        return;
    }
    float target_pos[3] = {
        pos != nullptr ? pos[0] : w->world_pos[d][0],
        pos != nullptr ? pos[1] : w->world_pos[d][1],
        pos != nullptr ? pos[2] : w->world_pos[d][2],
    };
    float target_rot[4] = {
        rot_q != nullptr ? rot_q[0] : w->world_rot[d][0],
        rot_q != nullptr ? rot_q[1] : w->world_rot[d][1],
        rot_q != nullptr ? rot_q[2] : w->world_rot[d][2],
        rot_q != nullptr ? rot_q[3] : w->world_rot[d][3],
    };
    ecs_quat_norm(target_rot);
    float target_scl[3] = {
        scale != nullptr ? fmaxf(scale[0], 0.05f) : w->world_scl[d][0],
        scale != nullptr ? fmaxf(scale[1], 0.05f) : w->world_scl[d][1],
        scale != nullptr ? fmaxf(scale[2], 0.05f) : w->world_scl[d][2],
    };

    float target_m[12];
    ecs_mat3x4_from_trs(target_pos, target_rot, target_scl, target_m);

    float inv_p[12];
    if (!ecs_mat3x4_invert(w->world_m[pd], inv_p)) {
        ecs_set_local_trs(w, e, target_pos, target_rot, target_scl);
        return;
    }
    float local_m[12];
    ecs_mat3x4_mul(inv_p, target_m, local_m);

    float lp[3], lq[4], ls[3];
    ecs_mat3x4_decompose(local_m, lp, lq, ls);
    ecs_set_local_trs(w, e, lp, lq, ls);
}

void ecs_set_world_pos(EcsWorld *w, EcsEntity e, float x, float y, float z) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    if (w->parent[d] == ECS_NULL_ENTITY) {
        ecs_set_local_pos(w, e, x, y, z);
        return;
    }
    ecs_propagate_transforms(w);
    uint32_t pd = ecs_dense_index(w, w->parent[d]);
    float inv_p[12];
    if (pd != ECS_INVALID_DENSE && ecs_mat3x4_invert(w->world_m[pd], inv_p)) {
        float wp[3] = { x, y, z };
        float lp[3];
        ecs_mat3x4_transform_point(inv_p, wp, lp);
        ecs_set_local_pos(w, e, lp[0], lp[1], lp[2]);
    } else {
        ecs_set_local_pos(w, e, x, y, z);
    }
}

void ecs_set_world_rot(EcsWorld *w, EcsEntity e, const float rot_q[4]) {
    if (rot_q == nullptr) {
        return;
    }
    ecs_set_world_trs(w, e, nullptr, rot_q, nullptr);
}

void ecs_set_world_scale(EcsWorld *w, EcsEntity e, float sx, float sy, float sz) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }
    if (w->parent[d] == ECS_NULL_ENTITY) {
        ecs_set_local_scale(w, e, sx, sy, sz);
        return;
    }
    const float sc[3] = { sx, sy, sz };
    ecs_set_world_trs(w, e, nullptr, nullptr, sc);
}

// ----------------------------------------------------------------------------
// Cycle-Safe Hierarchy Reparenting & Depth-Bucket Topological Propagation
// ----------------------------------------------------------------------------

bool ecs_is_descendant(const EcsWorld *w, EcsEntity maybe_desc, EcsEntity ancestor) {
    if (w == nullptr || maybe_desc == ECS_NULL_ENTITY || ancestor == ECS_NULL_ENTITY) {
        return false;
    }
    EcsEntity cur = maybe_desc;
    for (uint32_t step = 0u; step <= ECS_MAX_DEPTH && cur != ECS_NULL_ENTITY; ++step) {
        if (cur == ancestor) {
            return true;
        }
        uint32_t d = ecs_dense_index(w, cur);
        if (d == ECS_INVALID_DENSE) {
            return false;
        }
        cur = w->parent[d];
    }
    return false;
}

static void unlink_from_parent(EcsWorld *w, EcsEntity child, uint32_t cd) {
    EcsEntity old_p = w->parent[cd];
    if (old_p == ECS_NULL_ENTITY) {
        return;
    }
    uint32_t pd = ecs_dense_index(w, old_p);
    EcsEntity prev = w->prev_sibling[cd];
    EcsEntity next = w->next_sibling[cd];

    if (prev != ECS_NULL_ENTITY) {
        uint32_t prev_d = ecs_dense_index(w, prev);
        if (prev_d != ECS_INVALID_DENSE) {
            w->next_sibling[prev_d] = next;
        }
    } else if (pd != ECS_INVALID_DENSE && w->first_child[pd] == child) {
        w->first_child[pd] = next;
    }

    if (next != ECS_NULL_ENTITY) {
        uint32_t next_d = ecs_dense_index(w, next);
        if (next_d != ECS_INVALID_DENSE) {
            w->prev_sibling[next_d] = prev;
        }
    }

    w->parent[cd]       = ECS_NULL_ENTITY;
    w->prev_sibling[cd] = ECS_NULL_ENTITY;
    w->next_sibling[cd] = ECS_NULL_ENTITY;
}

bool ecs_set_parent(EcsWorld *w, EcsEntity child, EcsEntity new_parent, bool keep_world_pose) {
    uint32_t cd = ecs_dense_index(w, child);
    if (cd == ECS_INVALID_DENSE) {
        return false;
    }
    if (new_parent != ECS_NULL_ENTITY) {
        if (!ecs_alive(w, new_parent) || child == new_parent) {
            return false;
        }
        // Cycle guard: new_parent cannot be child or any descendant of child
        if (ecs_is_descendant(w, new_parent, child)) {
            return false;
        }
        uint32_t npd = ecs_dense_index(w, new_parent);
        if (w->depth[npd] + 1u >= ECS_MAX_DEPTH) {
            return false;
        }
    }
    if (w->parent[cd] == new_parent) {
        return true;
    }

    float saved_world_m[12] = {0};
    if (keep_world_pose) {
        ecs_propagate_transforms(w);
        cd = ecs_dense_index(w, child);
        memcpy(saved_world_m, w->world_m[cd], sizeof saved_world_m);
    }

    unlink_from_parent(w, child, cd);

    if (new_parent != ECS_NULL_ENTITY) {
        uint32_t npd = ecs_dense_index(w, new_parent);
        EcsEntity old_first = w->first_child[npd];
        w->parent[cd]       = new_parent;
        w->prev_sibling[cd] = ECS_NULL_ENTITY;
        w->next_sibling[cd] = old_first;
        w->first_child[npd] = child;
        if (old_first != ECS_NULL_ENTITY) {
            uint32_t ofd = ecs_dense_index(w, old_first);
            if (ofd != ECS_INVALID_DENSE) {
                w->prev_sibling[ofd] = child;
            }
        }
    }

    if (keep_world_pose) {
        float new_local_m[12];
        if (new_parent != ECS_NULL_ENTITY) {
            uint32_t npd = ecs_dense_index(w, new_parent);
            float inv_p[12];
            if (ecs_mat3x4_invert(w->world_m[npd], inv_p)) {
                ecs_mat3x4_mul(inv_p, saved_world_m, new_local_m);
            } else {
                memcpy(new_local_m, saved_world_m, sizeof new_local_m);
            }
        } else {
            memcpy(new_local_m, saved_world_m, sizeof new_local_m);
        }
        float lp[3], lq[4], ls[3];
        ecs_mat3x4_decompose(new_local_m, lp, lq, ls);
        w->lx[cd] = lp[0]; w->ly[cd] = lp[1]; w->lz[cd] = lp[2];
        w->qx[cd] = lq[0]; w->qy[cd] = lq[1]; w->qz[cd] = lq[2]; w->qw[cd] = lq[3];
        w->sx[cd] = fmaxf(ls[0], 0.05f);
        w->sy[cd] = fmaxf(ls[1], 0.05f);
        w->sz[cd] = fmaxf(ls[2], 0.05f);
    }

    w->dirty[cd] |= (uint8_t)(ECS_DIRTY_LOCAL | ECS_DIRTY_WORLD);
    w->topo_dirty = true;
    return true;
}

EcsEntity ecs_get_parent(const EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    return d != ECS_INVALID_DENSE ? w->parent[d] : ECS_NULL_ENTITY;
}

uint32_t ecs_get_depth(const EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    return d != ECS_INVALID_DENSE ? (uint32_t)w->depth[d] : 0u;
}

uint32_t ecs_count_children(const EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return 0u;
    }
    uint32_t count = 0u;
    EcsEntity ch = w->first_child[d];
    while (ch != ECS_NULL_ENTITY && count < ECS_MAX_ENTITIES) {
        count++;
        uint32_t chd = ecs_dense_index(w, ch);
        if (chd == ECS_INVALID_DENSE) {
            break;
        }
        ch = w->next_sibling[chd];
    }
    return count;
}

static void rebuild_topology_buckets(EcsWorld *w) {
    // Zero-recursion BFS over roots -> children guarantees strictly non-decreasing
    // depth order: parent ALWAYS appears strictly before every descendant.
    uint32_t head = 0u;
    uint32_t tail = 0u;
    for (uint32_t d = 0u; d < w->dense_count; ++d) {
        if (w->parent[d] == ECS_NULL_ENTITY) {
            w->depth[d]         = 0u;
            w->topo_order[tail++] = d;
        }
    }
    while (head < tail) {
        uint32_t pd = w->topo_order[head++];
        uint16_t next_depth = (uint16_t)(w->depth[pd] + 1u);
        EcsEntity ch = w->first_child[pd];
        while (ch != ECS_NULL_ENTITY && tail < ECS_MAX_ENTITIES) {
            uint32_t chd = ecs_dense_index(w, ch);
            if (chd == ECS_INVALID_DENSE) {
                break;
            }
            w->depth[chd]         = next_depth;
            w->topo_order[tail++] = chd;
            ch = w->next_sibling[chd];
        }
    }
    w->topo_count = tail;
    w->topo_dirty = false;
}

static void update_world_bounds(EcsWorld *w, uint32_t d) {
    const float *m   = w->world_m[d];
    const float *ext = w->local_ext[d];
    // Arvo exact OBB -> world AABB transform in 9 FMAs
    float wx = fabsf(m[0]) * ext[0] + fabsf(m[1]) * ext[1] + fabsf(m[2])  * ext[2];
    float wy = fabsf(m[4]) * ext[0] + fabsf(m[5]) * ext[1] + fabsf(m[6])  * ext[2];
    float wz = fabsf(m[8]) * ext[0] + fabsf(m[9]) * ext[1] + fabsf(m[10]) * ext[2];

    w->world_aabb_min[d][0] = m[3]  - wx;
    w->world_aabb_min[d][1] = m[7]  - wy;
    w->world_aabb_min[d][2] = m[11] - wz;
    w->world_aabb_max[d][0] = m[3]  + wx;
    w->world_aabb_max[d][1] = m[7]  + wy;
    w->world_aabb_max[d][2] = m[11] + wz;

    float c0 = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8]  * m[8]);
    float c1 = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9]  * m[9]);
    float c2 = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    float ms = fmaxf(c0, fmaxf(c1, c2));
    w->bound_scale[d]     = ms > 1.0e-5f ? ms : 1.0f;
    w->world_sphere[d][0] = m[3];
    w->world_sphere[d][1] = m[7];
    w->world_sphere[d][2] = m[11];
    w->world_sphere[d][3] = w->local_rad[d] * w->bound_scale[d];
}

void ecs_propagate_transforms(EcsWorld *w) {
    if (w == nullptr) {
        return;
    }
    if (w->topo_dirty) {
        rebuild_topology_buckets(w);
    }
    // Track which dense slots were updated this frame so children cascade in 1 branch
    bool updated[ECS_MAX_ENTITIES] = {0};
    uint32_t writes = 0u;

    for (uint32_t ti = 0u; ti < w->topo_count; ++ti) {
        uint32_t d = w->topo_order[ti];
        if (d >= w->dense_count) {
            continue;
        }
        EcsEntity p = w->parent[d];
        uint32_t pd = (p != ECS_NULL_ENTITY) ? ecs_dense_index(w, p) : ECS_INVALID_DENSE;
        bool parent_dirty = (pd != ECS_INVALID_DENSE) && updated[pd];

        if (w->dirty[d] == ECS_DIRTY_NONE && !parent_dirty) {
            continue;
        }

        const float pos[3] = { w->lx[d], w->ly[d], w->lz[d] };
        const float rot[4] = { w->qx[d], w->qy[d], w->qz[d], w->qw[d] };
        const float scl[3] = { w->sx[d], w->sy[d], w->sz[d] };
        float local_m[12];
        ecs_mat3x4_from_trs(pos, rot, scl, local_m);

        if (pd != ECS_INVALID_DENSE) {
            ecs_mat3x4_mul(w->world_m[pd], local_m, w->world_m[d]);
        } else {
            memcpy(w->world_m[d], local_m, sizeof local_m);
        }

        ecs_mat3x4_cofactor3(w->world_m[d], w->world_nrm[d]);
        ecs_mat3x4_decompose(w->world_m[d], w->world_pos[d], w->world_rot[d], w->world_scl[d]);
        update_world_bounds(w, d);

        w->dirty[d] = ECS_DIRTY_NONE;
        updated[d]  = true;
        writes++;
    }
    w->propagate_writes = writes;
}

bool ecs_get_world_matrix(const EcsWorld *w, EcsEntity e, float out_m[12]) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE || out_m == nullptr) {
        return false;
    }
    memcpy(out_m, w->world_m[d], 12u * sizeof(float));
    return true;
}

bool ecs_get_world_trs(const EcsWorld *w, EcsEntity e, EcsTrs *out_trs) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE || out_trs == nullptr) {
        return false;
    }
    memcpy(out_trs->pos,   w->world_pos[d], 3u * sizeof(float));
    memcpy(out_trs->rot,   w->world_rot[d], 4u * sizeof(float));
    memcpy(out_trs->scale, w->world_scl[d], 3u * sizeof(float));
    return true;
}

bool ecs_get_world_aabb(const EcsWorld *w, EcsEntity e, EcsAabb *out_aabb) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE || out_aabb == nullptr) {
        return false;
    }
    memcpy(out_aabb->min_v, w->world_aabb_min[d], 3u * sizeof(float));
    memcpy(out_aabb->max_v, w->world_aabb_max[d], 3u * sizeof(float));
    return true;
}

bool ecs_get_world_sphere(const EcsWorld *w, EcsEntity e, EcsSphere *out_sphere) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE || out_sphere == nullptr) {
        return false;
    }
    out_sphere->center[0] = w->world_sphere[d][0];
    out_sphere->center[1] = w->world_sphere[d][1];
    out_sphere->center[2] = w->world_sphere[d][2];
    out_sphere->radius    = w->world_sphere[d][3];
    return true;
}
