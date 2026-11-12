#include "lighting/lighting.h"
#include <math.h>
#include <stdbit.h>
#include <string.h>

static constexpr float k_pi      = 3.14159265358979323846f;
static constexpr float k_half_pi = 1.57079632679489661923f;

static float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

uint16_t lighting_f32_to_f16(float v) {
    uint32_t bits = 0u;
    memcpy(&bits, &v, sizeof bits);
    uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t  exp  = (int32_t)((bits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x007FFFFFu;

    if (exp <= 0) {
        if (exp < -10) {
            return (uint16_t)sign;
        }
        mant = (mant | 0x00800000u) >> (uint32_t)(1 - exp);
        if ((mant & 0x00001000u) != 0u) {
            mant += 0x00002000u;
        }
        return (uint16_t)(sign | (mant >> 13u));
    }
    if (exp >= 31) {
        if (((bits >> 23u) & 0xFFu) == 0xFFu && mant != 0u) {
            return (uint16_t)(sign | 0x7E00u); // Quiet NaN
        }
        return (uint16_t)(sign | 0x7C00u);     // Clamp overflow to Inf
    }
    if ((mant & 0x00001000u) != 0u) {
        mant += 0x00002000u;
        if ((mant & 0x00800000u) != 0u) {
            mant = 0u;
            exp += 1;
            if (exp >= 31) {
                return (uint16_t)(sign | 0x7C00u);
            }
        }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10u) | (mant >> 13u));
}

float lighting_f16_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16u;
    uint32_t exp  = (h >> 10u) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t out  = 0u;

    if (exp == 0u) {
        if (mant == 0u) {
            out = sign;
        } else {
            exp = 1u;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1u;
                exp -= 1u;
            }
            mant &= 0x03FFu;
            out = sign | ((exp + (127u - 15u)) << 23u) | (mant << 13u);
        }
    } else if (exp == 31u) {
        out = sign | 0x7F800000u | (mant << 13u);
    } else {
        out = sign | ((exp + (127u - 15u)) << 23u) | (mant << 13u);
    }

    float res = 0.0f;
    memcpy(&res, &out, sizeof res);
    return res;
}

// Khronos / Vulkan standard shared-exponent RGB9E5 [0 .. 65408]:
// 9-bit unsigned mantissas (R[8:0], G[17:9], B[26:18]) + 5-bit biased exponent E[31:27], bias=15.
uint32_t lighting_pack_rgb9e5(float r, float g, float b) {
    const float k_max_rgb9e5 = 65408.0f;
    float rc = isfinite(r) ? clampf(r, 0.0f, k_max_rgb9e5) : 0.0f;
    float gc = isfinite(g) ? clampf(g, 0.0f, k_max_rgb9e5) : 0.0f;
    float bc = isfinite(b) ? clampf(b, 0.0f, k_max_rgb9e5) : 0.0f;

    float max_c = fmaxf(rc, fmaxf(gc, bc));
    if (max_c < 1.52587890625e-5f) { // < 2^-16
        return 0u;
    }

    int exp_unbiased = 0;
    (void)frexpf(max_c, &exp_unbiased); // max_c = m * 2^exp_unbiased, m in [0.5, 1)
    int exp_shared = exp_unbiased + 15;
    if (exp_shared < 0) {
        exp_shared = 0;
    }
    if (exp_shared > 31) {
        exp_shared = 31;
    }

    float denom = ldexpf(1.0f, exp_shared - 15 - 9);
    uint32_t max_m = (uint32_t)lrintf(max_c / denom);
    if (max_m >= 512u && exp_shared < 31) {
        exp_shared += 1;
        denom *= 2.0f;
    }

    uint32_t rm = (uint32_t)clampf(roundf(rc / denom), 0.0f, 511.0f);
    uint32_t gm = (uint32_t)clampf(roundf(gc / denom), 0.0f, 511.0f);
    uint32_t bm = (uint32_t)clampf(roundf(bc / denom), 0.0f, 511.0f);
    return rm | (gm << 9u) | (bm << 18u) | ((uint32_t)exp_shared << 27u);
}

void lighting_unpack_rgb9e5(uint32_t packed, float out_rgb[3]) {
    uint32_t rm  =  packed         & 0x1FFu;
    uint32_t gm  = (packed >>  9u) & 0x1FFu;
    uint32_t bm  = (packed >> 18u) & 0x1FFu;
    int32_t  exp = (int32_t)((packed >> 27u) & 0x1Fu) - 15 - 9;
    float scale  = ldexpf(1.0f, exp);
    out_rgb[0]   = (float)rm * scale;
    out_rgb[1]   = (float)gm * scale;
    out_rgb[2]   = (float)bm * scale;
}

static void oct_encode_unit(float nx, float ny, float nz, float *out_ex, float *out_ey) {
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (len < 1.0e-8f) {
        *out_ex = 0.0f;
        *out_ey = 0.0f;
        return;
    }
    nx /= len;
    ny /= len;
    nz /= len;
    float inv_l1 = 1.0f / (fabsf(nx) + fabsf(ny) + fabsf(nz));
    float ex = nx * inv_l1;
    float ey = ny * inv_l1;
    if (nz < 0.0f) {
        float sx = ex >= 0.0f ? 1.0f : -1.0f;
        float sy = ey >= 0.0f ? 1.0f : -1.0f;
        float tx = (1.0f - fabsf(ey)) * sx;
        float ty = (1.0f - fabsf(ex)) * sy;
        ex = tx;
        ey = ty;
    }
    *out_ex = clampf(ex, -1.0f, 1.0f);
    *out_ey = clampf(ey, -1.0f, 1.0f);
}

static void oct_decode_unit(float ex, float ey, float out_nrm[3]) {
    ex = clampf(ex, -1.0f, 1.0f);
    ey = clampf(ey, -1.0f, 1.0f);
    float vx = ex;
    float vy = ey;
    float vz = 1.0f - fabsf(ex) - fabsf(ey);
    if (vz < 0.0f) {
        float sx = vx >= 0.0f ? 1.0f : -1.0f;
        float sy = vy >= 0.0f ? 1.0f : -1.0f;
        float tx = (1.0f - fabsf(vy)) * sx;
        float ty = (1.0f - fabsf(vx)) * sy;
        vx = tx;
        vy = ty;
    }
    float len = sqrtf(vx * vx + vy * vy + vz * vz);
    if (len < 1.0e-8f) {
        out_nrm[0] = 0.0f;
        out_nrm[1] = 0.0f;
        out_nrm[2] = 1.0f;
        return;
    }
    out_nrm[0] = vx / len;
    out_nrm[1] = vy / len;
    out_nrm[2] = vz / len;
}

uint32_t lighting_pack_oct_snorm16(float nx, float ny, float nz) {
    float ex = 0.0f;
    float ey = 0.0f;
    oct_encode_unit(nx, ny, nz, &ex, &ey);
    int16_t qx = (int16_t)lrintf(ex * 32767.0f);
    int16_t qy = (int16_t)lrintf(ey * 32767.0f);
    return ((uint32_t)(uint16_t)qx) | (((uint32_t)(uint16_t)qy) << 16u);
}

void lighting_unpack_oct_snorm16(uint32_t packed, float out_nrm[3]) {
    int16_t qx = (int16_t)(packed & 0xFFFFu);
    int16_t qy = (int16_t)((packed >> 16u) & 0xFFFFu);
    oct_decode_unit((float)qx / 32767.0f, (float)qy / 32767.0f, out_nrm);
}

uint16_t lighting_pack_oct_snorm8(float nx, float ny, float nz) {
    float ex = 0.0f;
    float ey = 0.0f;
    oct_encode_unit(nx, ny, nz, &ex, &ey);
    int8_t qx = (int8_t)lrintf(ex * 127.0f);
    int8_t qy = (int8_t)lrintf(ey * 127.0f);
    return ((uint16_t)(uint8_t)qx) | (((uint16_t)(uint8_t)qy) << 8u);
}

void lighting_unpack_oct_snorm8(uint16_t packed, float out_nrm[3]) {
    int8_t qx = (int8_t)(packed & 0xFFu);
    int8_t qy = (int8_t)((packed >> 8u) & 0xFFu);
    oct_decode_unit((float)qx / 127.0f, (float)qy / 127.0f, out_nrm);
}

uint32_t lighting_ssilvb_sector_mask(float alpha_min, float alpha_max) {
    float a0 = clampf(alpha_min, 0.0f, 1.0f);
    float a1 = clampf(alpha_max, 0.0f, 1.0f);
    if (a1 <= a0) {
        return 0u;
    }
    uint32_t b0  = (uint32_t)floorf(a0 * 32.0f);
    uint32_t b1  = (uint32_t)ceilf(a1 * 32.0f);
    if (b0 >= 32u) {
        return 0u;
    }
    if (b1 > 32u) {
        b1 = 32u;
    }
    if (b1 <= b0) {
        return 0u;
    }
    uint32_t cnt = b1 - b0;
    if (cnt >= 32u) {
        return 0xFFFFFFFFu;
    }
    return ((1u << cnt) - 1u) << b0;
}

LightingSsilvbSliceResult lighting_ssilvb_eval_slice(const float center_nrm[3],
                                                     float slice_cos,
                                                     float slice_sin,
                                                     float thickness,
                                                     const LightingSsilvbSample *samples,
                                                     uint32_t sample_count) {
    LightingSsilvbSliceResult res = {0};

    // Slice plane contains view vector V=(0,0,1) and tangent T=(slice_cos, slice_sin, 0).
    // Slice plane normal is P_n = (-slice_sin, slice_cos, 0).
    float pnx = -slice_sin;
    float pny =  slice_cos;
    float ndp = center_nrm[0] * pnx + center_nrm[1] * pny;
    float proj_x = center_nrm[0] - ndp * pnx;
    float proj_y = center_nrm[1] - ndp * pny;
    float proj_z = center_nrm[2];
    float proj_len = sqrtf(proj_x * proj_x + proj_y * proj_y + proj_z * proj_z);
    if (proj_len < 1.0e-5f) {
        res.visibility = 0.0f;
        return res;
    }

    float n_t = (proj_x * slice_cos + proj_y * slice_sin) / proj_len;
    float n_v = proj_z / proj_len;
    // Signed tilt of projected normal relative to V=(0,0,1) in [-pi/2, +pi/2].
    float n_ang = atan2f(n_t, fmaxf(n_v, 1.0e-4f));

    uint32_t vis_mask = 0u;
    float    ind_r    = 0.0f;
    float    ind_g    = 0.0f;
    float    ind_b    = 0.0f;
    float    thick    = fmaxf(thickness, 1.0e-3f);

    for (uint32_t i = 0u; i < sample_count && samples != nullptr; ++i) {
        const LightingSsilvbSample *s = &samples[i];
        float dx = s->delta_view[0];
        float dy = s->delta_view[1];
        float dz_front = s->delta_view[2];
        float dz_back  = dz_front - thick;

        float d_tan = dx * slice_cos + dy * slice_sin;
        float dist2 = dx * dx + dy * dy + dz_front * dz_front;
        if (dist2 < 1.0e-8f) {
            continue;
        }

        // Measure angle theta in [-pi/2, +pi/2] relative to the projected surface normal:
        // theta = 0 along projected normal, -pi/2 along -tangent horizon, +pi/2 along +tangent horizon.
        // In (T, V) slice coordinates, ray direction is (d_tan, dz), so angle from V toward +T is atan2(d_tan, dz)?
        // Careful: when d_tan > 0 and dz == 0, ray is at +pi/2 from V; when dz -> -inf (behind), ray approaches +-pi.
        float ang_f = atan2f(d_tan, dz_front) - n_ang;
        float ang_b = atan2f(d_tan, dz_back)  - n_ang;
        float lo    = fminf(ang_f, ang_b);
        float hi    = fmaxf(ang_f, ang_b);

        float alpha_min = (lo + k_half_pi) / k_pi;
        float alpha_max = (hi + k_half_pi) / k_pi;
        uint32_t hit_bits = lighting_ssilvb_sector_mask(alpha_min, alpha_max);
        uint32_t newly_occluded = hit_bits & ~vis_mask;
        if (newly_occluded != 0u) {
            float sector_w = (float)stdc_count_ones_ui(newly_occluded) * (1.0f / 32.0f);
            float inv_dist = 1.0f / sqrtf(dist2);
            float dir_to_center_x = -dx * inv_dist;
            float dir_to_center_y = -dy * inv_dist;
            float dir_to_center_z = -dz_front * inv_dist;
            float emit_cos = s->normal_view[0] * dir_to_center_x +
                             s->normal_view[1] * dir_to_center_y +
                             s->normal_view[2] * dir_to_center_z;
            // Allow both front-lit bounce and grazing emissive transfer
            float facing = clampf(emit_cos, 0.0f, 1.0f);
            float w = sector_w * facing;
            ind_r += s->radiance[0] * w;
            ind_g += s->radiance[1] * w;
            ind_b += s->radiance[2] * w;
            vis_mask |= hit_bits;
        }
    }

    uint32_t occ_bits = stdc_count_ones_ui(vis_mask);
    uint32_t unocc_lo = 16u - stdc_count_ones_ui(vis_mask & 0x0000FFFFu);
    uint32_t unocc_hi = 16u - stdc_count_ones_ui((vis_mask >> 16u) & 0x0000FFFFu);

    res.vis_mask        = vis_mask;
    res.visibility      = clampf(proj_len * (1.0f - (float)occ_bits * (1.0f / 32.0f)), 0.0f, 1.0f);
    res.bent_tilt       = ((float)unocc_hi - (float)unocc_lo) * (1.0f / 16.0f);
    res.indirect_rgb[0] = ind_r;
    res.indirect_rgb[1] = ind_g;
    res.indirect_rgb[2] = ind_b;
    return res;
}

void lighting_hrc_merge_ray(const float near_rad[3], float near_beta,
                            const float far_rad_a[3], const float far_rad_b[3],
                            float out_rad[3]) {
    float beta = clampf(near_beta, 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
        float far_avg = 0.5f * (far_rad_a[c] + far_rad_b[c]);
        out_rad[c] = near_rad[c] + beta * far_avg;
    }
}

void lighting_gtao_multibounce(float visibility, const float albedo[3], float out_ao_rgb[3]) {
    float v = clampf(visibility, 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
        float rho = clampf(albedo[c], 0.0f, 0.99f);
        // Jimenez et al. analytical multi-bounce polynomial fit
        float a =  2.0404f * rho - 0.3324f;
        float b = -4.7951f * rho + 0.6417f;
        float d =  2.7552f * rho + 0.6903f;
        out_ao_rgb[c] = clampf(fmaxf(v, ((v * a + b) * v + d) * v), 0.0f, 1.0f);
    }
}

static void pose_to_local(const float v[3], float pitch_rad, float yaw_rad, float out_local[3]) {
    float cy = cosf(yaw_rad);
    float sy = sinf(yaw_rad);
    float ry_x = v[0] * cy - v[2] * sy;
    float ry_y = v[1];
    float ry_z = v[0] * sy + v[2] * cy;

    float cx = cosf(pitch_rad);
    float sx = sinf(pitch_rad);
    out_local[0] =  ry_x;
    out_local[1] =  ry_y * cx + ry_z * sx;
    out_local[2] = -ry_y * sx + ry_z * cx;
}

static void local_to_pose(const float v[3], float pitch_rad, float yaw_rad, float out_world[3]) {
    float cx = cosf(pitch_rad);
    float sx = sinf(pitch_rad);
    float rx_x = v[0];
    float rx_y = v[1] * cx - v[2] * sx;
    float rx_z = v[1] * sx + v[2] * cx;

    float cy = cosf(yaw_rad);
    float sy = sinf(yaw_rad);
    out_world[0] =  rx_x * cy + rx_z * sy;
    out_world[1] =  rx_y;
    out_world[2] = -rx_x * sy + rx_z * cy;
}

bool lighting_ray_proxy_interval(const float ro[3], const float rd[3],
                                 float t_min, float t_max,
                                 const LightingProxy *proxy,
                                 float *out_t, float out_nrm[3]) {
    if (proxy == nullptr || t_max <= t_min) {
        return false;
    }
    float rel[3] = {
        ro[0] - proxy->center[0],
        ro[1] - proxy->center[1],
        ro[2] - proxy->center[2],
    };
    float lo[3];
    float ld[3];
    pose_to_local(rel, proxy->pitch_rad, proxy->yaw_rad, lo);
    pose_to_local(rd,  proxy->pitch_rad, proxy->yaw_rad, ld);

    if (proxy->kind == LIGHTING_PROXY_OBB) {
        float t0 = t_min;
        float t1 = t_max;
        int   hit_axis = -1;
        float hit_sign = 1.0f;

        for (int a = 0; a < 3; ++a) {
            float ext = fmaxf(proxy->extents[a], 1.0e-4f);
            if (fabsf(ld[a]) < 1.0e-8f) {
                if (lo[a] < -ext || lo[a] > ext) {
                    return false;
                }
                continue;
            }
            float inv_d = 1.0f / ld[a];
            float s0    = (-ext - lo[a]) * inv_d;
            float s1    = ( ext - lo[a]) * inv_d;
            float sign0 = -1.0f;
            if (s0 > s1) {
                float tmp = s0;
                s0 = s1;
                s1 = tmp;
                sign0 = 1.0f;
            }
            if (s0 > t0) {
                t0       = s0;
                hit_axis = a;
                hit_sign = sign0;
            }
            if (s1 < t1) {
                t1 = s1;
            }
            if (t0 > t1) {
                return false;
            }
        }
        if (t0 < t_min || t0 > t_max) {
            return false;
        }
        float ln[3] = { 0.0f, 0.0f, 0.0f };
        if (hit_axis >= 0) {
            ln[hit_axis] = hit_sign;
        } else {
            // Ray started inside OBB at t_min: pick closest face normal
            int best_a = 0;
            float best_d = 1.0e30f;
            for (int a = 0; a < 3; ++a) {
                float p = lo[a] + ld[a] * t0;
                float rem = fabsf(proxy->extents[a] - fabsf(p));
                if (rem < best_d) {
                    best_d = rem;
                    best_a = a;
                }
            }
            float p = lo[best_a] + ld[best_a] * t0;
            ln[best_a] = p >= 0.0f ? 1.0f : -1.0f;
        }
        if (out_t != nullptr) {
            *out_t = t0;
        }
        if (out_nrm != nullptr) {
            local_to_pose(ln, proxy->pitch_rad, proxy->yaw_rad, out_nrm);
        }
        return true;
    }

    // LIGHTING_PROXY_ELLIPSOID: (x/rx)^2 + (y/ry)^2 + (z/rz)^2 = 1
    float inv_r[3] = {
        1.0f / fmaxf(proxy->extents[0], 1.0e-4f),
        1.0f / fmaxf(proxy->extents[1], 1.0e-4f),
        1.0f / fmaxf(proxy->extents[2], 1.0e-4f),
    };
    float so[3] = { lo[0] * inv_r[0], lo[1] * inv_r[1], lo[2] * inv_r[2] };
    float sd[3] = { ld[0] * inv_r[0], ld[1] * inv_r[1], ld[2] * inv_r[2] };
    float qa    = sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2];
    if (qa < 1.0e-10f) {
        return false;
    }
    float qb   = so[0] * sd[0] + so[1] * sd[1] + so[2] * sd[2];
    float qc   = so[0] * so[0] + so[1] * so[1] + so[2] * so[2] - 1.0f;
    float disc = qb * qb - qa * qc;
    if (disc < 0.0f) {
        return false;
    }
    float root = sqrtf(disc);
    float t_hit = (-qb - root) / qa;
    if (t_hit < t_min || t_hit > t_max) {
        t_hit = (-qb + root) / qa;
        if (t_hit < t_min || t_hit > t_max) {
            return false;
        }
    }
    if (out_t != nullptr) {
        *out_t = t_hit;
    }
    if (out_nrm != nullptr) {
        float hp[3] = {
            lo[0] + ld[0] * t_hit,
            lo[1] + ld[1] * t_hit,
            lo[2] + ld[2] * t_hit,
        };
        float ln[3] = {
            hp[0] * inv_r[0] * inv_r[0],
            hp[1] * inv_r[1] * inv_r[1],
            hp[2] * inv_r[2] * inv_r[2],
        };
        float wn[3];
        local_to_pose(ln, proxy->pitch_rad, proxy->yaw_rad, wn);
        float nlen = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        if (nlen > 1.0e-8f) {
            out_nrm[0] = wn[0] / nlen;
            out_nrm[1] = wn[1] / nlen;
            out_nrm[2] = wn[2] / nlen;
        } else {
            out_nrm[0] = 0.0f;
            out_nrm[1] = 0.0f;
            out_nrm[2] = 1.0f;
        }
    }
    return true;
}

void lighting_proxy_fill(LightingProxy *out, uint32_t kind,
                         const float center[3], const float extents[3],
                         float pitch_rad, float yaw_rad, float roughness,
                         const float albedo[3], const float emissive[3]) {
    if (out == nullptr) {
        return;
    }
    const float ex = fmaxf(extents[0], 1.0e-4f);
    const float ey = fmaxf(extents[1], 1.0e-4f);
    const float ez = fmaxf(extents[2], 1.0e-4f);
    const float bound_r = (kind == LIGHTING_PROXY_OBB)
        ? (sqrtf(ex * ex + ey * ey + ez * ez) * 1.015f)
        : (fmaxf(ex, fmaxf(ey, ez)) * 1.015f);
    const uint16_t sp = lighting_f32_to_f16(sinf(pitch_rad));
    const uint16_t cp = lighting_f32_to_f16(cosf(pitch_rad));
    const uint16_t sy = lighting_f32_to_f16(sinf(yaw_rad));
    const uint16_t cy = lighting_f32_to_f16(cosf(yaw_rad));

    *out = (LightingProxy){
        .center           = { center[0], center[1], center[2] },
        .kind             = kind,
        .extents          = { ex, ey, ez },
        .roughness        = clampf(roughness, 0.04f, 1.0f),
        .pitch_rad        = pitch_rad,
        .yaw_rad          = yaw_rad,
        .alb_f16          = {
            lighting_f32_to_f16(clampf(albedo[0], 0.0f, 1.0f)),
            lighting_f32_to_f16(clampf(albedo[1], 0.0f, 1.0f)),
            lighting_f32_to_f16(clampf(albedo[2], 0.0f, 1.0f)),
        },
        .emi_f16          = {
            lighting_f32_to_f16(fmaxf(emissive[0], 0.0f)),
            lighting_f32_to_f16(fmaxf(emissive[1], 0.0f)),
            lighting_f32_to_f16(fmaxf(emissive[2], 0.0f)),
        },
        .bound_radius_f16 = lighting_f32_to_f16(bound_r),
        .trig_pitch       = (uint32_t)sp | ((uint32_t)cp << 16u),
        .trig_yaw         = (uint32_t)sy | ((uint32_t)cy << 16u),
    };
}

bool lighting_self_check(void) {
    // 1. IEEE fp16 round-trip check
    uint16_t h1 = lighting_f32_to_f16(0.625f);
    if (fabsf(lighting_f16_to_f32(h1) - 0.625f) > 1.0e-4f) {
        return false;
    }

    // 2. RGB9E5 HDR round-trip check
    uint32_t e5 = lighting_pack_rgb9e5(4.5f, 1.25f, 0.125f);
    float rgb[3] = {0};
    lighting_unpack_rgb9e5(e5, rgb);
    if (fabsf(rgb[0] - 4.5f) > 0.05f || fabsf(rgb[1] - 1.25f) > 0.03f ||
        fabsf(rgb[2] - 0.125f) > 0.02f) {
        return false;
    }

    // 3. Octahedral snorm16 & snorm8 normal codecs
    const float ref_n[3] = { 0.48f, -0.64f, 0.60f };
    float dec16[3] = {0};
    float dec8[3]  = {0};
    lighting_unpack_oct_snorm16(lighting_pack_oct_snorm16(ref_n[0], ref_n[1], ref_n[2]), dec16);
    lighting_unpack_oct_snorm8(lighting_pack_oct_snorm8(ref_n[0], ref_n[1], ref_n[2]), dec8);
    float dot16 = ref_n[0] * dec16[0] + ref_n[1] * dec16[1] + ref_n[2] * dec16[2];
    float dot8  = ref_n[0] * dec8[0]  + ref_n[1] * dec8[1]  + ref_n[2] * dec8[2];
    if (dot16 < 0.9999f || dot8 < 0.995f) {
        return false;
    }

    // 4. 32-Bit SSILVB sector mask & thin-occluder pass-through check
    if (lighting_ssilvb_sector_mask(0.0f, 1.0f) != 0xFFFFFFFFu ||
        lighting_ssilvb_sector_mask(0.0f, 0.5f) != 0x0000FFFFu ||
        lighting_ssilvb_sector_mask(0.5f, 1.0f) != 0xFFFF0000u) {
        return false;
    }

    const float up_n[3] = { 0.0f, 0.0f, 1.0f };
    const LightingSsilvbSample thin_pole = {
        .delta_view  = { 12.0f, 0.0f, 18.0f },
        .normal_view = { -1.0f, 0.0f, 0.0f },
        .radiance    = { 2.0f, 1.0f, 0.5f },
    };
    LightingSsilvbSliceResult sr = lighting_ssilvb_eval_slice(up_n, 1.0f, 0.0f, 6.0f, &thin_pole, 1u);
    if (sr.vis_mask == 0u || sr.vis_mask == 0xFFFFFFFFu ||
        sr.visibility <= 0.5f || sr.visibility >= 1.0f || sr.indirect_rgb[0] <= 0.0f) {
        return false;
    }

    // 5. Split Radiance Cascade interval merge & GTAO multi-bounce check
    const float near_l[3] = { 0.2f, 0.1f, 0.0f };
    const float far_a[3]  = { 0.8f, 0.6f, 0.4f };
    const float far_b[3]  = { 0.4f, 0.2f, 0.4f };
    float merged[3] = {0};
    lighting_hrc_merge_ray(near_l, 0.5f, far_a, far_b, merged);
    if (fabsf(merged[0] - 0.5f) > 1.0e-5f || fabsf(merged[1] - 0.3f) > 1.0e-5f ||
        fabsf(merged[2] - 0.2f) > 1.0e-5f) {
        return false;
    }

    const float alb[3] = { 0.8f, 0.2f, 0.2f };
    float ao_mb[3] = {0};
    lighting_gtao_multibounce(0.5f, alb, ao_mb);
    if (ao_mb[0] <= 0.5f || ao_mb[0] <= ao_mb[1]) {
        return false;
    }

    // 6. Analytical OBB & Ellipsoid interval oracle check
    LightingProxy obb = {0};
    const float c0[3] = { 0.0f, 0.0f, 100.0f };
    const float e0[3] = { 50.0f, 50.0f, 50.0f };
    const float em[3] = { 0.0f, 0.0f, 0.0f };
    lighting_proxy_fill(&obb, LIGHTING_PROXY_OBB, c0, e0, 0.0f, 0.0f, 0.35f, alb, em);
    const float ro[3] = { 0.0f, 0.0f, 250.0f };
    const float rd[3] = { 0.0f, 0.0f, -1.0f };
    float t_hit = 0.0f;
    float n_hit[3] = {0};
    if (!lighting_ray_proxy_interval(ro, rd, 10.0f, 300.0f, &obb, &t_hit, n_hit) ||
        fabsf(t_hit - 100.0f) > 1.0e-3f || n_hit[2] < 0.99f) {
        return false;
    }

    return true;
}
