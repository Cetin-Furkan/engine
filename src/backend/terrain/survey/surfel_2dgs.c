#include "backend/terrain/survey.h"
#include <math.h>
#include <string.h>

// ============================================================================
// Engine v0.2 Milestone 5: Oriented 2D Gaussian Splatting (2DGS) Geodetic Surfels
// Reference: Huang et al., "2D Gaussian Splatting for Geometrically Accurate
//            Radiance Fields" (SIGGRAPH 2024–2026) + Energy-Conserving Mip-2DGS
// ============================================================================

static constexpr float k_two_pi = 6.28318530717958647692f;

static float clampf_2dgs(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint16_t f32_to_f16_bits(float value) {
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof bits);
    uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t  exp  = (int32_t)((bits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x007FFFFFu;

    if (exp <= 0) {
        if (exp < -10) {
            return (uint16_t)sign;
        }
        mant = (mant | 0x00800000u) >> (uint32_t)(1 - exp);
        return (uint16_t)(sign | ((mant + 0x00001000u) >> 13u));
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7BFFu);
    }
    uint32_t rounded = mant + 0x00001000u;
    if ((rounded & 0x00800000u) != 0u) {
        rounded = 0u;
        exp += 1;
        if (exp >= 31) {
            return (uint16_t)(sign | 0x7BFFu);
        }
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10u) | (rounded >> 13u));
}

static float f16_bits_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16u;
    uint32_t exp  = (uint32_t)((h >> 10u) & 0x1Fu);
    uint32_t mant = (uint32_t)(h & 0x03FFu);
    uint32_t bits = 0u;

    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            int32_t e = -14;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1u;
                e -= 1;
            }
            mant &= 0x03FFu;
            bits = sign | ((uint32_t)(e + 127) << 23u) | (mant << 13u);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13u);
    } else {
        bits = sign | ((exp + (127u - 15u)) << 23u) | (mant << 13u);
    }

    float out = 0.0f;
    memcpy(&out, &bits, sizeof out);
    return out;
}

static uint16_t pack_oct_snorm8(const float n_in[3]) {
    float nx = (n_in != nullptr) ? n_in[0] : 0.0f;
    float ny = (n_in != nullptr) ? n_in[1] : 0.0f;
    float nz = (n_in != nullptr) ? n_in[2] : 1.0f;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    if (!(len > 1.0e-8f)) {
        nx = 0.0f;
        ny = 0.0f;
        nz = 1.0f;
    } else {
        float inv = 1.0f / len;
        nx *= inv;
        ny *= inv;
        nz *= inv;
    }

    float inv_l1 = 1.0f / fmaxf(fabsf(nx) + fabsf(ny) + fabsf(nz), 1.0e-8f);
    float ex     = nx * inv_l1;
    float ey     = ny * inv_l1;
    if (nz < 0.0f) {
        float ox = (1.0f - fabsf(ey)) * (ex >= 0.0f ? 1.0f : -1.0f);
        float oy = (1.0f - fabsf(ex)) * (ey >= 0.0f ? 1.0f : -1.0f);
        ex = ox;
        ey = oy;
    }

    int32_t qx = (int32_t)lrintf(clampf_2dgs(ex, -1.0f, 1.0f) * 127.0f);
    int32_t qy = (int32_t)lrintf(clampf_2dgs(ey, -1.0f, 1.0f) * 127.0f);
    uint8_t bx = (uint8_t)(int8_t)qx;
    uint8_t by = (uint8_t)(int8_t)qy;
    return (uint16_t)bx | ((uint16_t)by << 8u);
}

static void unpack_oct_snorm8(uint16_t packed16, float out_n[3]) {
    int8_t qx = (int8_t)(packed16 & 0xFFu);
    int8_t qy = (int8_t)((packed16 >> 8u) & 0xFFu);
    float  ex = clampf_2dgs((float)qx * (1.0f / 127.0f), -1.0f, 1.0f);
    float  ey = clampf_2dgs((float)qy * (1.0f / 127.0f), -1.0f, 1.0f);
    float  vx = ex;
    float  vy = ey;
    float  vz = 1.0f - fabsf(ex) - fabsf(ey);
    if (vz < 0.0f) {
        float sx = vx >= 0.0f ? 1.0f : -1.0f;
        float sy = vy >= 0.0f ? 1.0f : -1.0f;
        float ox = (1.0f - fabsf(vy)) * sx;
        float oy = (1.0f - fabsf(vx)) * sy;
        vx = ox;
        vy = oy;
    }
    float len = sqrtf(vx * vx + vy * vy + vz * vz);
    if (len > 1.0e-8f) {
        float inv = 1.0f / len;
        out_n[0] = vx * inv;
        out_n[1] = vy * inv;
        out_n[2] = vz * inv;
    } else {
        out_n[0] = 0.0f;
        out_n[1] = 0.0f;
        out_n[2] = 1.0f;
    }
}

// Duff et al. (JCGT 2017) branchless orthonormal basis around unit normal N
static void build_frisvad_onb(const float n[3], float b1[3], float b2[3]) {
    float s = (n[2] >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (s + n[2]);
    float b = n[0] * n[1] * a;

    b1[0] = 1.0f + s * n[0] * n[0] * a;
    b1[1] = s * b;
    b1[2] = -s * n[0];

    b2[0] = b;
    b2[1] = s + n[1] * n[1] * a;
    b2[2] = -n[1];
}

static float tangent_angle_for_dir(const float n_unit[3], const float dir[3]) {
    uint16_t oct = pack_oct_snorm8(n_unit);
    float    nq[3];
    float    b1[3];
    float    b2[3];
    unpack_oct_snorm8(oct, nq);
    build_frisvad_onb(nq, b1, b2);

    float c = dir[0] * b1[0] + dir[1] * b1[1] + dir[2] * b1[2];
    float s = dir[0] * b2[0] + dir[1] * b2[1] + dir[2] * b2[2];
    if (c * c + s * s < 1.0e-10f) {
        return 0.0f;
    }
    float phi = atan2f(s, c);
    if (phi < 0.0f) {
        phi += k_two_pi;
    }
    return phi;
}

void backend_surfel_2dgs_pack(TerrainSurfel2DGS *out,
                              float              wx,
                              float              wy,
                              float              elev_m,
                              const float        normal[3],
                              float              tangent_angle_rad,
                              float              sigma_u_m,
                              float              sigma_v_m,
                              uint8_t            r,
                              uint8_t            g,
                              uint8_t            b,
                              uint8_t            opacity_u8,
                              uint16_t           node_id,
                              uint16_t           flags) {
    if (out == nullptr) {
        return;
    }
    float phi = fmodf(tangent_angle_rad, k_two_pi);
    if (phi < 0.0f) {
        phi += k_two_pi;
    }
    uint16_t oct16 = pack_oct_snorm8(normal);
    uint16_t phi16 = (uint16_t)lrintf(clampf_2dgs(phi / k_two_pi, 0.0f, 1.0f) * 65535.0f);

    float su_clamped = clampf_2dgs(sigma_u_m, 0.05f, 4096.0f);
    float sv_clamped = clampf_2dgs(sigma_v_m, 0.05f, 4096.0f);
    uint16_t su16    = f32_to_f16_bits(su_clamped);
    uint16_t sv16    = f32_to_f16_bits(sv_clamped);

    *out = (TerrainSurfel2DGS){
        .center_x        = wx,
        .center_y        = wy,
        .elev_m          = elev_m,
        .nrm_tan_packed  = (uint32_t)oct16 | ((uint32_t)phi16 << 16u),
        .scale_uv_f16    = (uint32_t)su16 | ((uint32_t)sv16 << 16u),
        .rgba_opa_packed = (uint32_t)r | ((uint32_t)g << 8u) |
                           ((uint32_t)b << 16u) | ((uint32_t)opacity_u8 << 24u),
        .flags_node_id   = (uint32_t)node_id | ((uint32_t)flags << 16u),
        .reserved_pad    = 0u,
    };
}

void backend_surfel_2dgs_unpack_basis(const TerrainSurfel2DGS *surfel,
                                      float                    out_nrm[3],
                                      float                    out_tan_u[3],
                                      float                    out_tan_v[3],
                                      float                   *out_sigma_u_m,
                                      float                   *out_sigma_v_m) {
    if (surfel == nullptr) {
        return;
    }
    float n[3];
    unpack_oct_snorm8((uint16_t)(surfel->nrm_tan_packed & 0xFFFFu), n);

    float b1[3];
    float b2[3];
    build_frisvad_onb(n, b1, b2);

    uint16_t phi16 = (uint16_t)((surfel->nrm_tan_packed >> 16u) & 0xFFFFu);
    float    phi   = ((float)phi16 * (1.0f / 65535.0f)) * k_two_pi;
    float    cp    = cosf(phi);
    float    sp    = sinf(phi);

    if (out_nrm != nullptr) {
        out_nrm[0] = n[0];
        out_nrm[1] = n[1];
        out_nrm[2] = n[2];
    }
    if (out_tan_u != nullptr) {
        out_tan_u[0] = cp * b1[0] + sp * b2[0];
        out_tan_u[1] = cp * b1[1] + sp * b2[1];
        out_tan_u[2] = cp * b1[2] + sp * b2[2];
    }
    if (out_tan_v != nullptr) {
        out_tan_v[0] = -sp * b1[0] + cp * b2[0];
        out_tan_v[1] = -sp * b1[1] + cp * b2[1];
        out_tan_v[2] = -sp * b1[2] + cp * b2[2];
    }
    if (out_sigma_u_m != nullptr) {
        *out_sigma_u_m = fmaxf(f16_bits_to_f32((uint16_t)(surfel->scale_uv_f16 & 0xFFFFu)), 0.05f);
    }
    if (out_sigma_v_m != nullptr) {
        *out_sigma_v_m = fmaxf(f16_bits_to_f32((uint16_t)((surfel->scale_uv_f16 >> 16u) & 0xFFFFu)), 0.05f);
    }
}

bool backend_surfel_2dgs_eval_ray(const TerrainSurfel2DGS *surfel,
                                  float                    elev_base_m,
                                  float                    vertical_scale,
                                  const float              ray_origin[3],
                                  const float              ray_dir[3],
                                  float                    filter_sigma_m,
                                  TerrainSurfelRayHit     *out_hit) {
    if (out_hit != nullptr) {
        *out_hit = (TerrainSurfelRayHit){0};
    }
    if (surfel == nullptr || ray_origin == nullptr || ray_dir == nullptr) {
        return false;
    }

    float dlen = sqrtf(ray_dir[0] * ray_dir[0] +
                       ray_dir[1] * ray_dir[1] +
                       ray_dir[2] * ray_dir[2]);
    if (!(dlen > 1.0e-8f)) {
        return false;
    }
    float dx = ray_dir[0] / dlen;
    float dy = ray_dir[1] / dlen;
    float dz = ray_dir[2] / dlen;

    float nrm[3];
    float tan_u[3];
    float tan_v[3];
    float su = 1.0f;
    float sv = 1.0f;
    backend_surfel_2dgs_unpack_basis(surfel, nrm, tan_u, tan_v, &su, &sv);

    float denom = dx * nrm[0] + dy * nrm[1] + dz * nrm[2];
    if (fabsf(denom) < 1.0e-6f) {
        return false;
    }

    float cx = surfel->center_x;
    float cy = surfel->center_y;
    float cz = (surfel->elev_m - elev_base_m) * vertical_scale;

    float oc_x = cx - ray_origin[0];
    float oc_y = cy - ray_origin[1];
    float oc_z = cz - ray_origin[2];
    float t    = (oc_x * nrm[0] + oc_y * nrm[1] + oc_z * nrm[2]) / denom;
    if (!(t > 0.0f)) {
        return false;
    }

    float hx = (ray_origin[0] + dx * t) - cx;
    float hy = (ray_origin[1] + dy * t) - cy;
    float hz = (ray_origin[2] + dz * t) - cz;

    float u_m = hx * tan_u[0] + hy * tan_u[1] + hz * tan_u[2];
    float v_m = hx * tan_v[0] + hy * tan_v[1] + hz * tan_v[2];

    float sf     = fmaxf(filter_sigma_m, 0.0f);
    float eff_su = sqrtf(su * su + sf * sf);
    float eff_sv = sqrtf(sv * sv + sf * sf);
    float mip_k  = sqrtf((su * sv) / fmaxf(eff_su * eff_sv, 1.0e-8f));

    float u_norm = u_m / eff_su;
    float v_norm = v_m / eff_sv;
    float r2     = u_norm * u_norm + v_norm * v_norm;

    constexpr float k_cutoff_r2 = 2.75f * 2.75f; // 7.5625
    if (r2 >= k_cutoff_r2) {
        return false;
    }

    float win_t  = clampf_2dgs((k_cutoff_r2 - r2) / (k_cutoff_r2 - 5.0f), 0.0f, 1.0f);
    float window = win_t * win_t * (3.0f - 2.0f * win_t);
    float base_a = (float)((surfel->rgba_opa_packed >> 24u) & 0xFFu) * (1.0f / 255.0f);
    float alpha  = clampf_2dgs(base_a * mip_k * expf(-0.5f * r2) * window, 0.0f, 1.0f);

    if (out_hit != nullptr) {
        *out_hit = (TerrainSurfelRayHit){
            .t_ray  = t,
            .u_norm = u_norm,
            .v_norm = v_norm,
            .alpha  = alpha,
            .valid  = alpha > 1.0e-4f,
        };
    }
    return alpha > 1.0e-4f;
}

uint32_t backend_surfel_2dgs_extract_tile(const GpuMathQuadNode *node,
                                          uint32_t               node_index,
                                          const EpackTileBlock  *tile,
                                          float                  vertical_scale,
                                          uint32_t               sample_stride,
                                          TerrainSurfel2DGS     *out_surfels,
                                          uint32_t               max_surfels) {
    if (node == nullptr || tile == nullptr || out_surfels == nullptr || max_surfels == 0u) {
        return 0u;
    }

    uint32_t stride = sample_stride < 2u ? 2u : (sample_stride > 32u ? 32u : sample_stride);
    float    span_x = fmaxf(node->world_max_x - node->world_min_x, 1.0f);
    float    span_y = fmaxf(node->world_max_y - node->world_min_y, 1.0f);
    float    step_x = span_x / (float)(k_gmpack_tile_dim - 1u);
    float    step_y = span_y / (float)(k_gmpack_tile_dim - 1u);
    float    step_m = 0.5f * (step_x + step_y);
    float    vscale = vertical_scale > 0.05f ? vertical_scale : 1.0f;

    uint32_t written = 0u;
    for (uint32_t sy = stride; sy + stride < k_gmpack_tile_dim && written < max_surfels; sy += stride) {
        float v_tile = (float)sy / (float)(k_gmpack_tile_dim - 1u);
        float wy     = node->world_min_y + v_tile * span_y;

        for (uint32_t sx = stride; sx + stride < k_gmpack_tile_dim && written < max_surfels; sx += stride) {
            float hc = backend_epack_load_height_m(tile, sx, sy);
            float hl = backend_epack_load_height_m(tile, sx - 1u, sy);
            float hr = backend_epack_load_height_m(tile, sx + 1u, sy);
            float hd = backend_epack_load_height_m(tile, sx, sy - 1u);
            float hu = backend_epack_load_height_m(tile, sx, sy + 1u);

            float dh_dx = (hr - hl) / (2.0f * step_x);
            float dh_dy = (hu - hd) / (2.0f * step_y);
            float grad  = sqrtf(dh_dx * dh_dx + dh_dy * dh_dy);
            float lap   = fabsf(hl + hr + hd + hu - 4.0f * hc) / fmaxf(step_m, 0.5f);

            // Concentrate 2DGS surfels on steep limestone cliffs, canyon walls, and sharp karst ridges
            if (grad < 0.34f && !(grad >= 0.20f && lap >= 0.16f)) {
                continue;
            }

            float u_tile = (float)sx / (float)(k_gmpack_tile_dim - 1u);
            float wx     = node->world_min_x + u_tile * span_x;

            float dzdx = dh_dx * vscale;
            float dzdy = dh_dy * vscale;
            float inv_n = 1.0f / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.0f);
            float nrm[3] = { -dzdx * inv_n, -dzdy * inv_n, inv_n };

            // Align principal tangent axis T_u with cliff contour strike (-dzdy, dzdx, 0)
            float strike[3] = { -dzdy, dzdx, 0.0f };
            float phi       = tangent_angle_for_dir(nrm, strike);

            float cell_span = step_m * (float)stride;
            float sigma_u   = fminf(cell_span * 0.64f, 64.0f);
            float dip_scale = fminf(sqrtf(1.0f + dzdx * dzdx + dzdy * dzdy), 2.4f);
            float sigma_v   = fminf(cell_span * 0.45f * dip_scale, 48.0f);

            uint32_t rgba = backend_epack_load_rgba8(tile, sx, sy);
            float strata  = 1.0f + 0.05f * cosf(hc * 0.42f);
            uint8_t r_u8  = (uint8_t)lrintf(clampf_2dgs((float)( rgba         & 0xFFu) * strata, 0.0f, 255.0f));
            uint8_t g_u8  = (uint8_t)lrintf(clampf_2dgs((float)((rgba >>  8u) & 0xFFu) * strata, 0.0f, 255.0f));
            uint8_t b_u8  = (uint8_t)lrintf(clampf_2dgs((float)((rgba >> 16u) & 0xFFu) * strata, 0.0f, 255.0f));

            float opa_f   = clampf_2dgs((grad - 0.26f) * 0.72f + lap * 0.22f, 0.14f, 0.54f);
            uint8_t opa   = (uint8_t)lrintf(opa_f * 255.0f);

            backend_surfel_2dgs_pack(&out_surfels[written++],
                                     wx, wy, hc,
                                     nrm, phi,
                                     sigma_u, sigma_v,
                                     r_u8, g_u8, b_u8, opa,
                                     (uint16_t)(node_index & 0xFFFFu),
                                     (uint16_t)k_surfel_flag_cliff);
        }
    }
    return written;
}

uint32_t backend_surfel_2dgs_extract_survey(const TerrainSurveyDataset *ds,
                                            TerrainSurfel2DGS          *out_surfels,
                                            uint32_t                    max_surfels) {
    if (ds == nullptr || out_surfels == nullptr || max_surfels == 0u) {
        return 0u;
    }

    uint32_t written = 0u;
    const float up_nrm[3] = { 0.0f, 0.0f, 1.0f };

    // 1. Geodetic Ground Control Points (GCPs) & LiDAR/CSV survey posts
    for (uint32_t i = 0u; i < ds->point_count && written < max_surfels; ++i) {
        const TerrainSurveyPoint *pt = &ds->points[i];
        float   sigma = clampf_2dgs(pt->radius_m * 0.16f, 1.5f, 22.0f);
        uint8_t r     = pt->has_color ? pt->r : 244u;
        uint8_t g     = pt->has_color ? pt->g : 168u;
        uint8_t b     = pt->has_color ? pt->b : 44u;
        uint8_t opa   = (uint8_t)lrintf(clampf_2dgs(pt->weight * 0.68f, 0.20f, 0.75f) * 255.0f);
        backend_surfel_2dgs_pack(&out_surfels[written++],
                                 pt->world_x, pt->world_y, pt->elevation_m,
                                 up_nrm, 0.0f,
                                 sigma, sigma,
                                 r, g, b, opa,
                                 0u, (uint16_t)k_surfel_flag_gcp);
    }

    // 2. Triangulated multi-view photogrammetric tie-points
    for (uint32_t i = 0u; i < ds->tiepoint_count && written < max_surfels; ++i) {
        const TerrainStereoTiePoint *tp = &ds->tiepoints[i];
        if (!tp->valid) {
            continue;
        }
        float   sigma = clampf_2dgs(tp->radius_m * 0.14f, 1.2f, 18.0f);
        uint8_t opa   = (uint8_t)lrintf(clampf_2dgs(tp->weight * 0.72f, 0.25f, 0.78f) * 255.0f);
        backend_surfel_2dgs_pack(&out_surfels[written++],
                                 tp->triangulated_x, tp->triangulated_y, tp->triangulated_elev_m,
                                 up_nrm, 0.0f,
                                 sigma, sigma,
                                 64u, 226u, 248u, opa,
                                 0u, (uint16_t)k_surfel_flag_tiepoint);
    }

    // 3. Anisotropic ridge / escarpment breaklines
    for (uint32_t i = 0u; i < ds->breakline_count && written < max_surfels; ++i) {
        const TerrainSurveyBreakline *bl = &ds->breaklines[i];
        float dx  = bl->world_bx - bl->world_ax;
        float dy  = bl->world_by - bl->world_ay;
        float dz  = bl->elev_b_m - bl->elev_a_m;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f) {
            continue;
        }
        float seg_step = clampf_2dgs(bl->radius_m * 0.45f, 6.0f, 36.0f);
        uint32_t n_seg = (uint32_t)ceilf(len / seg_step);
        if (n_seg < 1u) {
            n_seg = 1u;
        } else if (n_seg > 32u) {
            n_seg = 32u;
        }

        float dir3[3] = { dx / len, dy / len, 0.0f };
        float phi     = tangent_angle_for_dir(up_nrm, dir3);
        float su      = (len / (float)n_seg) * 0.65f;
        float sv      = clampf_2dgs(bl->radius_m * 0.15f, 1.5f, 14.0f);
        uint8_t opa   = (uint8_t)lrintf(clampf_2dgs(bl->weight * 0.62f, 0.18f, 0.70f) * 255.0f);

        for (uint32_t s = 0u; s < n_seg && written < max_surfels; ++s) {
            float t  = ((float)s + 0.5f) / (float)n_seg;
            float wx = bl->world_ax + dx * t;
            float wy = bl->world_ay + dy * t;
            float ez = bl->elev_a_m + dz * t;
            backend_surfel_2dgs_pack(&out_surfels[written++],
                                     wx, wy, ez,
                                     up_nrm, phi,
                                     su, sv,
                                     250u, 206u, 72u, opa,
                                     0u, (uint16_t)k_surfel_flag_breakln);
        }
    }

    return written;
}

bool backend_surfel_2dgs_self_check(void) {
    // 1. Orthonormal basis & fp16 scale roundtrip check on a steep oblique cliff normal
    const float raw_n[3] = { -0.62f, 0.48f, 0.6203225f };
    TerrainSurfel2DGS s  = {0};
    backend_surfel_2dgs_pack(&s,
                             1595.0f, -103.0f, 1650.0f,
                             raw_n, 1.15f,
                             8.5f, 3.25f,
                             180u, 165u, 142u, 204u,
                             42u, (uint16_t)k_surfel_flag_cliff);

    float n[3], tu[3], tv[3];
    float su = 0.0f, sv = 0.0f;
    backend_surfel_2dgs_unpack_basis(&s, n, tu, tv, &su, &sv);

    float dot_nu = n[0] * tu[0] + n[1] * tu[1] + n[2] * tu[2];
    float dot_nv = n[0] * tv[0] + n[1] * tv[1] + n[2] * tv[2];
    float dot_uv = tu[0] * tv[0] + tu[1] * tv[1] + tu[2] * tv[2];
    if (fabsf(dot_nu) > 1.0e-4f || fabsf(dot_nv) > 1.0e-4f || fabsf(dot_uv) > 1.0e-4f) {
        return false;
    }
    if (fabsf(su - 8.5f) > 0.02f || fabsf(sv - 3.25f) > 0.02f) {
        return false;
    }

    // 2. Exact 2DGS view-angle invariance check:
    //    Shoot two rays from completely different viewing angles (frontal 0 deg vs grazing 78 deg)
    //    aimed at the EXACT SAME tangent-plane point P = C + 0.6*su*T_u - 0.4*sv*T_v.
    //    Unlike 3DGS affine EWA splatting, 2DGS ray-plane intersection must recover identical (u, v, alpha)!
    const float base_m = 810.0f;
    const float vscale = 1.15f;
    const float cz     = (s.elev_m - base_m) * vscale;
    const float target[3] = {
        s.center_x + 0.6f * su * tu[0] - 0.4f * sv * tv[0],
        s.center_y + 0.6f * su * tu[1] - 0.4f * sv * tv[1],
        cz         + 0.6f * su * tu[2] - 0.4f * sv * tv[2],
    };

    const float orig_frontal[3] = {
        target[0] + n[0] * 120.0f,
        target[1] + n[1] * 120.0f,
        target[2] + n[2] * 120.0f,
    };
    const float dir_frontal[3] = { -n[0], -n[1], -n[2] };

    // Grazing ray at ~78 degrees from normal
    const float graze_vec[3] = {
        n[0] * 0.20f + tu[0] * 0.98f,
        n[1] * 0.20f + tu[1] * 0.98f,
        n[2] * 0.20f + tu[2] * 0.98f,
    };
    const float orig_graze[3] = {
        target[0] + graze_vec[0] * 250.0f,
        target[1] + graze_vec[1] * 250.0f,
        target[2] + graze_vec[2] * 250.0f,
    };
    const float dir_graze[3] = { -graze_vec[0], -graze_vec[1], -graze_vec[2] };

    TerrainSurfelRayHit hit_front = {0};
    TerrainSurfelRayHit hit_graze = {0};
    if (!backend_surfel_2dgs_eval_ray(&s, base_m, vscale, orig_frontal, dir_frontal, 0.25f, &hit_front) ||
        !backend_surfel_2dgs_eval_ray(&s, base_m, vscale, orig_graze, dir_graze, 0.25f, &hit_graze)) {
        return false;
    }
    if (fabsf(hit_front.u_norm - hit_graze.u_norm) > 1.0e-3f ||
        fabsf(hit_front.v_norm - hit_graze.v_norm) > 1.0e-3f ||
        fabsf(hit_front.alpha  - hit_graze.alpha)  > 1.0e-3f) {
        return false;
    }
    return true;
}
