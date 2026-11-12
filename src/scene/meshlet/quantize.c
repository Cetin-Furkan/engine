#include "scene/scene.h"
#include <math.h>

static float clampf_range(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float sign_not_zero(float v) {
    return v >= 0.0f ? 1.0f : -1.0f;
}

void scene_oct_encode_snorm16(float nx, float ny, float nz, int16_t out_oct[2]) {
    if (out_oct == nullptr) {
        return;
    }
    float len2 = nx * nx + ny * ny + nz * nz;
    if (len2 > 1.0e-16f) {
        float inv_len = 1.0f / sqrtf(len2);
        nx *= inv_len;
        ny *= inv_len;
        nz *= inv_len;
    } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
    }

    float inv_l1 = 1.0f / (fabsf(nx) + fabsf(ny) + fabsf(nz));
    float ox = nx * inv_l1;
    float oy = ny * inv_l1;
    if (nz < 0.0f) {
        float tx = (1.0f - fabsf(oy)) * sign_not_zero(ox);
        float ty = (1.0f - fabsf(ox)) * sign_not_zero(oy);
        ox = tx;
        oy = ty;
    }

    ox = clampf_range(ox, -1.0f, 1.0f);
    oy = clampf_range(oy, -1.0f, 1.0f);
    out_oct[0] = (int16_t)lrintf(ox * 32767.0f);
    out_oct[1] = (int16_t)lrintf(oy * 32767.0f);
}

void scene_oct_decode_snorm16(const int16_t oct[2], float out_nrm[3]) {
    if (oct == nullptr || out_nrm == nullptr) {
        return;
    }
    float ex = clampf_range((float)oct[0] / 32767.0f, -1.0f, 1.0f);
    float ey = clampf_range((float)oct[1] / 32767.0f, -1.0f, 1.0f);
    float vx = ex;
    float vy = ey;
    float vz = 1.0f - fabsf(ex) - fabsf(ey);
    if (vz < 0.0f) {
        float tx = (1.0f - fabsf(vy)) * sign_not_zero(vx);
        float ty = (1.0f - fabsf(vx)) * sign_not_zero(vy);
        vx = tx;
        vy = ty;
    }
    float len2 = vx * vx + vy * vy + vz * vz;
    if (len2 > 1.0e-16f) {
        float inv_len = 1.0f / sqrtf(len2);
        out_nrm[0] = vx * inv_len;
        out_nrm[1] = vy * inv_len;
        out_nrm[2] = vz * inv_len;
    } else {
        out_nrm[0] = 0.0f;
        out_nrm[1] = 1.0f;
        out_nrm[2] = 0.0f;
    }
}

void scene_quantize_pos16(const float pos[3], const float origin[3], const float scale[3],
                          uint16_t out_q[3]) {
    if (pos == nullptr || origin == nullptr || scale == nullptr || out_q == nullptr) {
        return;
    }
    for (uint32_t k = 0u; k < 3u; ++k) {
        if (scale[k] <= 1.0e-8f) {
            out_q[k] = 0u;
        } else {
            float t = clampf_range((pos[k] - origin[k]) / scale[k], 0.0f, 1.0f);
            out_q[k] = (uint16_t)lrintf(t * 65535.0f);
        }
    }
}

void scene_dequantize_pos16(const uint16_t q[3], const float origin[3], const float scale[3],
                            float out_pos[3]) {
    if (q == nullptr || origin == nullptr || scale == nullptr || out_pos == nullptr) {
        return;
    }
    const float inv_65535 = 1.0f / 65535.0f;
    for (uint32_t k = 0u; k < 3u; ++k) {
        out_pos[k] = origin[k] + scale[k] * ((float)q[k] * inv_65535);
    }
}

uint32_t scene_pack_rgba8(float r, float g, float b, float roughness) {
    uint32_t cr = (uint32_t)lrintf(clampf_range(r, 0.0f, 1.0f) * 255.0f);
    uint32_t cg = (uint32_t)lrintf(clampf_range(g, 0.0f, 1.0f) * 255.0f);
    uint32_t cb = (uint32_t)lrintf(clampf_range(b, 0.0f, 1.0f) * 255.0f);
    uint32_t ca = (uint32_t)lrintf(clampf_range(roughness, 0.0f, 1.0f) * 255.0f);
    return (cr & 0xFFu) | ((cg & 0xFFu) << 8u) | ((cb & 0xFFu) << 16u) | ((ca & 0xFFu) << 24u);
}

SceneBarycentrics2D scene_eval_barycentrics_2d(const float clip0[4], const float clip1[4],
                                               const float clip2[4], float px_center,
                                               float py_center, float viewport_w,
                                               float viewport_h) {
    SceneBarycentrics2D out = {0};
    if (clip0 == nullptr || clip1 == nullptr || clip2 == nullptr ||
        viewport_w <= 0.0f || viewport_h <= 0.0f ||
        fabsf(clip0[3]) <= 1.0e-8f || fabsf(clip1[3]) <= 1.0e-8f || fabsf(clip2[3]) <= 1.0e-8f) {
        return out;
    }

    float iw0 = 1.0f / clip0[3];
    float iw1 = 1.0f / clip1[3];
    float iw2 = 1.0f / clip2[3];

    float s0x = clip0[0] * iw0, s0y = clip0[1] * iw0, z0 = clip0[2] * iw0;
    float s1x = clip1[0] * iw1, s1y = clip1[1] * iw1, z1 = clip1[2] * iw1;
    float s2x = clip2[0] * iw2, s2y = clip2[1] * iw2, z2 = clip2[2] * iw2;

    float p_ndc_x = (2.0f * px_center / viewport_w) - 1.0f;
    float p_ndc_y = (2.0f * py_center / viewport_h) - 1.0f;

    float e1x = s1x - s0x, e1y = s1y - s0y;
    float e2x = s2x - s0x, e2y = s2y - s0y;
    float det = e1x * e2y - e1y * e2x;
    if (fabsf(det) <= 1.0e-12f) {
        return out;
    }

    float inv_det = 1.0f / det;
    float dx = p_ndc_x - s0x;
    float dy = p_ndc_y - s0y;

    float u_scr = (dx * e2y - dy * e2x) * inv_det;
    float v_scr = (e1x * dy - e1y * dx) * inv_det;
    float w_scr = 1.0f - u_scr - v_scr;

    float du_dx = (2.0f * e2y / viewport_w) * inv_det;
    float du_dy = (-2.0f * e2x / viewport_h) * inv_det;
    float dv_dx = (-2.0f * e1y / viewport_w) * inv_det;
    float dv_dy = (2.0f * e1x / viewport_h) * inv_det;
    float dw_dx = -(du_dx + dv_dx);
    float dw_dy = -(du_dy + dv_dy);

    float n0 = w_scr * iw0;
    float n1 = u_scr * iw1;
    float n2 = v_scr * iw2;
    float z_inv = n0 + n1 + n2;
    if (fabsf(z_inv) <= 1.0e-12f) {
        return out;
    }

    float z_persp = 1.0f / z_inv;
    out.lambda[0] = n0 * z_persp;
    out.lambda[1] = n1 * z_persp;
    out.lambda[2] = n2 * z_persp;

    float dn0_dx = dw_dx * iw0, dn0_dy = dw_dy * iw0;
    float dn1_dx = du_dx * iw1, dn1_dy = du_dy * iw1;
    float dn2_dx = dv_dx * iw2, dn2_dy = dv_dy * iw2;
    float dzinv_dx = dn0_dx + dn1_dx + dn2_dx;
    float dzinv_dy = dn0_dy + dn1_dy + dn2_dy;

    out.dlambda_dx[0] = (dn0_dx - out.lambda[0] * dzinv_dx) * z_persp;
    out.dlambda_dx[1] = (dn1_dx - out.lambda[1] * dzinv_dx) * z_persp;
    out.dlambda_dx[2] = (dn2_dx - out.lambda[2] * dzinv_dx) * z_persp;

    out.dlambda_dy[0] = (dn0_dy - out.lambda[0] * dzinv_dy) * z_persp;
    out.dlambda_dy[1] = (dn1_dy - out.lambda[1] * dzinv_dy) * z_persp;
    out.dlambda_dy[2] = (dn2_dy - out.lambda[2] * dzinv_dy) * z_persp;

    out.z_ndc = w_scr * z0 + u_scr * z1 + v_scr * z2;
    out.valid = true;
    return out;
}

bool scene_self_check(void) {
    int16_t oct[2] = {0};
    float n_out[3] = {0};
    const float inv_sqrt3 = 0.577350269f;
    scene_oct_encode_snorm16(inv_sqrt3, -inv_sqrt3, -inv_sqrt3, oct);
    scene_oct_decode_snorm16(oct, n_out);
    float dot_v = inv_sqrt3 * n_out[0] - inv_sqrt3 * n_out[1] - inv_sqrt3 * n_out[2];
    if (dot_v < 0.9999f) {
        return false;
    }

    uint32_t vis = scene_visbuf_pack(0u, 17u, 93u);
    uint32_t i_id = 99u, m_id = 0u, p_id = 0u;
    if (vis == 0u || !scene_visbuf_unpack(vis, &i_id, &m_id, &p_id) ||
        i_id != 0u || m_id != 17u || p_id != 93u) {
        return false;
    }

    const float c0[4] = { -0.5f, -0.5f, 0.25f, 1.0f };
    const float c1[4] = {  0.5f, -0.5f, 0.50f, 1.0f };
    const float c2[4] = { -0.5f,  0.5f, 0.75f, 1.0f };
    SceneBarycentrics2D b = scene_eval_barycentrics_2d(c0, c1, c2, 480.0f, 400.0f, 1280.0f, 800.0f);
    if (!b.valid || fabsf((b.lambda[0] + b.lambda[1] + b.lambda[2]) - 1.0f) > 1.0e-5f) {
        return false;
    }
    return true;
}
