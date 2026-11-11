#include "backend/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// Multi-View Stereo (MVS) Patch-ZNCC, Geodetic RBF/IDW & Ortho-Imagery Fusion
// ============================================================================

static constexpr float k_rad_to_deg = 57.2957795130823208768f;

void backend_survey_dataset_init(TerrainSurveyDataset *ds) {
    if (ds == nullptr) {
        return;
    }
    memset(ds, 0, sizeof(*ds));
    ds->mvs_search_range_m = 20.0f;
    ds->mvs_step_m         = 0.50f;
    ds->mvs_min_ncc        = 0.55f;
    ds->persist_gmpack     = false;
}

void backend_survey_dataset_free(TerrainSurveyDataset *ds) {
    if (ds == nullptr) {
        return;
    }
    for (uint32_t i = 0u; i < ds->photo_count && i < k_survey_max_photos; ++i) {
        free(ds->photos[i].rgba_pixels);
        ds->photos[i].rgba_pixels = nullptr;
    }
    for (uint32_t i = 0u; i < ds->ortho_overlay_count && i < k_survey_max_ortho_overlays; ++i) {
        free(ds->ortho_overlays[i].rgba_pixels);
        ds->ortho_overlays[i].rgba_pixels = nullptr;
    }
    free(ds->points);
    ds->points = nullptr;
    memset(ds, 0, sizeof(*ds));
}

int backend_survey_add_ortho_overlay(TerrainSurveyDataset      *ds,
                                     const TerrainOrthoOverlay *ov_spec) {
    if (ds == nullptr || ov_spec == nullptr || ov_spec->rgba_pixels == nullptr ||
        ov_spec->width_px == 0u || ov_spec->height_px == 0u) {
        return -1;
    }
    TerrainOrthoOverlay ov = *ov_spec;
    ov.rgba_pixels = nullptr;
    if (!isfinite(ov.half_span_x) || ov.half_span_x <= 0.5f) {
        ov.half_span_x = 300.0f;
    }
    if (!isfinite(ov.half_span_y) || ov.half_span_y <= 0.5f) {
        ov.half_span_y = 300.0f;
    }
    if (!isfinite(ov.blend_weight) || ov.blend_weight <= 0.0f) {
        ov.blend_weight = 1.0f;
    } else if (ov.blend_weight > 1.0f) {
        ov.blend_weight = 1.0f;
    }

    int replace_idx = -1;
    if (ov.image_path[0] != '\0') {
        const char *ov_base = strrchr(ov.image_path, '/');
        ov_base = (ov_base != nullptr) ? (ov_base + 1) : ov.image_path;
        for (uint32_t i = 0u; i < ds->ortho_overlay_count && i < k_survey_max_ortho_overlays; ++i) {
            const char *cur_path = ds->ortho_overlays[i].image_path;
            if (cur_path[0] == '\0') {
                continue;
            }
            const char *cur_base = strrchr(cur_path, '/');
            cur_base = (cur_base != nullptr) ? (cur_base + 1) : cur_path;
            if (strcmp(cur_path, ov.image_path) == 0 || strcmp(cur_base, ov_base) == 0) {
                replace_idx = (int)i;
                break;
            }
        }
    }
    if (replace_idx < 0 && ds->ortho_overlay_count >= k_survey_max_ortho_overlays) {
        return -1;
    }

    size_t count = (size_t)ov.width_px * (size_t)ov.height_px;
    uint32_t *copy = malloc(count * sizeof(uint32_t));
    if (copy == nullptr) {
        return -1;
    }
    memcpy(copy, ov_spec->rgba_pixels, count * sizeof(uint32_t));
    ov.rgba_pixels = copy;

    if (replace_idx >= 0) {
        free(ds->ortho_overlays[replace_idx].rgba_pixels);
        ds->ortho_overlays[replace_idx] = ov;
        return replace_idx;
    }
    ds->ortho_overlays[ds->ortho_overlay_count++] = ov;
    return (int)(ds->ortho_overlay_count - 1u);
}

static void catmull_rom_w(float t, float out_w[4]) {
    float t2 = t * t;
    float t3 = t2 * t;
    out_w[0] = -0.5f * t + t2 - 0.5f * t3;
    out_w[1] =  1.0f - 2.5f * t2 + 1.5f * t3;
    out_w[2] =  0.5f * t + 2.0f * t2 - 1.5f * t3;
    out_w[3] = -0.5f * t2 + 0.5f * t3;
}

uint32_t backend_survey_bake_ortho_tile(const GpuMathQuadNode *node,
                                        GpuMathTileBlock      *tile,
                                        const uint32_t        *rgba_pixels,
                                        uint32_t               width_px,
                                        uint32_t               height_px,
                                        float                  center_x,
                                        float                  center_y,
                                        float                  half_span_x,
                                        float                  half_span_y,
                                        float                  blend_weight) {
    if (node == nullptr || tile == nullptr || rgba_pixels == nullptr ||
        width_px == 0u || height_px == 0u || half_span_x <= 0.5f || half_span_y <= 0.5f) {
        return 0u;
    }
    float ov_min_x = center_x - half_span_x;
    float ov_max_x = center_x + half_span_x;
    float ov_min_y = center_y - half_span_y;
    float ov_max_y = center_y + half_span_y;

    if (ov_max_x < node->world_min_x || ov_min_x > node->world_max_x ||
        ov_max_y < node->world_min_y || ov_min_y > node->world_max_y) {
        return 0u;
    }

    float span_x = fmaxf(node->world_max_x - node->world_min_x, 1.0f);
    float span_y = fmaxf(node->world_max_y - node->world_min_y, 1.0f);
    float u0 = fminf(fmaxf((ov_min_x - node->world_min_x) / span_x, 0.0f), 1.0f);
    float u1 = fminf(fmaxf((ov_max_x - node->world_min_x) / span_x, 0.0f), 1.0f);
    float v0 = fminf(fmaxf((ov_min_y - node->world_min_y) / span_y, 0.0f), 1.0f);
    float v1 = fminf(fmaxf((ov_max_y - node->world_min_y) / span_y, 0.0f), 1.0f);

    uint32_t sx0 = (uint32_t)floorf(u0 * (float)(k_gmpack_tile_dim - 1u));
    uint32_t sx1 = (uint32_t)ceilf(u1 * (float)(k_gmpack_tile_dim - 1u));
    uint32_t sy0 = (uint32_t)floorf(v0 * (float)(k_gmpack_tile_dim - 1u));
    uint32_t sy1 = (uint32_t)ceilf(v1 * (float)(k_gmpack_tile_dim - 1u));
    if (sx1 >= k_gmpack_tile_dim) sx1 = k_gmpack_tile_dim - 1u;
    if (sy1 >= k_gmpack_tile_dim) sy1 = k_gmpack_tile_dim - 1u;

    int   max_ix = (int)width_px - 1;
    int   max_iy = (int)height_px - 1;
    float inv_2hx = 1.0f / (2.0f * half_span_x);
    float inv_2hy = 1.0f / (2.0f * half_span_y);
    float w_clamp = fminf(fmaxf(blend_weight, 0.0f), 1.0f);
    uint32_t baked_cnt = 0u;

    for (uint32_t sy = sy0; sy <= sy1; ++sy) {
        float wy = node->world_min_y + ((float)sy / (float)(k_gmpack_tile_dim - 1u)) * span_y;
        float ov_v = (ov_max_y - wy) * inv_2hy;
        if (ov_v < 0.0f || ov_v > 1.0f) {
            continue;
        }
        float tc_y = ov_v * (float)max_iy;
        int   iy   = (int)floorf(tc_y);
        float fy   = tc_y - (float)iy;
        float wy_c[4];
        catmull_rom_w(fy, wy_c);
        float feather_y = fminf(fminf(ov_v, 1.0f - ov_v) / 0.015f, 1.0f);

        for (uint32_t sx = sx0; sx <= sx1; ++sx) {
            float wx = node->world_min_x + ((float)sx / (float)(k_gmpack_tile_dim - 1u)) * span_x;
            float ov_u = (wx - ov_min_x) * inv_2hx;
            if (ov_u < 0.0f || ov_u > 1.0f) {
                continue;
            }
            float tc_x = ov_u * (float)max_ix;
            int   ix   = (int)floorf(tc_x);
            float fx   = tc_x - (float)ix;
            float wx_c[4];
            catmull_rom_w(fx, wx_c);

            float sum_rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int dy = -1; dy <= 2; ++dy) {
                int cy = iy + dy;
                if (cy < 0) cy = 0;
                if (cy > max_iy) cy = max_iy;
                float row_rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const uint32_t *row_ptr = &rgba_pixels[(size_t)cy * width_px];
                for (int dx = -1; dx <= 2; ++dx) {
                    int cx = ix + dx;
                    if (cx < 0) cx = 0;
                    if (cx > max_ix) cx = max_ix;
                    uint32_t p = row_ptr[cx];
                    float wt = wx_c[dx + 1];
                    row_rgba[0] += (float)( p         & 0xFFu) * wt;
                    row_rgba[1] += (float)((p >>  8u) & 0xFFu) * wt;
                    row_rgba[2] += (float)((p >> 16u) & 0xFFu) * wt;
                    row_rgba[3] += (float)((p >> 24u) & 0xFFu) * wt;
                }
                float wyt = wy_c[dy + 1];
                sum_rgba[0] += row_rgba[0] * wyt;
                sum_rgba[1] += row_rgba[1] * wyt;
                sum_rgba[2] += row_rgba[2] * wyt;
                sum_rgba[3] += row_rgba[3] * wyt;
            }

            float feather_x = fminf(fminf(ov_u, 1.0f - ov_u) / 0.015f, 1.0f);
            float edge_f    = feather_x * feather_y;
            float edge_s    = edge_f * edge_f * (3.0f - 2.0f * edge_f);
            float alpha     = fminf(fmaxf(sum_rgba[3] / 255.0f, 0.0f), 1.0f) * w_clamp * edge_s;
            if (alpha <= 1.0e-3f) {
                continue;
            }

            uint32_t idx  = sy * k_gmpack_tile_dim + sx;
            uint32_t orig = tile->imagery_rgba8[idx];
            uint32_t ao_u8 = (orig >> 24u) & 0xFFu;
            float base_r = (float)( orig         & 0xFFu);
            float base_g = (float)((orig >>  8u) & 0xFFu);
            float base_b = (float)((orig >> 16u) & 0xFFu);

            float out_r = base_r + (fminf(fmaxf(sum_rgba[0], 0.0f), 255.0f) - base_r) * alpha;
            float out_g = base_g + (fminf(fmaxf(sum_rgba[1], 0.0f), 255.0f) - base_g) * alpha;
            float out_b = base_b + (fminf(fmaxf(sum_rgba[2], 0.0f), 255.0f) - base_b) * alpha;

            uint32_t R = (uint32_t)lrintf(fminf(fmaxf(out_r, 0.0f), 255.0f));
            uint32_t G = (uint32_t)lrintf(fminf(fmaxf(out_g, 0.0f), 255.0f));
            uint32_t B = (uint32_t)lrintf(fminf(fmaxf(out_b, 0.0f), 255.0f));
            tile->imagery_rgba8[idx] = R | (G << 8u) | (B << 16u) | (ao_u8 << 24u);
            baked_cnt++;
        }
    }
    return baked_cnt;
}

int backend_survey_add_photo(TerrainSurveyDataset       *ds,
                             const GpuMathPageDirectory *dir,
                             const TerrainSurveyPhoto   *photo_spec) {
    if (ds == nullptr || photo_spec == nullptr || photo_spec->rgba_pixels == nullptr ||
        photo_spec->intrinsics.width_px == 0u || photo_spec->intrinsics.height_px == 0u ||
        ds->photo_count >= k_survey_max_photos) {
        return -1;
    }

    TerrainSurveyPhoto p = *photo_spec;
    p.rgba_pixels = nullptr;
    if (!backend_survey_init_intrinsics(&p.intrinsics)) {
        return -1;
    }
    backend_survey_init_pose(dir, &p.pose);
    if (!isfinite(p.blend_weight) || p.blend_weight <= 0.0f) {
        p.blend_weight = 1.0f;
    } else if (p.blend_weight > 1.0f) {
        p.blend_weight = 1.0f;
    }

    size_t count = (size_t)p.intrinsics.width_px * (size_t)p.intrinsics.height_px;
    uint32_t *copy = malloc(count * sizeof(uint32_t));
    if (copy == nullptr) {
        return -1;
    }
    memcpy(copy, photo_spec->rgba_pixels, count * sizeof(uint32_t));
    p.rgba_pixels = copy;

    ds->photos[ds->photo_count++] = p;
    ds->last_stats.photos_loaded  = ds->photo_count;
    return (int)(ds->photo_count - 1u);
}

int backend_survey_add_point(TerrainSurveyDataset       *ds,
                             const GpuMathPageDirectory *dir,
                             const TerrainSurveyPoint   *pt) {
    if (ds == nullptr || pt == nullptr || !isfinite(pt->elevation_m)) {
        return -1;
    }
    if (ds->point_count >= ds->point_cap) {
        uint32_t next_cap = ds->point_cap == 0u ? 64u : ds->point_cap * 2u;
        TerrainSurveyPoint *grown = realloc(ds->points, (size_t)next_cap * sizeof(TerrainSurveyPoint));
        if (grown == nullptr) {
            return -1;
        }
        ds->points    = grown;
        ds->point_cap = next_cap;
    }

    TerrainSurveyPoint item = *pt;
    if (item.use_wgs84) {
        backend_survey_wgs84_to_world(dir, item.latitude_deg, item.longitude_deg,
                                      &item.world_x, &item.world_y);
    } else {
        backend_survey_world_to_wgs84(dir, item.world_x, item.world_y,
                                      &item.latitude_deg, &item.longitude_deg);
    }
    if (!isfinite(item.radius_m) || item.radius_m <= 0.5f) {
        item.radius_m = 60.0f;
    }
    if (!isfinite(item.weight) || item.weight <= 0.0f) {
        item.weight = 1.0f;
    } else if (item.weight > 1.0f) {
        item.weight = 1.0f;
    }

    ds->points[ds->point_count++]       = item;
    ds->last_stats.survey_points_loaded = ds->point_count;
    return (int)(ds->point_count - 1u);
}

int backend_survey_add_breakline(TerrainSurveyDataset         *ds,
                                 const TerrainSurveyBreakline *bl) {
    if (ds == nullptr || bl == nullptr || ds->breakline_count >= k_survey_max_breaklines ||
        !isfinite(bl->elev_a_m) || !isfinite(bl->elev_b_m)) {
        return -1;
    }
    TerrainSurveyBreakline item = *bl;
    if (!isfinite(item.radius_m) || item.radius_m <= 0.5f) {
        item.radius_m = 50.0f;
    }
    if (!isfinite(item.weight) || item.weight <= 0.0f) {
        item.weight = 0.9f;
    } else if (item.weight > 1.0f) {
        item.weight = 1.0f;
    }
    ds->breaklines[ds->breakline_count++] = item;
    ds->last_stats.breaklines_loaded      = ds->breakline_count;
    return (int)(ds->breakline_count - 1u);
}

int backend_survey_add_tiepoint(TerrainSurveyDataset        *ds,
                                const TerrainStereoTiePoint *tp) {
    if (ds == nullptr || tp == nullptr || tp->view_count < 2u ||
        tp->view_count > k_survey_max_views_per_tie ||
        ds->tiepoint_count >= k_survey_max_tiepoints) {
        return -1;
    }
    TerrainStereoTiePoint item = *tp;
    if (!isfinite(item.radius_m) || item.radius_m <= 0.5f) {
        item.radius_m = 80.0f;
    }
    if (!isfinite(item.weight) || item.weight <= 0.0f) {
        item.weight = 0.95f;
    } else if (item.weight > 1.0f) {
        item.weight = 1.0f;
    }
    item.valid = false;
    ds->tiepoints[ds->tiepoint_count++] = item;
    return (int)(ds->tiepoint_count - 1u);
}

static void sample_photo_bilinear_rgba(const TerrainSurveyPhoto *photo,
                                       float                     u_px,
                                       float                     v_px,
                                       float                     out_rgba[4]) {
    uint32_t w = photo->intrinsics.width_px;
    uint32_t h = photo->intrinsics.height_px;
    float cx = fminf(fmaxf(u_px, 0.0f), (float)(w - 1u));
    float cy = fminf(fmaxf(v_px, 0.0f), (float)(h - 1u));
    uint32_t x0 = (uint32_t)floorf(cx);
    uint32_t y0 = (uint32_t)floorf(cy);
    uint32_t x1 = (x0 + 1u < w) ? (x0 + 1u) : (w - 1u);
    uint32_t y1 = (y0 + 1u < h) ? (y0 + 1u) : (h - 1u);
    float fx = cx - (float)x0;
    float fy = cy - (float)y0;

    uint32_t p00 = photo->rgba_pixels[y0 * w + x0];
    uint32_t p10 = photo->rgba_pixels[y0 * w + x1];
    uint32_t p01 = photo->rgba_pixels[y1 * w + x0];
    uint32_t p11 = photo->rgba_pixels[y1 * w + x1];

    for (uint32_t c = 0u; c < 4u; ++c) {
        uint32_t shift = c * 8u;
        float v00 = (float)((p00 >> shift) & 0xFFu);
        float v10 = (float)((p10 >> shift) & 0xFFu);
        float v01 = (float)((p01 >> shift) & 0xFFu);
        float v11 = (float)((p11 >> shift) & 0xFFu);
        float vx0 = v00 + (v10 - v00) * fx;
        float vx1 = v01 + (v11 - v01) * fx;
        out_rgba[c] = vx0 + (vx1 - vx0) * fy;
    }
}

static float sample_photo_luma(const TerrainSurveyPhoto *photo, float u_px, float v_px) {
    float rgba[4];
    sample_photo_bilinear_rgba(photo, u_px, v_px, rgba);
    return 0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2];
}

int backend_survey_solve_tiepoints(TerrainSurveyDataset       *ds,
                                   const GpuMathPageDirectory *dir) {
    if (ds == nullptr) {
        return -1;
    }
    uint32_t solved = 0u;
    for (uint32_t t = 0u; t < ds->tiepoint_count; ++t) {
        TerrainStereoTiePoint *tp = &ds->tiepoints[t];
        if (tp->valid) {
            solved++;
            continue;
        }
        if (tp->view_count < 2u) {
            continue;
        }
        float origins[k_survey_max_views_per_tie][3];
        float dirs[k_survey_max_views_per_tie][3];
        uint32_t used_photos[k_survey_max_views_per_tie];
        float rgb_accum[3] = { 0.0f, 0.0f, 0.0f };
        uint32_t valid_rays = 0u;

        for (uint32_t v = 0u; v < tp->view_count && v < k_survey_max_views_per_tie; ++v) {
            uint32_t pidx = tp->views[v].photo_index;
            if (pidx >= ds->photo_count) {
                continue;
            }
            bool duplicate_photo = false;
            for (uint32_t prev = 0u; prev < valid_rays; ++prev) {
                if (used_photos[prev] == pidx) {
                    duplicate_photo = true;
                    break;
                }
            }
            if (duplicate_photo) {
                continue;
            }
            const TerrainSurveyPhoto *ph = &ds->photos[pidx];
            float u_px = tp->views[v].u_px;
            float v_px = tp->views[v].v_px;
            if (!isfinite(u_px) || !isfinite(v_px) ||
                u_px < 0.0f || u_px > (float)(ph->intrinsics.width_px - 1u) ||
                v_px < 0.0f || v_px > (float)(ph->intrinsics.height_px - 1u)) {
                continue;
            }
            if (backend_survey_pixel_to_world_ray(&ph->intrinsics, &ph->pose,
                                                  u_px, v_px,
                                                  origins[valid_rays], dirs[valid_rays])) {
                float rgba[4];
                sample_photo_bilinear_rgba(ph, u_px, v_px, rgba);
                rgb_accum[0] += rgba[0];
                rgb_accum[1] += rgba[1];
                rgb_accum[2] += rgba[2];
                used_photos[valid_rays] = pidx;
                valid_rays++;
            }
        }
        if (valid_rays < 2u) {
            continue;
        }

        float pt3d[3]      = { 0.0f, 0.0f, 0.0f };
        float rmse_m       = 0.0f;
        float parallax_deg = 0.0f;
        if (!backend_survey_triangulate_rays(valid_rays, origins, dirs,
                                             pt3d, &rmse_m, &parallax_deg) ||
            rmse_m > 25.0f) {
            continue;
        }

        tp->triangulated_x      = pt3d[0];
        tp->triangulated_y      = pt3d[1];
        tp->triangulated_elev_m = pt3d[2];
        tp->reproj_rmse_m       = rmse_m;
        tp->valid               = true;
        solved++;

        TerrainSurveyPoint spt = {
            .world_x     = pt3d[0],
            .world_y     = pt3d[1],
            .elevation_m = pt3d[2],
            .radius_m    = tp->radius_m,
            .weight      = tp->weight,
            .r           = (uint8_t)lrintf(fminf(fmaxf(rgb_accum[0] / (float)valid_rays, 0.0f), 255.0f)),
            .g           = (uint8_t)lrintf(fminf(fmaxf(rgb_accum[1] / (float)valid_rays, 0.0f), 255.0f)),
            .b           = (uint8_t)lrintf(fminf(fmaxf(rgb_accum[2] / (float)valid_rays, 0.0f), 255.0f)),
            .has_color   = true,
            .use_wgs84   = false,
        };
        (void)backend_survey_add_point(ds, dir, &spt);
    }
    ds->last_stats.tiepoints_triangulated = solved;
    return (int)solved;
}

// Wendland C^2 compactly supported radial basis function: (1 - q)^4 * (4q + 1) for q in [0, 1]
static float wendland_c2(float q) {
    if (q >= 1.0f || q < 0.0f) {
        return 0.0f;
    }
    float omq  = 1.0f - q;
    float omq2 = omq * omq;
    float omq4 = omq2 * omq2;
    return omq4 * (4.0f * q + 1.0f);
}

static float smoothstep01(float edge0, float edge1, float x) {
    float t = fminf(fmaxf((x - edge0) / fmaxf(edge1 - edge0, 1.0e-5f), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float sample_elev_grid_bilinear(const float *elev_grid, float u, float v) {
    float tx = fminf(fmaxf(u, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u);
    float ty = fminf(fmaxf(v, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u);
    uint32_t x0 = (uint32_t)floorf(tx);
    uint32_t y0 = (uint32_t)floorf(ty);
    uint32_t x1 = (x0 + 1u < k_gmpack_tile_dim) ? (x0 + 1u) : (k_gmpack_tile_dim - 1u);
    uint32_t y1 = (y0 + 1u < k_gmpack_tile_dim) ? (y0 + 1u) : (k_gmpack_tile_dim - 1u);
    float fx = tx - (float)x0;
    float fy = ty - (float)y0;

    float h00 = elev_grid[y0 * k_gmpack_tile_dim + x0];
    float h10 = elev_grid[y0 * k_gmpack_tile_dim + x1];
    float h01 = elev_grid[y1 * k_gmpack_tile_dim + x0];
    float h11 = elev_grid[y1 * k_gmpack_tile_dim + x1];
    float hx0 = h00 + (h10 - h00) * fx;
    float hx1 = h01 + (h11 - h01) * fx;
    return hx0 + (hx1 - hx0) * fy;
}

typedef struct MvsStereoPair {
    const TerrainSurveyPhoto *p0;
    const TerrainSurveyPhoto *p1;
    float                     patch_step_m;
    float                     weight;
} MvsStereoPair;

// Evaluates 3x3 tangent-plane world-patch Zero-Mean Normalized Cross-Correlation (ZNCC) between two views at elevation h
static float eval_mvs_pair_zncc(const TerrainSurveyPhoto *p0,
                                const TerrainSurveyPhoto *p1,
                                float                     wx,
                                float                     wy,
                                float                     elev_m,
                                float                     dzdx,
                                float                     dzdy,
                                float                     patch_step_m) {
    float l0[9];
    float l1[9];
    float sum0 = 0.0f;
    float sum1 = 0.0f;
    uint32_t k = 0u;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float ox = (float)dx * patch_step_m;
            float oy = (float)dy * patch_step_m;
            float px = wx + ox;
            float py = wy + oy;
            float pz = elev_m + ox * dzdx + oy * dzdy;
            float u0 = 0.0f, v0 = 0.0f, d0 = 0.0f;
            float u1 = 0.0f, v1 = 0.0f, d1 = 0.0f;
            if (!backend_survey_project_world_to_pixel(&p0->intrinsics, &p0->pose, px, py, pz, &u0, &v0, &d0) ||
                !backend_survey_project_world_to_pixel(&p1->intrinsics, &p1->pose, px, py, pz, &u1, &v1, &d1)) {
                return -1.0f;
            }
            float val0 = sample_photo_luma(p0, u0, v0);
            float val1 = sample_photo_luma(p1, u1, v1);
            l0[k] = val0;
            l1[k] = val1;
            sum0 += val0;
            sum1 += val1;
            k++;
        }
    }

    float mean0 = sum0 * (1.0f / 9.0f);
    float mean1 = sum1 * (1.0f / 9.0f);
    float num   = 0.0f;
    float var0  = 0.0f;
    float var1  = 0.0f;
    for (uint32_t i = 0u; i < 9u; ++i) {
        float a = l0[i] - mean0;
        float b = l1[i] - mean1;
        num  += a * b;
        var0 += a * a;
        var1 += b * b;
    }
    // Reject homogeneous/untextured patches to avoid noisy depth estimates
    if (var0 < 4.0f || var1 < 4.0f) {
        return -1.0f;
    }
    return num / sqrtf(var0 * var1 + 1.0e-5f);
}

static float eval_mvs_multiview_zncc(const MvsStereoPair *pairs,
                                     uint32_t             pair_count,
                                     float                wx,
                                     float                wy,
                                     float                elev_m,
                                     float                dzdx,
                                     float                dzdy) {
    float ncc_sum = 0.0f;
    float w_sum   = 0.0f;
    for (uint32_t i = 0u; i < pair_count; ++i) {
        float ncc = eval_mvs_pair_zncc(pairs[i].p0, pairs[i].p1,
                                       wx, wy, elev_m, dzdx, dzdy,
                                       pairs[i].patch_step_m);
        if (ncc > -0.95f) {
            ncc_sum += ncc * pairs[i].weight;
            w_sum   += pairs[i].weight;
        }
    }
    return (w_sum > 1.0e-5f) ? (ncc_sum / w_sum) : -1.0f;
}

static bool photo_overlaps_tile(const TerrainSurveyPhoto *ph,
                                const GpuMathQuadNode    *node,
                                float                     min_elev,
                                float                     max_elev) {
    if (ph->rgba_pixels == nullptr) {
        return false;
    }
    // Check if camera itself lies above/inside the tile XY bounds
    if (ph->pose.world_x >= node->world_min_x - 200.0f &&
        ph->pose.world_x <= node->world_max_x + 200.0f &&
        ph->pose.world_y >= node->world_min_y - 200.0f &&
        ph->pose.world_y <= node->world_max_y + 200.0f) {
        return true;
    }
    // Project a 3x3x2 lattice of points across the tile's 3D bounding box
    float min_u =  1.0e9f, min_v =  1.0e9f;
    float max_u = -1.0e9f, max_v = -1.0e9f;
    uint32_t in_front = 0u;
    for (uint32_t iz = 0u; iz < 2u; ++iz) {
        float ez = (iz == 0u) ? min_elev : max_elev;
        for (uint32_t iy = 0u; iy <= 2u; ++iy) {
            float wy = node->world_min_y + 0.5f * (float)iy * (node->world_max_y - node->world_min_y);
            for (uint32_t ix = 0u; ix <= 2u; ++ix) {
                float wx = node->world_min_x + 0.5f * (float)ix * (node->world_max_x - node->world_min_x);
                float u = 0.0f, v = 0.0f, d = 0.0f;
                (void)backend_survey_project_world_to_pixel(&ph->intrinsics, &ph->pose, wx, wy, ez, &u, &v, &d);
                if (d > 0.2f && isfinite(u) && isfinite(v)) {
                    in_front++;
                    if (u < min_u) min_u = u;
                    if (v < min_v) min_v = v;
                    if (u > max_u) max_u = u;
                    if (v > max_v) max_v = v;
                }
            }
        }
    }
    if (in_front == 0u) {
        return false;
    }
    return !(max_u < -64.0f || min_u > (float)ph->intrinsics.width_px + 64.0f ||
             max_v < -64.0f || min_v > (float)ph->intrinsics.height_px + 64.0f);
}

bool backend_survey_tile_may_overlap(const TerrainSurveyDataset *ds,
                                     const GpuMathQuadNode      *node) {
    if (ds == nullptr || node == nullptr) {
        return false;
    }
    if (ds->photo_count == 0u && ds->ortho_overlay_count == 0u &&
        ds->point_count == 0u && ds->breakline_count == 0u) {
        return false;
    }

    for (uint32_t i = 0u; i < ds->ortho_overlay_count && i < k_survey_max_ortho_overlays; ++i) {
        const TerrainOrthoOverlay *ov = &ds->ortho_overlays[i];
        if (ov->rgba_pixels == nullptr || ov->half_span_x <= 0.0f || ov->half_span_y <= 0.0f ||
            ov->blend_weight <= 0.0f) {
            continue;
        }
        if (ov->center_x + ov->half_span_x >= node->world_min_x &&
            ov->center_x - ov->half_span_x <= node->world_max_x &&
            ov->center_y + ov->half_span_y >= node->world_min_y &&
            ov->center_y - ov->half_span_y <= node->world_max_y) {
            return true;
        }
    }

    float span_x = fmaxf(node->world_max_x - node->world_min_x, 1.0f);
    float step_m = span_x / (float)(k_gmpack_tile_dim - 1u);
    float min_kernel_r = step_m * 1.15f;

    for (uint32_t i = 0u; i < ds->point_count; ++i) {
        const TerrainSurveyPoint *pt = &ds->points[i];
        float r = fmaxf(pt->radius_m, min_kernel_r);
        if (pt->world_x + r >= node->world_min_x && pt->world_x - r <= node->world_max_x &&
            pt->world_y + r >= node->world_min_y && pt->world_y - r <= node->world_max_y) {
            return true;
        }
    }

    for (uint32_t i = 0u; i < ds->breakline_count; ++i) {
        const TerrainSurveyBreakline *bl = &ds->breaklines[i];
        float r     = fmaxf(bl->radius_m, min_kernel_r);
        float min_x = fminf(bl->world_ax, bl->world_bx) - r;
        float max_x = fmaxf(bl->world_ax, bl->world_bx) + r;
        float min_y = fminf(bl->world_ay, bl->world_by) - r;
        float max_y = fmaxf(bl->world_ay, bl->world_by) + r;
        if (max_x >= node->world_min_x && min_x <= node->world_max_x &&
            max_y >= node->world_min_y && min_y <= node->world_max_y) {
            return true;
        }
    }

    if (ds->photo_count > 0u) {
        uint32_t blk_idx = 0u;
        uint16_t morton  = 0u;
        uint8_t  lod     = 0u;
        uint8_t  flags   = 0u;
        float    node_min_e = 0.0f;
        float    node_max_e = 0.0f;
        gpumath_tile_header_unpack(node->header, &blk_idx, &morton, &lod, &flags, &node_min_e, &node_max_e);
        if (node_max_e <= node_min_e) {
            node_min_e = k_besparmak_elev_min;
            node_max_e = k_besparmak_elev_max;
        }
        float min_e = node_min_e - 60.0f;
        float max_e = node_max_e + 60.0f;
        for (uint32_t i = 0u; i < ds->photo_count && i < k_survey_max_photos; ++i) {
            const TerrainSurveyPhoto *ph = &ds->photos[i];
            if ((ph->pose.use_agl || ph->pose.altitude_msl_m <= 0.0f) && ph->pose.height_agl_m > 0.0f) {
                if (ph->pose.world_x >= node->world_min_x - 200.0f &&
                    ph->pose.world_x <= node->world_max_x + 200.0f &&
                    ph->pose.world_y >= node->world_min_y - 200.0f &&
                    ph->pose.world_y <= node->world_max_y + 200.0f) {
                    return true;
                }
            }
            if (photo_overlaps_tile(ph, node, min_e, max_e)) {
                return true;
            }
        }
    }

    return false;
}

bool backend_survey_fuse_tile(TerrainSurveyDataset *ds,
                              GpuMathArchiveHeader *hdr,
                              uint32_t              node_index,
                              GpuMathTileBlock     *tile,
                              TerrainSurveyStats   *io_stats) {
    if (ds == nullptr || hdr == nullptr || tile == nullptr || node_index >= k_gmpack_node_count) {
        return false;
    }
    if (ds->photo_count == 0u && ds->ortho_overlay_count == 0u &&
        ds->point_count == 0u && ds->breakline_count == 0u) {
        return false;
    }

    GpuMathQuadNode *node = &hdr->nodes[node_index];
    uint32_t ortho_baked_texels = 0u;
    for (uint32_t i = 0u; i < ds->ortho_overlay_count && i < k_survey_max_ortho_overlays; ++i) {
        const TerrainOrthoOverlay *ov = &ds->ortho_overlays[i];
        ortho_baked_texels += backend_survey_bake_ortho_tile(node, tile,
                                                             ov->rgba_pixels, ov->width_px, ov->height_px,
                                                             ov->center_x, ov->center_y,
                                                             ov->half_span_x, ov->half_span_y,
                                                             ov->blend_weight);
    }
    if (ds->photo_count == 0u && ds->point_count == 0u && ds->breakline_count == 0u) {
        if (ortho_baked_texels > 0u && io_stats != nullptr) {
            io_stats->tiles_updated         += 1u;
            io_stats->texels_orthorectified += ortho_baked_texels;
        }
        return ortho_baked_texels > 0u;
    }
    uint32_t blk_idx = 0u;
    uint16_t morton  = 0u;
    uint8_t  lod     = 0u;
    uint8_t  flags   = 0u;
    float    node_min_e = 0.0f;
    float    node_max_e = 0.0f;
    gpumath_tile_header_unpack(node->header, &blk_idx, &morton, &lod, &flags, &node_min_e, &node_max_e);
    if (node_max_e <= node_min_e) {
        node_min_e = k_besparmak_elev_min;
        node_max_e = k_besparmak_elev_max;
    }

    float span_x = fmaxf(node->world_max_x - node->world_min_x, 1.0f);
    float span_y = fmaxf(node->world_max_y - node->world_min_y, 1.0f);
    float step_m = span_x / (float)(k_gmpack_tile_dim - 1u);
    float min_kernel_r = step_m * 1.15f;

    // 1. Filter active geodetic points, breaklines, and photos overlapping this tile's AABB
    //    and compute the tight 2D region-of-interest (ROI) [roi_sx0..roi_sx1] x [roi_sy0..roi_sy1]
    float roi_min_x =  1.0e9f;
    float roi_max_x = -1.0e9f;
    float roi_min_y =  1.0e9f;
    float roi_max_y = -1.0e9f;

    enum : uint32_t { k_local_pt_cap = 512u, k_local_bl_cap = 128u };
    uint32_t active_pts[k_local_pt_cap];
    uint32_t active_pt_cnt = 0u;
    for (uint32_t i = 0u; i < ds->point_count && active_pt_cnt < k_local_pt_cap; ++i) {
        const TerrainSurveyPoint *pt = &ds->points[i];
        float r = fmaxf(pt->radius_m, min_kernel_r);
        if (pt->world_x + r >= node->world_min_x && pt->world_x - r <= node->world_max_x &&
            pt->world_y + r >= node->world_min_y && pt->world_y - r <= node->world_max_y) {
            active_pts[active_pt_cnt++] = i;
            if (pt->world_x - r < roi_min_x) roi_min_x = pt->world_x - r;
            if (pt->world_x + r > roi_max_x) roi_max_x = pt->world_x + r;
            if (pt->world_y - r < roi_min_y) roi_min_y = pt->world_y - r;
            if (pt->world_y + r > roi_max_y) roi_max_y = pt->world_y + r;
        }
    }

    uint32_t active_bls[k_local_bl_cap];
    uint32_t active_bl_cnt = 0u;
    for (uint32_t i = 0u; i < ds->breakline_count && active_bl_cnt < k_local_bl_cap; ++i) {
        const TerrainSurveyBreakline *bl = &ds->breaklines[i];
        float r     = fmaxf(bl->radius_m, min_kernel_r);
        float min_x = fminf(bl->world_ax, bl->world_bx) - r;
        float max_x = fmaxf(bl->world_ax, bl->world_bx) + r;
        float min_y = fminf(bl->world_ay, bl->world_by) - r;
        float max_y = fmaxf(bl->world_ay, bl->world_by) + r;
        if (max_x >= node->world_min_x && min_x <= node->world_max_x &&
            max_y >= node->world_min_y && min_y <= node->world_max_y) {
            active_bls[active_bl_cnt++] = i;
            if (min_x < roi_min_x) roi_min_x = min_x;
            if (max_x > roi_max_x) roi_max_x = max_x;
            if (min_y < roi_min_y) roi_min_y = min_y;
            if (max_y > roi_max_y) roi_max_y = max_y;
        }
    }

    uint32_t active_photos[k_survey_max_photos];
    uint32_t active_photo_cnt = 0u;
    uint32_t mvs_photo_cnt    = 0u;
    for (uint32_t i = 0u; i < ds->photo_count && i < k_survey_max_photos; ++i) {
        if ((ds->photos[i].pose.use_agl || ds->photos[i].pose.altitude_msl_m <= 0.0f) &&
            ds->photos[i].pose.height_agl_m > 0.0f) {
            if (ds->photos[i].pose.world_x >= node->world_min_x &&
                ds->photos[i].pose.world_x <= node->world_max_x &&
                ds->photos[i].pose.world_y >= node->world_min_y &&
                ds->photos[i].pose.world_y <= node->world_max_y) {
                float u = (ds->photos[i].pose.world_x - node->world_min_x) / span_x;
                float v = (ds->photos[i].pose.world_y - node->world_min_y) / span_y;
                uint32_t sx = (uint32_t)lrintf(fminf(fmaxf(u, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u));
                uint32_t sy = (uint32_t)lrintf(fminf(fmaxf(v, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u));
                float base_ground = (float)tile->height_u16[sy * k_gmpack_tile_dim + sx] * 0.0625f;
                ds->photos[i].pose.altitude_msl_m = base_ground + ds->photos[i].pose.height_agl_m;
                ds->photos[i].pose.use_agl = true;
            } else if (ds->photos[i].pose.altitude_msl_m <= 0.0f) {
                float ground_elev = backend_eval_besparmak_elevation_m(
                    ds->photos[i].pose.latitude_deg,
                    ds->photos[i].pose.longitude_deg);
                ds->photos[i].pose.altitude_msl_m = ground_elev + ds->photos[i].pose.height_agl_m;
                ds->photos[i].pose.use_agl = true;
            }
        }
        if (photo_overlaps_tile(&ds->photos[i], node, node_min_e - 50.0f, node_max_e + 50.0f)) {
            active_photos[active_photo_cnt++] = i;
            if (ds->photos[i].enable_mvs) {
                mvs_photo_cnt++;
            }
            // Bound the photo's ground footprint on this tile by intersecting a 3x3 grid of image rays
            // with [node_min_e - 60, node_max_e + 60], or expanding to the full tile for grazing views
            const TerrainSurveyPhoto *ph = &ds->photos[i];
            if (ph->pose.pitch_deg > -28.0f) {
                roi_min_x = node->world_min_x;
                roi_max_x = node->world_max_x;
                roi_min_y = node->world_min_y;
                roi_max_y = node->world_max_y;
            } else {
                float pad_m = fmaxf(step_m * 2.5f, 80.0f);
                for (uint32_t iy = 0u; iy <= 2u; ++iy) {
                    float pv = 0.5f * (float)iy * (float)(ph->intrinsics.height_px - 1u);
                    for (uint32_t ix = 0u; ix <= 2u; ++ix) {
                        float pu = 0.5f * (float)ix * (float)(ph->intrinsics.width_px - 1u);
                        float orig[3] = {0}, rdir[3] = {0};
                        if (backend_survey_pixel_to_world_ray(&ph->intrinsics, &ph->pose, pu, pv, orig, rdir) &&
                            rdir[2] < -0.05f) {
                            for (uint32_t iz = 0u; iz < 2u; ++iz) {
                                float ez = (iz == 0u) ? (node_min_e - 50.0f) : (node_max_e + 50.0f);
                                float t  = (ez - orig[2]) / rdir[2];
                                if (t > 0.0f && t < 4500.0f) {
                                    float hx = orig[0] + rdir[0] * t;
                                    float hy = orig[1] + rdir[1] * t;
                                    if (hx - pad_m < roi_min_x) roi_min_x = hx - pad_m;
                                    if (hx + pad_m > roi_max_x) roi_max_x = hx + pad_m;
                                    if (hy - pad_m < roi_min_y) roi_min_y = hy - pad_m;
                                    if (hy + pad_m > roi_max_y) roi_max_y = hy + pad_m;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (active_pt_cnt == 0u && active_bl_cnt == 0u && active_photo_cnt == 0u) {
        if (ortho_baked_texels > 0u && io_stats != nullptr) {
            io_stats->tiles_updated         += 1u;
            io_stats->texels_orthorectified += ortho_baked_texels;
        }
        return ortho_baked_texels > 0u;
    }
    if (roi_max_x < node->world_min_x || roi_min_x > node->world_max_x ||
        roi_max_y < node->world_min_y || roi_min_y > node->world_max_y) {
        if (ortho_baked_texels > 0u && io_stats != nullptr) {
            io_stats->tiles_updated         += 1u;
            io_stats->texels_orthorectified += ortho_baked_texels;
        }
        return ortho_baked_texels > 0u;
    }

    float u0_roi = fminf(fmaxf((roi_min_x - node->world_min_x) / span_x, 0.0f), 1.0f);
    float u1_roi = fminf(fmaxf((roi_max_x - node->world_min_x) / span_x, 0.0f), 1.0f);
    float v0_roi = fminf(fmaxf((roi_min_y - node->world_min_y) / span_y, 0.0f), 1.0f);
    float v1_roi = fminf(fmaxf((roi_max_y - node->world_min_y) / span_y, 0.0f), 1.0f);
    uint32_t roi_sx0 = (uint32_t)floorf(u0_roi * (float)(k_gmpack_tile_dim - 1u));
    uint32_t roi_sx1 = (uint32_t)ceilf(u1_roi * (float)(k_gmpack_tile_dim - 1u));
    uint32_t roi_sy0 = (uint32_t)floorf(v0_roi * (float)(k_gmpack_tile_dim - 1u));
    uint32_t roi_sy1 = (uint32_t)ceilf(v1_roi * (float)(k_gmpack_tile_dim - 1u));
    if (roi_sx1 >= k_gmpack_tile_dim) roi_sx1 = k_gmpack_tile_dim - 1u;
    if (roi_sy1 >= k_gmpack_tile_dim) roi_sy1 = k_gmpack_tile_dim - 1u;

    float *elev_m = malloc(k_gmpack_tile_samples * sizeof(float));
    if (elev_m == nullptr) {
        return false;
    }
    float new_min_e =  1.0e9f;
    float new_max_e = -1.0e9f;
    float sum_e     =  0.0f;
    for (uint32_t i = 0u; i < k_gmpack_tile_samples; ++i) {
        float h = (float)tile->height_u16[i] * 0.0625f;
        elev_m[i] = h;
        if (h < new_min_e) new_min_e = h;
        if (h > new_max_e) new_max_e = h;
        sum_e += h;
    }

    uint32_t dem_posts_changed = 0u;
    uint32_t mvs_posts_changed = 0u;
    float    max_elev_delta    = 0.0f;
    double   mvs_ncc_sum       = 0.0;

    // 2. Geodetic Survey Control Points, Breaklines & Multi-View Stereo (MVS) DEM Refinement (clipped to ROI)
    if (active_pt_cnt > 0u || active_bl_cnt > 0u || mvs_photo_cnt >= 2u) {
        float search_range = fmaxf(ds->mvs_search_range_m, 2.0f);
        float sweep_step   = fmaxf(ds->mvs_step_m, 0.20f);
        float min_ncc      = fminf(fmaxf(ds->mvs_min_ncc, 0.25f), 0.98f);
        float max_patch_step_m = fmaxf(step_m * 0.65f, 0.5f);

        for (uint32_t sy = roi_sy0; sy <= roi_sy1; ++sy) {
            float ty = (float)sy / (float)(k_gmpack_tile_dim - 1u);
            float wy = node->world_min_y + ty * span_y;
            uint32_t sy_m = (sy > 0u) ? (sy - 1u) : 0u;
            uint32_t sy_p = (sy + 1u < k_gmpack_tile_dim) ? (sy + 1u) : (k_gmpack_tile_dim - 1u);

            for (uint32_t sx = roi_sx0; sx <= roi_sx1; ++sx) {
                float tx = (float)sx / (float)(k_gmpack_tile_dim - 1u);
                float wx = node->world_min_x + tx * span_x;
                uint32_t idx = sy * k_gmpack_tile_dim + sx;
                float h_orig = elev_m[idx];
                float h_cur  = h_orig;

                // 2a. Geodetic Survey Points & Breaklines (Wendland C^2 RBF + IDW interpolation)
                if (active_pt_cnt > 0u || active_bl_cnt > 0u) {
                    double idw_num   = 0.0;
                    double idw_den   = 0.0;
                    float  max_alpha = 0.0f;

                    for (uint32_t pi = 0u; pi < active_pt_cnt; ++pi) {
                        const TerrainSurveyPoint *pt = &ds->points[active_pts[pi]];
                        float eff_r = fmaxf(pt->radius_m, min_kernel_r);
                        float dx = wx - pt->world_x;
                        float dy = wy - pt->world_y;
                        float d2 = dx * dx + dy * dy;
                        float r2 = eff_r * eff_r;
                        if (d2 >= r2) {
                            continue;
                        }
                        float q     = sqrtf(d2) / eff_r;
                        float alpha = pt->weight * wendland_c2(q);
                        if (alpha > max_alpha) {
                            max_alpha = alpha;
                        }
                        double pk = (double)alpha / ((double)(q * q) + 1.0e-4);
                        idw_num += pk * (double)pt->elevation_m;
                        idw_den += pk;
                    }

                    for (uint32_t bi = 0u; bi < active_bl_cnt; ++bi) {
                        const TerrainSurveyBreakline *bl = &ds->breaklines[active_bls[bi]];
                        float eff_r = fmaxf(bl->radius_m, min_kernel_r);
                        float abx = bl->world_bx - bl->world_ax;
                        float aby = bl->world_by - bl->world_ay;
                        float ab2 = abx * abx + aby * aby;
                        float t_seg = (ab2 > 1.0e-4f)
                            ? fminf(fmaxf(((wx - bl->world_ax) * abx + (wy - bl->world_ay) * aby) / ab2, 0.0f), 1.0f)
                            : 0.0f;
                        float cx = bl->world_ax + t_seg * abx;
                        float cy = bl->world_ay + t_seg * aby;
                        float dx = wx - cx;
                        float dy = wy - cy;
                        float d2 = dx * dx + dy * dy;
                        float r2 = eff_r * eff_r;
                        if (d2 >= r2) {
                            continue;
                        }
                        float q      = sqrtf(d2) / eff_r;
                        float alpha  = bl->weight * wendland_c2(q);
                        if (alpha > max_alpha) {
                            max_alpha = alpha;
                        }
                        float bl_elev = bl->elev_a_m + t_seg * (bl->elev_b_m - bl->elev_a_m);
                        double pk     = (double)alpha / ((double)(q * q) + 1.0e-4);
                        idw_num += pk * (double)bl_elev;
                        idw_den += pk;
                    }

                    if (idw_den > 1.0e-7 && max_alpha > 1.0e-4f) {
                        float h_target = (float)(idw_num / idw_den);
                        float beta     = fminf(max_alpha, 1.0f);
                        h_cur = (1.0f - beta) * h_cur + beta * h_target;
                    }
                }

                // 2b. Dense Multi-View Stereo (MVS) Patch-ZNCC Elevation Sweep across all valid stereo pairs
                if (mvs_photo_cnt >= 2u) {
                    uint32_t cand_views[8];
                    float    cand_depths[8];
                    uint32_t cand_cnt = 0u;
                    for (uint32_t ai = 0u; ai < active_photo_cnt && cand_cnt < 8u; ++ai) {
                        const TerrainSurveyPhoto *ph = &ds->photos[active_photos[ai]];
                        if (!ph->enable_mvs) {
                            continue;
                        }
                        float u = 0.0f, v = 0.0f, d = 0.0f;
                        if (backend_survey_project_world_to_pixel(&ph->intrinsics, &ph->pose,
                                                                  wx, wy, h_cur, &u, &v, &d) &&
                            u >= 3.0f && u <= (float)(ph->intrinsics.width_px - 4u) &&
                            v >= 3.0f && v <= (float)(ph->intrinsics.height_px - 4u)) {
                            cand_views[cand_cnt]  = active_photos[ai];
                            cand_depths[cand_cnt] = d;
                            cand_cnt++;
                        }
                    }

                    if (cand_cnt >= 2u) {
                        MvsStereoPair pairs[28];
                        uint32_t pair_cnt    = 0u;
                        float    max_pair_w  = 0.0f;

                        for (uint32_t va = 0u; va < cand_cnt; ++va) {
                            const TerrainSurveyPhoto *p0 = &ds->photos[cand_views[va]];
                            float v0x = wx - p0->pose.world_x;
                            float v0y = wy - p0->pose.world_y;
                            float v0z = h_cur - p0->pose.altitude_msl_m;
                            float l0  = sqrtf(v0x * v0x + v0y * v0y + v0z * v0z);
                            if (l0 <= 0.1f) {
                                continue;
                            }
                            float gsd0 = cand_depths[va] / fmaxf(p0->intrinsics.fx_px, 1.0f);

                            for (uint32_t vb = va + 1u; vb < cand_cnt; ++vb) {
                                const TerrainSurveyPhoto *p1 = &ds->photos[cand_views[vb]];
                                float v1x = wx - p1->pose.world_x;
                                float v1y = wy - p1->pose.world_y;
                                float v1z = h_cur - p1->pose.altitude_msl_m;
                                float l1  = sqrtf(v1x * v1x + v1y * v1y + v1z * v1z);
                                if (l1 <= 0.1f) {
                                    continue;
                                }
                                float cos_par = (v0x * v1x + v0y * v1y + v0z * v1z) / (l0 * l1);
                                cos_par = fminf(fmaxf(cos_par, -1.0f), 1.0f);
                                float parallax_deg = acosf(cos_par) * k_rad_to_deg;

                                if (parallax_deg >= 2.0f && parallax_deg <= 140.0f && pair_cnt < 28u) {
                                    float gsd1 = cand_depths[vb] / fmaxf(p1->intrinsics.fx_px, 1.0f);
                                    float pair_gsd = fmaxf(gsd0, gsd1);
                                    float p_step = fminf(fmaxf(pair_gsd * 1.5f, 0.25f), max_patch_step_m);
                                    float pw = fminf(p0->blend_weight, p1->blend_weight);
                                    pairs[pair_cnt++] = (MvsStereoPair){
                                        .p0           = p0,
                                        .p1           = p1,
                                        .patch_step_m = p_step,
                                        .weight       = pw,
                                    };
                                    if (pw > max_pair_w) {
                                        max_pair_w = pw;
                                    }
                                }
                            }
                        }

                        if (pair_cnt > 0u) {
                            uint32_t sx_m = (sx > 0u) ? (sx - 1u) : 0u;
                            uint32_t sx_p = (sx + 1u < k_gmpack_tile_dim) ? (sx + 1u) : (k_gmpack_tile_dim - 1u);
                            float hl0 = (float)tile->height_u16[sy * k_gmpack_tile_dim + sx_m] * 0.0625f;
                            float hr0 = (float)tile->height_u16[sy * k_gmpack_tile_dim + sx_p] * 0.0625f;
                            float hd0 = (float)tile->height_u16[sy_m * k_gmpack_tile_dim + sx] * 0.0625f;
                            float hu0 = (float)tile->height_u16[sy_p * k_gmpack_tile_dim + sx] * 0.0625f;
                            float dzdx0 = (hr0 - hl0) / (2.0f * step_m);
                            float dzdy0 = (hu0 - hd0) / (2.0f * step_m);

                            int   steps    = (int)ceilf(search_range / sweep_step);
                            if (steps > 48) steps = 48;
                            int   c_stride = (steps >= 9) ? 3 : 1;
                            float best_ncc = -2.0f;
                            int   best_s   = 0;

                            // Stage 1: Coarse elevation sweep
                            for (int s = -steps; s <= steps; s += c_stride) {
                                float h_test = h_cur + (float)s * sweep_step;
                                float ncc = eval_mvs_multiview_zncc(pairs, pair_cnt, wx, wy, h_test, dzdx0, dzdy0);
                                if (ncc > best_ncc) {
                                    best_ncc = ncc;
                                    best_s   = s;
                                }
                            }
                            // Stage 2: Fine local refinement around coarse peak
                            if (c_stride > 1 && best_ncc > -0.9f) {
                                int f_lo = (best_s - 2 > -steps) ? (best_s - 2) : -steps;
                                int f_hi = (best_s + 2 <  steps) ? (best_s + 2) :  steps;
                                for (int s = f_lo; s <= f_hi; ++s) {
                                    if (s == best_s) continue;
                                    float h_test = h_cur + (float)s * sweep_step;
                                    float ncc = eval_mvs_multiview_zncc(pairs, pair_cnt, wx, wy, h_test, dzdx0, dzdy0);
                                    if (ncc > best_ncc) {
                                        best_ncc = ncc;
                                        best_s   = s;
                                    }
                                }
                            }

                            if (best_ncc >= min_ncc) {
                                float h_best = h_cur + (float)best_s * sweep_step;
                                // 3-point parabolic sub-step refinement
                                if (best_s > -steps && best_s < steps) {
                                    float ncc_m = eval_mvs_multiview_zncc(pairs, pair_cnt, wx, wy, h_best - sweep_step, dzdx0, dzdy0);
                                    float ncc_p = eval_mvs_multiview_zncc(pairs, pair_cnt, wx, wy, h_best + sweep_step, dzdx0, dzdy0);
                                    float denom = ncc_m - 2.0f * best_ncc + ncc_p;
                                    if (fabsf(denom) > 1.0e-4f && ncc_m > -0.5f && ncc_p > -0.5f) {
                                        float sub = 0.5f * (ncc_m - ncc_p) / denom;
                                        sub = fminf(fmaxf(sub, -0.5f), 0.5f);
                                        h_best += sub * sweep_step;
                                    }
                                }
                                float conf = fminf(fmaxf((best_ncc - min_ncc) / (1.0f - min_ncc), 0.0f), 1.0f);
                                float mvs_w = conf * 0.85f * max_pair_w;
                                h_cur = (1.0f - mvs_w) * h_cur + mvs_w * h_best;
                                mvs_posts_changed++;
                                mvs_ncc_sum += (double)best_ncc;
                            }
                        }
                    }
                }

                float delta = fabsf(h_cur - h_orig);
                if (delta > 0.03f) {
                    dem_posts_changed++;
                    if (delta > max_elev_delta) {
                        max_elev_delta = delta;
                    }
                }
                elev_m[idx] = fminf(fmaxf(h_cur, 0.0f), 4095.0f);
            }
        }
    }

    // 3. Recompute height_u16, analytical surface normals, and projective ortho-image fusion within ROI
    uint32_t texels_fused = 0u;
    float tile_ceil_z = node_max_e + max_elev_delta + 5.0f;

    for (uint32_t sy = roi_sy0; sy <= roi_sy1; ++sy) {
        float ty = (float)sy / (float)(k_gmpack_tile_dim - 1u);
        float wy = node->world_min_y + ty * span_y;
        uint32_t sy_m = (sy > 0u) ? (sy - 1u) : 0u;
        uint32_t sy_p = (sy + 1u < k_gmpack_tile_dim) ? (sy + 1u) : (k_gmpack_tile_dim - 1u);

        for (uint32_t sx = roi_sx0; sx <= roi_sx1; ++sx) {
            float tx = (float)sx / (float)(k_gmpack_tile_dim - 1u);
            float wx = node->world_min_x + tx * span_x;
            uint32_t sx_m = (sx > 0u) ? (sx - 1u) : 0u;
            uint32_t sx_p = (sx + 1u < k_gmpack_tile_dim) ? (sx + 1u) : (k_gmpack_tile_dim - 1u);

            uint32_t idx = sy * k_gmpack_tile_dim + sx;
            float hc = elev_m[idx];
            if (hc < new_min_e) new_min_e = hc;
            if (hc > new_max_e) new_max_e = hc;

            uint32_t q16 = (uint32_t)lrintf(fminf(fmaxf(hc * 16.0f, 0.0f), 65535.0f));
            tile->height_u16[idx] = (uint16_t)q16;

            float hl = elev_m[sy * k_gmpack_tile_dim + sx_m];
            float hr = elev_m[sy * k_gmpack_tile_dim + sx_p];
            float hd = elev_m[sy_m * k_gmpack_tile_dim + sx];
            float hu = elev_m[sy_p * k_gmpack_tile_dim + sx];

            float dzdx = (hr - hl) / (2.0f * step_m);
            float dzdy = (hu - hd) / (2.0f * step_m);
            float grad = sqrtf(dzdx * dzdx + dzdy * dzdy);
            float inv_n = 1.0f / sqrtf(dzdx * dzdx + dzdy * dzdy + 1.0f);
            float nx = -dzdx * inv_n;
            float ny = -dzdy * inv_n;
            float nz =  1.0f * inv_n;

            uint32_t orig_rgba = tile->imagery_rgba8[idx];
            float base_r = (float)( orig_rgba         & 0xFFu);
            float base_g = (float)((orig_rgba >>  8u) & 0xFFu);
            float base_b = (float)((orig_rgba >> 16u) & 0xFFu);
            uint32_t ao_u8 = (orig_rgba >> 24u) & 0xFFu;
            if (dem_posts_changed > 0u) {
                float ao = fminf(fmaxf(0.84f - 0.16f * grad, 0.125f), 1.0f);
                ao_u8 = (uint32_t)lrintf(ao * 255.0f);
            }

            float accum_r = 0.0f;
            float accum_g = 0.0f;
            float accum_b = 0.0f;
            float accum_w = 0.0f;
            float max_photo_alpha = 0.0f;

            // Projective ortho-rectification across all overlapping calibrated photos
            for (uint32_t ai = 0u; ai < active_photo_cnt; ++ai) {
                const TerrainSurveyPhoto *ph = &ds->photos[active_photos[ai]];
                float u_px = 0.0f, v_px = 0.0f, depth_m = 0.0f;
                if (!backend_survey_project_world_to_pixel(&ph->intrinsics, &ph->pose,
                                                           wx, wy, hc, &u_px, &v_px, &depth_m)) {
                    continue;
                }

                // Ray vector from camera to surface point
                float rx = wx - ph->pose.world_x;
                float ry = wy - ph->pose.world_y;
                float rz = hc - ph->pose.altitude_msl_m;
                float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rlen < 0.2f) {
                    continue;
                }
                float cos_inc = -(rx * nx + ry * ny + rz * nz) / rlen;
                if (cos_inc <= 0.05f) {
                    continue;
                }

                // Fast AABB-clipped DEM heightfield ray-marching self-occlusion check inside the tile
                bool occluded = false;
                if (!(cos_inc > 0.86f && (tile_ceil_z - hc) < 45.0f)) {
                    float dir_x = -rx;
                    float dir_y = -ry;
                    float dir_z = -rz;
                    float tx_max = 1.0f;
                    if (dir_x > 1.0e-5f) {
                        tx_max = (node->world_max_x - wx) / dir_x;
                    } else if (dir_x < -1.0e-5f) {
                        tx_max = (node->world_min_x - wx) / dir_x;
                    }
                    float ty_max = 1.0f;
                    if (dir_y > 1.0e-5f) {
                        ty_max = (node->world_max_y - wy) / dir_y;
                    } else if (dir_y < -1.0e-5f) {
                        ty_max = (node->world_min_y - wy) / dir_y;
                    }
                    float tz_max = 1.0f;
                    if (dir_z > 1.0e-5f && tile_ceil_z > hc) {
                        tz_max = (tile_ceil_z - hc) / dir_z;
                    }
                    float t_clip = fminf(1.0f, fminf(fmaxf(tx_max, 0.0f), fminf(fmaxf(ty_max, 0.0f), fmaxf(tz_max, 0.0f))));
                    if (t_clip > 1.0e-4f) {
                        for (uint32_t step = 1u; step <= 10u; ++step) {
                            float frac = t_clip * ((float)step * 0.1f);
                            float mx   = wx + dir_x * frac;
                            float my   = wy + dir_y * frac;
                            float mz   = hc + dir_z * frac;
                            if (mx >= node->world_min_x && mx <= node->world_max_x &&
                                my >= node->world_min_y && my <= node->world_max_y) {
                                float mu = (mx - node->world_min_x) / span_x;
                                float mv = (my - node->world_min_y) / span_y;
                                float mh = sample_elev_grid_bilinear(elev_m, mu, mv);
                                if (mz + 1.25f < mh) {
                                    occluded = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (occluded) {
                    continue;
                }

                float u_norm = u_px / (float)(ph->intrinsics.width_px - 1u);
                float v_norm = v_px / (float)(ph->intrinsics.height_px - 1u);
                float v_top_min = (ph->pose.pitch_deg > -65.0f && ph->pose.pitch_deg < -15.0f) ? 0.16f : 0.0f;
                float v_top_max = (ph->pose.pitch_deg > -65.0f && ph->pose.pitch_deg < -15.0f) ? 0.28f : 0.07f;
                float feather = smoothstep01(0.0f, 0.07f, u_norm) *
                                smoothstep01(0.0f, 0.07f, 1.0f - u_norm) *
                                smoothstep01(v_top_min, v_top_max, v_norm) *
                                smoothstep01(0.0f, 0.07f, 1.0f - v_norm);
                if (ph->pose.pitch_deg > -65.0f && ph->pose.pitch_deg < -15.0f) {
                    float dist_fade = 1.0f - smoothstep01(320.0f, 650.0f, depth_m);
                    feather *= dist_fade;
                }
                if (feather <= 1.0e-4f) {
                    continue;
                }

                float rgba[4];
                sample_photo_bilinear_rgba(ph, u_px, v_px, rgba);
                float tex_alpha = rgba[3] * (1.0f / 255.0f);
                if (tex_alpha <= 1.0e-3f) {
                    continue;
                }

                // Weight higher-resolution (smaller GSD = depth / fx) and more orthogonal views higher
                float gsd_m   = fmaxf(depth_m / ph->intrinsics.fx_px, 0.02f);
                float res_w   = 1.0f / (0.25f + gsd_m);
                float angle_w = 0.25f + 0.75f * cos_inc;
                float view_w  = ph->blend_weight * feather * tex_alpha * angle_w * res_w;

                accum_r += rgba[0] * view_w;
                accum_g += rgba[1] * view_w;
                accum_b += rgba[2] * view_w;
                accum_w += view_w;

                float alpha_eff = ph->blend_weight * feather * tex_alpha;
                if (alpha_eff > max_photo_alpha) {
                    max_photo_alpha = alpha_eff;
                }
            }

            // Optional colored point-cloud RGB contribution
            for (uint32_t pi = 0u; pi < active_pt_cnt; ++pi) {
                const TerrainSurveyPoint *pt = &ds->points[active_pts[pi]];
                if (!pt->has_color) {
                    continue;
                }
                float eff_r = fmaxf(pt->radius_m, min_kernel_r);
                float dx = wx - pt->world_x;
                float dy = wy - pt->world_y;
                float d2 = dx * dx + dy * dy;
                float r2 = eff_r * eff_r;
                if (d2 < r2) {
                    float q  = sqrtf(d2) / eff_r;
                    float pw = pt->weight * wendland_c2(q);
                    if (pw > 1.0e-3f) {
                        accum_r += (float)pt->r * pw;
                        accum_g += (float)pt->g * pw;
                        accum_b += (float)pt->b * pw;
                        accum_w += pw;
                        if (pw > max_photo_alpha) {
                            max_photo_alpha = pw;
                        }
                    }
                }
            }

            if (accum_w > 1.0e-5f && max_photo_alpha > 1.0e-3f) {
                float fused_r = accum_r / accum_w;
                float fused_g = accum_g / accum_w;
                float fused_b = accum_b / accum_w;
                float blend   = fminf(max_photo_alpha, 1.0f);
                base_r = (1.0f - blend) * base_r + blend * fused_r;
                base_g = (1.0f - blend) * base_g + blend * fused_g;
                base_b = (1.0f - blend) * base_b + blend * fused_b;
                texels_fused++;
            }

            uint32_t R = (uint32_t)lrintf(fminf(fmaxf(base_r, 0.0f), 255.0f));
            uint32_t G = (uint32_t)lrintf(fminf(fmaxf(base_g, 0.0f), 255.0f));
            uint32_t B = (uint32_t)lrintf(fminf(fmaxf(base_b, 0.0f), 255.0f));
            tile->imagery_rgba8[idx] = R | (G << 8u) | (B << 16u) | (ao_u8 << 24u);
        }
    }

    free(elev_m);
    texels_fused += ortho_baked_texels;

    if (dem_posts_changed == 0u && texels_fused == 0u) {
        return false;
    }

    node->header      = gpumath_tile_header_pack(blk_idx, morton, lod, flags, new_min_e, new_max_e);
    node->elev_mean_m = sum_e / (float)k_gmpack_tile_samples;
    if (new_min_e < hdr->dir.elev_min_m) {
        hdr->dir.elev_min_m = new_min_e;
    }
    if (new_max_e > hdr->dir.elev_max_m) {
        hdr->dir.elev_max_m = new_max_e;
    }

    if (io_stats != nullptr) {
        bool should_accumulate = true;
        if (io_stats == &ds->last_stats) {
            if (ds->node_stats_counted[node_index] != 0u) {
                should_accumulate = false;
            } else {
                ds->node_stats_counted[node_index] = 1u;
            }
        }
        if (should_accumulate) {
            uint32_t prev_mvs = io_stats->mvs_elev_posts_refined;
            io_stats->tiles_updated          += 1u;
            io_stats->texels_orthorectified  += texels_fused;
            io_stats->dem_posts_refined      += dem_posts_changed;
            io_stats->mvs_elev_posts_refined += mvs_posts_changed;
            if (mvs_posts_changed > 0u) {
                double prev_sum = (double)io_stats->mean_mvs_ncc * (double)prev_mvs;
                io_stats->mean_mvs_ncc = (float)((prev_sum + mvs_ncc_sum) /
                                                 (double)(prev_mvs + mvs_posts_changed));
            }
        }
        if (max_elev_delta > io_stats->max_elev_delta_m) {
            io_stats->max_elev_delta_m = max_elev_delta;
        }
    }
    return true;
}

static int pwrite_exact(int fd, const void *src, size_t bytes, off_t offset) {
    const uint8_t *in = (const uint8_t *)src;
    size_t done = 0u;
    while (done < bytes) {
        ssize_t n = pwrite(fd, in + done, bytes - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int backend_survey_apply_to_pack_file(TerrainSurveyDataset *ds,
                                      const char           *pack_path,
                                      TerrainSurveyStats   *out_stats) {
    if (ds == nullptr || pack_path == nullptr || pack_path[0] == '\0') {
        return -1;
    }
    if (backend_terrain_ensure_pack(pack_path) != 0) {
        return -1;
    }

    GpuMathArchiveHeader *hdr = aligned_alloc(4096u, sizeof(GpuMathArchiveHeader));
    GpuMathTileBlock     *blk = aligned_alloc(4096u, sizeof(GpuMathTileBlock));
    if (hdr == nullptr || blk == nullptr) {
        free(hdr);
        free(blk);
        return -1;
    }
    if (backend_terrain_read_header(pack_path, hdr) != 0) {
        free(hdr);
        free(blk);
        return -1;
    }

    int fd = open(pack_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        free(hdr);
        free(blk);
        return -1;
    }

    TerrainSurveyStats stats = ds->last_stats;
    stats.tiles_updated          = 0u;
    stats.texels_orthorectified  = 0u;
    stats.dem_posts_refined      = 0u;
    stats.mvs_elev_posts_refined = 0u;
    stats.max_elev_delta_m       = 0.0f;
    stats.mean_mvs_ncc           = 0.0f;

    for (uint32_t idx = 0u; idx < k_gmpack_node_count; ++idx) {
        if (backend_terrain_read_tile(pack_path, idx, blk) != 0) {
            continue;
        }
        if (backend_survey_fuse_tile(ds, hdr, idx, blk, &stats)) {
            ds->node_stats_counted[idx] = 1u;
            uint64_t ssd_off = relptr2d_resolve_addr(0ull, hdr->nodes[idx].rel_ssd_payload);
            if (ssd_off >= k_gmpack_header_bytes &&
                ssd_off + k_gmpack_tile_bytes <= k_gmpack_total_bytes) {
                (void)pwrite_exact(fd, blk, sizeof(GpuMathTileBlock), (off_t)ssd_off);
            }
        }
    }

    if (stats.tiles_updated > 0u) {
        (void)pwrite_exact(fd, hdr, sizeof(GpuMathArchiveHeader), 0);
    }

    close(fd);
    free(hdr);
    free(blk);

    ds->last_stats = stats;
    if (out_stats != nullptr) {
        *out_stats = stats;
    }
    return 0;
}
