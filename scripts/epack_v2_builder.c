#include "backend/terrain/pack.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct EpackV2BuildMeta {
    double   lat_min_deg;
    double   lat_max_deg;
    double   lon_min_deg;
    double   lon_max_deg;
    float    world_half_extent;
    float    elev_min_m;
    float    elev_max_m;
    float    center_east_m;
    float    center_north_m;
    uint32_t total_node_count;
    uint32_t max_lod;
    uint32_t epsg_code;
    uint32_t dem_res_mm;
    uint32_t ortho_res_mm;
    uint64_t disk_table_offset;
    uint64_t tile_data_offset;
} EpackV2BuildMeta;

static inline uint16_t clamp_height_u16(float elev_m) {
    if (!isfinite(elev_m) || elev_m <= 0.0f) {
        return 0u;
    }
    float scaled = roundf(elev_m * 16.0f);
    if (scaled >= 65535.0f) {
        return 65535u;
    }
    return (uint16_t)scaled;
}

// Encodes a K x K grid of 256x256 tiles out of a North-up block image of dimensions
// (side x side) where side = K * 255 + 1 (shared adjacent borders!).
// Row 0 of dem_north_up / rgb_north_up is North (+Y).
// Output tiles are ordered by ty in 0..K-1 (South to North!), tx in 0..K-1 (West to East!),
// with tile row sy=0 = South boundary y0 and sy=255 = North boundary y1.
void epack_v2_encode_block_tiles(const float   *dem_north_up,
                                 const uint8_t *rgb_north_up,
                                 uint32_t       block_k,
                                 uint32_t       stride_px,
                                 int            rgb_only,
                                 uint8_t       *out_payloads,
                                 float         *out_min_m,
                                 float         *out_max_m,
                                 float         *out_mean_m) {
    if (rgb_north_up == nullptr || out_payloads == nullptr || block_k == 0u || stride_px == 0u) {
        return;
    }

    static _Thread_local GpuMathTileBlock src_blk;
    static _Thread_local EpackTileBlock   enc_blk;

    const uint32_t tile_stride_bytes = rgb_only ? k_epack_v2_rgb_bytes : k_epack_v2_dem_rgb_bytes;
    const uint32_t total_px_side     = block_k * 255u + 1u;

    for (uint32_t ty = 0u; ty < block_k; ++ty) {
        for (uint32_t tx = 0u; tx < block_k; ++tx) {
            const uint32_t tile_i = ty * block_k + tx;
            float          tmin   =  1.0e9f;
            float          tmax   = -1.0e9f;
            double         tsum   =  0.0;

            for (uint32_t sy = 0u; sy < k_gmpack_tile_dim; ++sy) {
                // Convert South-up sy (0..255) -> North-up image row
                const uint32_t south_row = ty * 255u + sy;
                const uint32_t north_row = (total_px_side - 1u) - south_row;
                const uint32_t base_col  = tx * 255u;

                for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
                    const uint32_t col     = base_col + sx;
                    const size_t   src_idx = (size_t)north_row * (size_t)stride_px + (size_t)col;
                    const uint32_t dst_idx = sy * k_gmpack_tile_dim + sx;

                    if (rgb_only == 0 && dem_north_up != nullptr) {
                        float h = dem_north_up[src_idx];
                        if (!isfinite(h) || h < 0.0f) {
                            h = 0.0f;
                        }
                        if (h < tmin) tmin = h;
                        if (h > tmax) tmax = h;
                        tsum += (double)h;
                        src_blk.height_u16[dst_idx] = clamp_height_u16(h);
                    } else {
                        src_blk.height_u16[dst_idx] = 0u;
                    }

                    const size_t rgb_off = src_idx * 3u;
                    const uint32_t r     = rgb_north_up[rgb_off + 0u];
                    const uint32_t g     = rgb_north_up[rgb_off + 1u];
                    const uint32_t b     = rgb_north_up[rgb_off + 2u];
                    src_blk.imagery_rgba8[dst_idx] = 0xFF000000u | (b << 16u) | (g << 8u) | r;
                }
            }

            if (out_min_m != nullptr) {
                out_min_m[tile_i] = (rgb_only == 0 && tmin < 1.0e8f) ? tmin : 0.0f;
            }
            if (out_max_m != nullptr) {
                out_max_m[tile_i] = (rgb_only == 0 && tmax > -1.0e8f) ? tmax : 0.0f;
            }
            if (out_mean_m != nullptr) {
                out_mean_m[tile_i] = (rgb_only == 0)
                    ? (float)(tsum / (double)k_gmpack_tile_samples)
                    : 0.0f;
            }

            backend_epack_encode_tile(&src_blk, &enc_blk);
            uint8_t *dst_ptr = out_payloads + (size_t)tile_i * (size_t)tile_stride_bytes;
            if (rgb_only != 0) {
                memcpy(dst_ptr, (const uint8_t *)&enc_blk + k_epack_v2_rgb_offset, k_epack_v2_rgb_bytes);
            } else {
                memcpy(dst_ptr, (const uint8_t *)&enc_blk, k_epack_v2_dem_rgb_bytes);
            }
        }
    }
}

void epack_v2_downsample_2x_north_up(const float   *src_dem,
                                     const uint8_t *src_rgb,
                                     uint32_t       src_side,
                                     float         *dst_dem,
                                     uint8_t       *dst_rgb,
                                     uint32_t       dst_side) {
    // src_side = 2 * dst_side - 1 (exact shared endpoint stride-2 sampling)
    // Height uses exact shared vertex post (bit-exact border agreement!),
    // RGB uses subtle 3x3 tent filter inside interior and exact post on borders.
    for (uint32_t dy = 0u; dy < dst_side; ++dy) {
        const uint32_t sy = dy * 2u;
        const uint32_t sy_m = (sy > 0u) ? (sy - 1u) : 0u;
        const uint32_t sy_p = (sy + 1u < src_side) ? (sy + 1u) : (src_side - 1u);

        for (uint32_t dx = 0u; dx < dst_side; ++dx) {
            const uint32_t sx = dx * 2u;
            const size_t   di = (size_t)dy * (size_t)dst_side + (size_t)dx;
            const size_t   si = (size_t)sy * (size_t)src_side + (size_t)sx;

            if (src_dem != nullptr && dst_dem != nullptr) {
                dst_dem[di] = src_dem[si];
            }
            if (src_rgb != nullptr && dst_rgb != nullptr) {
                if (dx == 0u || dy == 0u || dx + 1u == dst_side || dy + 1u == dst_side) {
                    dst_rgb[di * 3u + 0u] = src_rgb[si * 3u + 0u];
                    dst_rgb[di * 3u + 1u] = src_rgb[si * 3u + 1u];
                    dst_rgb[di * 3u + 2u] = src_rgb[si * 3u + 2u];
                } else {
                    const uint32_t sx_m = sx - 1u;
                    const uint32_t sx_p = sx + 1u;
                    for (uint32_t c = 0u; c < 3u; ++c) {
                        uint32_t acc = 4u * src_rgb[((size_t)sy   * src_side + sx  ) * 3u + c] +
                                       2u * src_rgb[((size_t)sy   * src_side + sx_m) * 3u + c] +
                                       2u * src_rgb[((size_t)sy   * src_side + sx_p) * 3u + c] +
                                       2u * src_rgb[((size_t)sy_m * src_side + sx  ) * 3u + c] +
                                       2u * src_rgb[((size_t)sy_p * src_side + sx  ) * 3u + c] +
                                       1u * src_rgb[((size_t)sy_m * src_side + sx_m) * 3u + c] +
                                       1u * src_rgb[((size_t)sy_m * src_side + sx_p) * 3u + c] +
                                       1u * src_rgb[((size_t)sy_p * src_side + sx_m) * 3u + c] +
                                       1u * src_rgb[((size_t)sy_p * src_side + sx_p) * 3u + c];
                        dst_rgb[di * 3u + c] = (uint8_t)((acc + 8u) >> 4u);
                    }
                }
            }
        }
    }
}

int epack_v2_write_header(const char *path, const EpackV2BuildMeta *meta) {
    if (path == nullptr || meta == nullptr) {
        return -1;
    }
    GpuMathArchiveHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.dir.magic             = k_epack_v2_magic;
    hdr.dir.version           = k_epack_v2_version;
    hdr.dir.node_count        = 1u;
    hdr.dir.tile_dim          = k_gmpack_tile_dim;
    hdr.dir.world_half_extent = meta->world_half_extent;
    hdr.dir.elev_min_m        = meta->elev_min_m;
    hdr.dir.elev_max_m        = meta->elev_max_m;
    hdr.dir.vertical_scale    = 1.0f;
    hdr.dir.lat_min_deg       = meta->lat_min_deg;
    hdr.dir.lat_max_deg       = meta->lat_max_deg;
    hdr.dir.lon_min_deg       = meta->lon_min_deg;
    hdr.dir.lon_max_deg       = meta->lon_max_deg;

    EpackV2Meta *m       = backend_epack_v2_meta(&hdr);
    m->v2_magic          = k_epack_v2_magic;
    m->total_node_count  = meta->total_node_count;
    m->max_lod           = meta->max_lod;
    m->flags             = k_epack_v2_flag_real;
    m->disk_table_offset = meta->disk_table_offset;
    m->tile_data_offset  = meta->tile_data_offset;
    m->epsg_code         = meta->epsg_code;
    m->center_east_m     = meta->center_east_m;
    m->center_north_m    = meta->center_north_m;
    m->dem_res_mm        = meta->dem_res_mm;
    m->ortho_res_mm      = meta->ortho_res_mm;

    FILE *f = fopen(path, "r+b");
    if (f == nullptr) {
        return -1;
    }
    bool ok = (fseeko(f, 0, SEEK_SET) == 0) &&
              (fwrite(&hdr, sizeof(hdr), 1u, f) == 1u);
    fclose(f);
    return ok ? 0 : -1;
}

int epack_v2_fix_tile_rim(uint8_t *tile_86k,
                          float   *out_min_m,
                          float   *out_max_m,
                          float   *out_mean_m) {
    if (tile_86k == nullptr) {
        return -1;
    }

    static _Thread_local EpackTileBlock   enc_in;
    static _Thread_local GpuMathTileBlock raw;
    static _Thread_local EpackTileBlock   enc_out;

    memset(&enc_in, 0, sizeof(enc_in));
    memcpy(&enc_in, tile_86k, k_epack_v2_dem_rgb_bytes);
    backend_epack_decode_tile(&enc_in, &raw);

    bool changed = false;
    for (int pass = 0; pass < 6; ++pass) {
        bool any_zero = false;
        static _Thread_local uint16_t next_h[k_gmpack_tile_samples];
        memcpy(next_h, raw.height_u16, sizeof(next_h));
        for (uint32_t y = 0u; y < k_gmpack_tile_dim; ++y) {
            for (uint32_t x = 0u; x < k_gmpack_tile_dim; ++x) {
                const uint32_t idx = y * k_gmpack_tile_dim + x;
                if (raw.height_u16[idx] >= 16u) {
                    continue;
                }
                any_zero = true;
                uint16_t nbr = 0u;
                if (y + 1u < k_gmpack_tile_dim && raw.height_u16[(y + 1u) * k_gmpack_tile_dim + x] >= 16u) {
                    nbr = raw.height_u16[(y + 1u) * k_gmpack_tile_dim + x];
                } else if (y > 0u && raw.height_u16[(y - 1u) * k_gmpack_tile_dim + x] >= 16u) {
                    nbr = raw.height_u16[(y - 1u) * k_gmpack_tile_dim + x];
                } else if (x > 0u && raw.height_u16[y * k_gmpack_tile_dim + (x - 1u)] >= 16u) {
                    nbr = raw.height_u16[y * k_gmpack_tile_dim + (x - 1u)];
                } else if (x + 1u < k_gmpack_tile_dim && raw.height_u16[y * k_gmpack_tile_dim + (x + 1u)] >= 16u) {
                    nbr = raw.height_u16[y * k_gmpack_tile_dim + (x + 1u)];
                }
                if (nbr >= 16u) {
                    next_h[idx] = nbr;
                    changed = true;
                }
            }
        }
        memcpy(raw.height_u16, next_h, sizeof(next_h));
        if (!any_zero) {
            break;
        }
    }

    float  tmin =  1.0e9f;
    float  tmax = -1.0e9f;
    double tsum =  0.0;
    for (uint32_t i = 0u; i < k_gmpack_tile_samples; ++i) {
        float h = (float)raw.height_u16[i] * 0.0625f;
        if (h < tmin) tmin = h;
        if (h > tmax) tmax = h;
        tsum += (double)h;
    }
    if (out_min_m != nullptr)  *out_min_m  = tmin;
    if (out_max_m != nullptr)  *out_max_m  = tmax;
    if (out_mean_m != nullptr) *out_mean_m = (float)(tsum / (double)k_gmpack_tile_samples);

    if (!changed) {
        return 0;
    }

    backend_epack_encode_tile(&raw, &enc_out);
    // Preserve bit-exact border_rgb565 & color_blocks; overwrite only height payload (elev_patches + border_height_u16)
    memcpy(tile_86k,
           &enc_out,
           offsetof(EpackTileBlock, border_rgb565));
    return 1;
}
