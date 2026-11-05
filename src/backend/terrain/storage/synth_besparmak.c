#include "backend/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Calibrated Baklan & Beşparmak Dağı (Denizli, Türkiye) Quadtree Synthesizer
// ============================================================================
// Used as an offline/air-gapped fallback if examples/demo/maps/baklan_besparmak.gmpack
// has not yet been fetched from AWS Terrarium DEM + Esri/OpenTopoMap.
// Accurately models:
//   - Baklan Ovası agricultural plain (828..845 m) in the north/northwest
//   - Baklan town alluvial slope (37.976748° N, 29.606909° E, ~865 m)
//   - Beşparmak Dağı West Peak (37.952201° N, 29.617113° E, ~1,307 m)
//   - Beşparmak Dağı Five-Fingered Main Ridge (37.960556° N, 29.636667° E, 1,612 m)

static float hash2d(float x, float y) {
    float s = sinf(x * 127.1f + y * 311.7f) * 43758.5453f;
    return s - floorf(s);
}

static float noise2d(float x, float y) {
    float ix = floorf(x);
    float iy = floorf(y);
    float fx = x - ix;
    float fy = y - iy;
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash2d(ix, iy);
    float b = hash2d(ix + 1.0f, iy);
    float c = hash2d(ix, iy + 1.0f);
    float d = hash2d(ix + 1.0f, iy + 1.0f);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

static float fbm_ridge(float x, float y) {
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    for (int i = 0; i < 5; ++i) {
        float n = 1.0f - fabsf(2.0f * noise2d(x * freq, y * freq) - 1.0f);
        sum += (n * n) * amp;
        freq *= 2.07f;
        amp *= 0.48f;
    }
    return sum;
}

typedef struct RidgePeak {
    double lat;
    double lon;
    float  height_m;
    float  rx_deg;
    float  ry_deg;
    float  angle_rad;
} RidgePeak;

static float eval_besparmak_elevation_m(double lat, double lon) {
    // Base Baklan Ovası floor (~832 m, gently sloping south-to-north toward Büyük Menderes)
    float base = 832.0f + (float)((37.996 - lat) * 95.0);

    // Five-fingered Beşparmak Dağı massif crests (WNW-ESE strike across Baklan-Çal)
    static constexpr RidgePeak k_peaks[5] = {
        { 37.960556, 29.636667, 755.0f, 0.026f, 0.014f, -0.32f }, // Main Summit (1,612 m)
        { 37.952201, 29.617113, 470.0f, 0.022f, 0.013f, -0.25f }, // West Peak (1,307 m)
        { 37.956500, 29.627500, 610.0f, 0.020f, 0.011f, -0.28f }, // Central Finger (~1,465 m)
        { 37.963800, 29.648200, 560.0f, 0.023f, 0.012f, -0.36f }, // East Finger (~1,415 m)
        { 37.946800, 29.606200, 340.0f, 0.024f, 0.015f, -0.18f }, // SW Foothill Spur (~1,185 m)
    };

    float ridge_gain = 0.0f;
    for (size_t i = 0; i < 5u; ++i) {
        float dx = (float)(lon - k_peaks[i].lon);
        float dy = (float)(lat - k_peaks[i].lat);
        float c = cosf(k_peaks[i].angle_rad);
        float s = sinf(k_peaks[i].angle_rad);
        float rx = (dx * c - dy * s) / k_peaks[i].rx_deg;
        float ry = (dx * s + dy * c) / k_peaks[i].ry_deg;
        float r2 = rx * rx + ry * ry;
        float env = expf(-0.5f * r2);
        ridge_gain += k_peaks[i].height_m * env;
    }

    // Add multi-fractal dolomite crags on the upper slopes
    float crag_mask = fminf(fmaxf(ridge_gain / 260.0f, 0.0f), 1.0f);
    float crags = (fbm_ridge((float)(lon - 29.53) * 85.0f, (float)(lat - 37.85) * 85.0f) - 0.35f) *
                  145.0f * crag_mask;

    float h = base + ridge_gain + crags;
    return fminf(fmaxf(h, k_besparmak_elev_min), k_besparmak_elev_max);
}

static uint32_t eval_besparmak_imagery_rgba(double lat, double lon, float elev_m, float slope_deg) {
    // Realistic Aegean/Anatolian satellite + topographic map color synthesis:
    //   - Baklan Plain (< 875 m): geometric agricultural field parcels (olive, wheat, fallow ochre)
    //   - Baklan Town (around 37.9767 N, 29.6069 E): terracotta roofs & road grid
    //   - Mid-slopes (880..1280 m): Mediterranean maquis / pine scrub (deep olive-green & limestone)
    //   - High Beşparmak ridges (> 1280 m): exposed pale grey-white dolomite rock & scree
    float u = (float)((lon - k_besparmak_lon_min) / (k_besparmak_lon_max - k_besparmak_lon_min));
    float v = (float)((lat - k_besparmak_lat_min) / (k_besparmak_lat_max - k_besparmak_lat_min));

    float r = 0.42f;
    float g = 0.46f;
    float b = 0.34f;

    if (elev_m < 885.0f) {
        // Baklan Ovası agricultural parcel pattern
        float px = floorf(u * 64.0f);
        float py = floorf(v * 64.0f);
        float ph = hash2d(px, py);
        if (ph < 0.34f) {
            r = 0.56f; g = 0.52f; b = 0.34f; // Harvested wheat / golden soil
        } else if (ph < 0.68f) {
            r = 0.30f; g = 0.45f; b = 0.24f; // Irrigated green crop parcel
        } else {
            r = 0.48f; g = 0.39f; b = 0.28f; // Ploughed Anatolian loam
        }
        // Baklan town urban cluster around (37.976748, 29.606909)
        float tdx = (float)((lon - k_baklan_center_lon) * 110.0);
        float tdy = (float)((lat - k_baklan_center_lat) * 110.0);
        float td2 = tdx * tdx + tdy * tdy;
        if (td2 < 1.2f) {
            float street = fmodf(fabsf(tdx * 18.0f), 1.0f) < 0.16f ||
                           fmodf(fabsf(tdy * 18.0f), 1.0f) < 0.16f;
            if (street) {
                r = 0.68f; g = 0.67f; b = 0.64f;
            } else {
                r = 0.72f; g = 0.43f; b = 0.33f; // Terracotta roofs
            }
        }
    } else if (elev_m < 1260.0f) {
        float t = (elev_m - 885.0f) / (1260.0f - 885.0f);
        float scrub = noise2d(u * 96.0f, v * 96.0f);
        r = (0.31f + 0.24f * t) + 0.06f * scrub;
        g = (0.41f + 0.12f * t) + 0.05f * scrub;
        b = (0.25f + 0.20f * t) + 0.04f * scrub;
    } else {
        float t = fminf((elev_m - 1260.0f) / (1612.0f - 1260.0f), 1.0f);
        r = 0.62f + 0.22f * t;
        g = 0.60f + 0.22f * t;
        b = 0.56f + 0.23f * t;
    }

    // Topographic 50m contour lines baked gently into the map imagery
    float contour_dist = fabsf(fmodf(elev_m, 50.0f) - 25.0f);
    if (contour_dist > 23.2f) {
        r *= 0.82f;
        g *= 0.79f;
        b *= 0.74f;
    }

    uint32_t R = (uint32_t)lrintf(fminf(fmaxf(r * 255.0f, 0.0f), 255.0f));
    uint32_t G = (uint32_t)lrintf(fminf(fmaxf(g * 255.0f, 0.0f), 255.0f));
    uint32_t B = (uint32_t)lrintf(fminf(fmaxf(b * 255.0f, 0.0f), 255.0f));
    uint32_t A = (uint32_t)lrintf(fminf(fmaxf((1.0f - slope_deg / 90.0f) * 255.0f, 32.0f), 255.0f));
    return R | (G << 8u) | (B << 16u) | (A << 24u);
}

int backend_synth_besparmak_archive(const char *pack_path) {
    if (pack_path == nullptr || pack_path[0] == '\0') {
        return -1;
    }
    char dir_buf[1024];
    snprintf(dir_buf, sizeof dir_buf, "%s", pack_path);
    char *slash = strrchr(dir_buf, '/');
    if (slash != nullptr) {
        *slash = '\0';
        (void)mkdir(dir_buf, 0755);
    }

    uint8_t *image = aligned_alloc(4096u, k_gmpack_total_bytes);
    if (image == nullptr) {
        return -1;
    }
    memset(image, 0, k_gmpack_total_bytes);

    GpuMathArchiveHeader *hdr = (GpuMathArchiveHeader *)image;
    GpuMathTileBlock *tiles   = (GpuMathTileBlock *)(image + k_gmpack_header_bytes);

    hdr->dir = (GpuMathPageDirectory){
        .magic               = k_gmpack_magic,
        .version             = k_gmpack_version,
        .node_count          = k_gmpack_node_count,
        .tile_dim            = k_gmpack_tile_dim,
        .world_half_extent   = k_terrain_world_half,
        .elev_min_m          = k_besparmak_elev_min,
        .elev_max_m          = k_besparmak_elev_max,
        .vertical_scale      = 1.15f,
        .lat_min_deg         = k_besparmak_lat_min,
        .lat_max_deg         = k_besparmak_lat_max,
        .lon_min_deg         = k_besparmak_lon_min,
        .lon_max_deg         = k_besparmak_lon_max,
        .root_node_rel       = relptr2d_pack(0, 0, 0, 0u, 1u),
        .tile_pool_rel       = relptr2d_pack((int32_t)k_gmpack_header_blocks, 0, 0, 0u, 1u),
        .active_leaf_count   = 1u,
        .resident_tile_count = 1u,
        .stream_loads_total  = 0u,
        .deepest_lod_active  = 0u,
        .focus_world_x       = 0.0f,
        .focus_world_y       = 0.0f,
        .focus_zoom          = 1.0f,
        .frame_epoch         = 1u,
    };
    hdr->dir.active_leaves[0] = 0u;

    for (uint8_t lod = 0u; lod < k_gmpack_lod_count; ++lod) {
        uint8_t grid_n = (uint8_t)(1u << lod);
        float tile_span_world = (k_terrain_world_half * 2.0f) / (float)grid_n;
        uint8_t gx_start = (lod == 4u) ? 4u : 0u;
        uint8_t gx_end   = (lod == 4u) ? 12u : grid_n;
        uint8_t gy_start = (lod == 4u) ? 4u : 0u;
        uint8_t gy_end   = (lod == 4u) ? 12u : grid_n;

        for (uint8_t gy = gy_start; gy < gy_end; ++gy) {
            for (uint8_t gx = gx_start; gx < gx_end; ++gx) {
                uint32_t idx = backend_quadtree_node_index(lod, gx, gy);
                GpuMathQuadNode *node = &hdr->nodes[idx];
                GpuMathTileBlock *blk = &tiles[idx];

                float w_min_x = -k_terrain_world_half + (float)gx * tile_span_world;
                float w_max_x = w_min_x + tile_span_world;
                // gy=0 is North (+Y in world), gy=grid_n-1 is South (-Y in world)
                float w_max_y = k_terrain_world_half - (float)gy * tile_span_world;
                float w_min_y = w_max_y - tile_span_world;

                double lon0 = k_besparmak_lon_min +
                              ((double)gx / (double)grid_n) * (k_besparmak_lon_max - k_besparmak_lon_min);
                double lon1 = k_besparmak_lon_min +
                              ((double)(gx + 1u) / (double)grid_n) * (k_besparmak_lon_max - k_besparmak_lon_min);
                double lat1 = k_besparmak_lat_max -
                              ((double)gy / (double)grid_n) * (k_besparmak_lat_max - k_besparmak_lat_min);
                double lat0 = k_besparmak_lat_max -
                              ((double)(gy + 1u) / (double)grid_n) * (k_besparmak_lat_max - k_besparmak_lat_min);

                float min_e = 1.0e9f;
                float max_e = -1.0e9f;
                float sum_e = 0.0f;

                for (uint32_t sy = 0u; sy < k_gmpack_tile_dim; ++sy) {
                    double ty = (double)sy / (double)(k_gmpack_tile_dim - 1u);
                    // sy=0 is South (w_min_y, lat0), sy=255 is North (w_max_y, lat1)
                    double lat = lat0 + ty * (lat1 - lat0);
                    for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
                        double tx = (double)sx / (double)(k_gmpack_tile_dim - 1u);
                        double lon = lon0 + tx * (lon1 - lon0);
                        float elev = eval_besparmak_elevation_m(lat, lon);
                        if (elev < min_e) min_e = elev;
                        if (elev > max_e) max_e = elev;
                        sum_e += elev;

                        uint32_t q16 = (uint32_t)lrintf(fminf(fmaxf(elev * 16.0f, 0.0f), 65535.0f));
                        blk->height_u16[sy * k_gmpack_tile_dim + sx] = (uint16_t)q16;
                    }
                }

                // Compute slope and satellite/topographic RGBA8 per grid cell
                float sample_spacing_m = 24000.0f / (float)(grid_n * (k_gmpack_tile_dim - 1u));
                for (uint32_t sy = 0u; sy < k_gmpack_tile_dim; ++sy) {
                    double ty = (double)sy / (double)(k_gmpack_tile_dim - 1u);
                    double lat = lat0 + ty * (lat1 - lat0);
                    uint32_t sy_prev = sy > 0u ? sy - 1u : 0u;
                    uint32_t sy_next = sy + 1u < k_gmpack_tile_dim ? sy + 1u : k_gmpack_tile_dim - 1u;
                    for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
                        double tx = (double)sx / (double)(k_gmpack_tile_dim - 1u);
                        double lon = lon0 + tx * (lon1 - lon0);
                        uint32_t sx_prev = sx > 0u ? sx - 1u : 0u;
                        uint32_t sx_next = sx + 1u < k_gmpack_tile_dim ? sx + 1u : k_gmpack_tile_dim - 1u;

                        float h_l = (float)blk->height_u16[sy * k_gmpack_tile_dim + sx_prev] * 0.0625f;
                        float h_r = (float)blk->height_u16[sy * k_gmpack_tile_dim + sx_next] * 0.0625f;
                        float h_d = (float)blk->height_u16[sy_prev * k_gmpack_tile_dim + sx] * 0.0625f;
                        float h_u = (float)blk->height_u16[sy_next * k_gmpack_tile_dim + sx] * 0.0625f;
                        float dzdx = (h_r - h_l) / (2.0f * sample_spacing_m);
                        float dzdy = (h_u - h_d) / (2.0f * sample_spacing_m);
                        float slope_deg = atanf(sqrtf(dzdx * dzdx + dzdy * dzdy)) * 57.2957795f;
                        float elev = (float)blk->height_u16[sy * k_gmpack_tile_dim + sx] * 0.0625f;

                        blk->imagery_rgba8[sy * k_gmpack_tile_dim + sx] =
                            eval_besparmak_imagery_rgba(lat, lon, elev, slope_deg);
                    }
                }

                int32_t ssd_block_4k = (int32_t)(k_gmpack_header_blocks + idx * k_gmpack_tile_blocks_4k);
                uint16_t morton = relptr2d_morton2d_encode(gx, gy);
                uint16_t parent_idx = lod > 0u
                    ? (uint16_t)backend_quadtree_node_index((uint8_t)(lod - 1u), (uint8_t)(gx >> 1u), (uint8_t)(gy >> 1u))
                    : 0u;
                RelPtr2D child_rel = k_relptr2d_null;
                bool has_children = (lod < 3u) || (lod == 3u && gx >= 2u && gx < 6u && gy >= 2u && gy < 6u);
                if (has_children) {
                    uint32_t first_child = backend_quadtree_node_index((uint8_t)(lod + 1u),
                                                                       (uint8_t)(gx * 2u),
                                                                       (uint8_t)(gy * 2u));
                    if (first_child > 0u) {
                        child_rel = relptr2d_pack((int32_t)first_child - (int32_t)idx,
                                                  (int16_t)(gx * 2u), (int16_t)(gy * 2u),
                                                  (uint8_t)(lod + 1u), 1u);
                    }
                }

                *node = (GpuMathQuadNode){
                    .rel_ssd_payload = relptr2d_pack(ssd_block_4k, (int16_t)gx, (int16_t)gy, lod, 1u),
                    .rel_gpu_payload = idx == 0u
                        ? relptr2d_pack((int32_t)k_gmpack_header_blocks, 0, 0, 0u, 1u)
                        : k_relptr2d_null,
                    .rel_children    = child_rel,
                    .header          = gpumath_tile_header_pack((uint32_t)ssd_block_4k, morton, lod,
                                                                idx == 0u ? (k_tile_flag_resident | k_tile_flag_pinned) : 0u,
                                                                min_e, max_e),
                    .world_min_x     = w_min_x,
                    .world_min_y     = w_min_y,
                    .world_max_x     = w_max_x,
                    .world_max_y     = w_max_y,
                    .node_index      = (uint16_t)idx,
                    .parent_index    = parent_idx,
                    .gpu_slot        = 0u,
                    .lod             = lod,
                    .grid_x          = gx,
                    .grid_y          = gy,
                    .resident        = idx == 0u ? 1u : 0u,
                    .pad_tail        = 0u,
                    .elev_mean_m     = sum_e / (float)k_gmpack_tile_samples,
                };
            }
        }
    }

    int fd = open(pack_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(image);
        return -1;
    }
    size_t remaining = k_gmpack_total_bytes;
    const uint8_t *cur = image;
    while (remaining > 0u) {
        ssize_t n = write(fd, cur, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            free(image);
            return -1;
        }
        cur += (size_t)n;
        remaining -= (size_t)n;
    }
    close(fd);
    free(image);
    printf("[Backend] Synthesized & saved 85-tile Baklan & Beşparmak Dağı archive to SSD: %s (%.2f MiB)\n",
           pack_path, (double)k_gmpack_total_bytes / (1024.0 * 1024.0));
    fflush(stdout);
    return 0;
}
