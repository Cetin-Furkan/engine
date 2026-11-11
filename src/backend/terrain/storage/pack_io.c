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
// SSD .gmpack Archive Reader & Self-Relative 2D Pointer Block Verifier
// ============================================================================

uint32_t backend_quadtree_level_base(uint8_t lod) {
    switch (lod) {
    case 0u:
        return 0u;
    case 1u:
        return 1u;
    case 2u:
        return 5u;
    case 3u:
        return 21u;
    case 4u:
        return 85u;
    default:
        return 0u;
    }
}

uint32_t backend_quadtree_node_index(uint8_t lod, uint8_t gx, uint8_t gy) {
    if (lod >= k_gmpack_lod_count) {
        return 0u;
    }
    uint32_t side = 1u << lod;
    if (gx >= side || gy >= side) {
        return 0u;
    }
    return backend_quadtree_level_base(lod) + (uint32_t)gy * side + (uint32_t)gx;
}

static int pread_exact(int fd, void *dst, size_t bytes, off_t offset) {
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0u;
    while (done < bytes) {
        ssize_t n = pread(fd, out + done, bytes - done, offset + (off_t)done);
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

int backend_terrain_ensure_pack(const char *pack_path) {
    if (pack_path == nullptr || pack_path[0] == '\0') {
        return -1;
    }
    struct stat st = {0};
    if (stat(pack_path, &st) == 0 && (size_t)st.st_size == k_gmpack_total_bytes) {
        int fd = open(pack_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            uint32_t magic = 0u;
            uint32_t ver   = 0u;
            if (pread_exact(fd, &magic, sizeof magic, 0) == 0 &&
                pread_exact(fd, &ver, sizeof ver, 4) == 0 &&
                magic == k_gmpack_magic && ver == k_gmpack_version) {
                close(fd);
                return 0;
            }
            close(fd);
        }
    }
    return backend_synth_besparmak_archive(pack_path);
}

int backend_terrain_read_header(const char *pack_path, GpuMathArchiveHeader *out_hdr) {
    if (pack_path == nullptr || out_hdr == nullptr) {
        return -1;
    }
    if (backend_terrain_ensure_pack(pack_path) != 0) {
        return -1;
    }
    int fd = open(pack_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    int rc = pread_exact(fd, out_hdr, sizeof(GpuMathArchiveHeader), 0);
    close(fd);
    if (rc != 0) {
        return -1;
    }
    if (out_hdr->dir.magic != k_gmpack_magic ||
        out_hdr->dir.version != k_gmpack_version ||
        out_hdr->dir.node_count != k_gmpack_node_count) {
        return -1;
    }
    return 0;
}

int backend_terrain_read_tile(const char *pack_path, uint32_t node_index, GpuMathTileBlock *out_tile) {
    if (pack_path == nullptr || out_tile == nullptr || node_index >= k_gmpack_node_count) {
        return -1;
    }
    int fd = open(pack_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    // Read the node's RelPtr2D from the header or compute directly via RelPtr2D block algebra
    GpuMathQuadNode node = {0};
    off_t node_off = (off_t)(sizeof(GpuMathPageDirectory) + node_index * sizeof(GpuMathQuadNode));
    if (pread_exact(fd, &node, sizeof node, node_off) != 0) {
        close(fd);
        return -1;
    }
    uint64_t ssd_byte_offset = relptr2d_resolve_addr(0ull, node.rel_ssd_payload);
    if (ssd_byte_offset < k_gmpack_header_bytes ||
        ssd_byte_offset + k_gmpack_tile_bytes > k_gmpack_total_bytes) {
        close(fd);
        return -1;
    }
    int rc = pread_exact(fd, out_tile, sizeof(GpuMathTileBlock), (off_t)ssd_byte_offset);
    close(fd);
    return rc;
}

int backend_terrain_write_tile(const char *pack_path, uint32_t node_index, const GpuMathTileBlock *in_tile) {
    if (pack_path == nullptr || in_tile == nullptr || node_index >= k_gmpack_node_count) {
        return -1;
    }
    int fd = open(pack_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    GpuMathQuadNode node = {0};
    off_t node_off = (off_t)(sizeof(GpuMathPageDirectory) + node_index * sizeof(GpuMathQuadNode));
    if (pread_exact(fd, &node, sizeof node, node_off) != 0) {
        close(fd);
        return -1;
    }
    uint64_t ssd_byte_offset = relptr2d_resolve_addr(0ull, node.rel_ssd_payload);
    if (ssd_byte_offset < k_gmpack_header_bytes ||
        ssd_byte_offset + k_gmpack_tile_bytes > k_gmpack_total_bytes) {
        close(fd);
        return -1;
    }
    const uint8_t *src = (const uint8_t *)in_tile;
    size_t done = 0u;
    while (done < sizeof(GpuMathTileBlock)) {
        ssize_t n = pwrite(fd, src + done, sizeof(GpuMathTileBlock) - done, (off_t)(ssd_byte_offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

// ============================================================================
// .epack v1 GPU-Decoded Compressed Terrain Codec (100 KiB = 25 x 4 KiB / tile)
// ============================================================================

static int pwrite_exact_fd(int fd, const void *src, size_t bytes, off_t offset) {
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

static inline uint16_t clamp_u16_int(int v) {
    if (v <= 0) {
        return 0u;
    }
    if (v >= 65535) {
        return 65535u;
    }
    return (uint16_t)v;
}

static inline int16_t clamp_i16_int(int v) {
    if (v < -32768) {
        return (int16_t)-32768;
    }
    if (v > 32767) {
        return (int16_t)32767;
    }
    return (int16_t)v;
}

static inline uint16_t pack_rgb565_f(float r, float g, float b) {
    int r5 = (int)lrintf(fminf(fmaxf(r, 0.0f), 255.0f) * (31.0f / 255.0f));
    int g6 = (int)lrintf(fminf(fmaxf(g, 0.0f), 255.0f) * (63.0f / 255.0f));
    int b5 = (int)lrintf(fminf(fmaxf(b, 0.0f), 255.0f) * (31.0f / 255.0f));
    return (uint16_t)((uint32_t)r5 | ((uint32_t)g6 << 5u) | ((uint32_t)b5 << 11u));
}

static inline void unpack_rgb565_f(uint16_t c, float *out_r, float *out_g, float *out_b) {
    *out_r = (float)(c & 31u) * (255.0f / 31.0f);
    *out_g = (float)((c >> 5u) & 63u) * (255.0f / 63.0f);
    *out_b = (float)((c >> 11u) & 31u) * (255.0f / 31.0f);
}

static void encode_color_block_4x4(const uint32_t *rgba, uint32_t bx, uint32_t by, EpackColorBlock *out_blk) {
    float r[16], g[16], b[16];
    float mr = 0.0f, mg = 0.0f, mb = 0.0f;
    float min_r = 255.0f, min_g = 255.0f, min_b = 255.0f;
    float max_r = 0.0f, max_g = 0.0f, max_b = 0.0f;

    uint32_t k = 0u;
    for (uint32_t ly = 0u; ly < 4u; ++ly) {
        uint32_t sy = by * 4u + ly;
        for (uint32_t lx = 0u; lx < 4u; ++lx) {
            uint32_t p = rgba[sy * k_gmpack_tile_dim + bx * 4u + lx];
            float pr = (float)(p & 0xFFu);
            float pg = (float)((p >> 8u) & 0xFFu);
            float pb = (float)((p >> 16u) & 0xFFu);
            r[k] = pr;
            g[k] = pg;
            b[k] = pb;
            mr += pr;
            mg += pg;
            mb += pb;
            if (pr < min_r) min_r = pr;
            if (pg < min_g) min_g = pg;
            if (pb < min_b) min_b = pb;
            if (pr > max_r) max_r = pr;
            if (pg > max_g) max_g = pg;
            if (pb > max_b) max_b = pb;
            k++;
        }
    }
    mr *= (1.0f / 16.0f);
    mg *= (1.0f / 16.0f);
    mb *= (1.0f / 16.0f);

    float span2 = (max_r - min_r) * (max_r - min_r) +
                  (max_g - min_g) * (max_g - min_g) +
                  (max_b - min_b) * (max_b - min_b);
    if (span2 < 1.0f) {
        uint16_t flat = pack_rgb565_f(mr, mg, mb);
        *out_blk = (EpackColorBlock){
            .c0_rgb565 = flat,
            .c1_rgb565 = flat,
            .selectors = 0u,
        };
        return;
    }

    // 3x3 covariance + 4 power-iteration steps for chroma-preserving principal axis
    float cxx = 0.0f, cxy = 0.0f, cxz = 0.0f;
    float cyy = 0.0f, cyz = 0.0f, czz = 0.0f;
    for (uint32_t i = 0u; i < 16u; ++i) {
        float dr = r[i] - mr;
        float dg = g[i] - mg;
        float db = b[i] - mb;
        cxx += dr * dr;
        cxy += dr * dg;
        cxz += dr * db;
        cyy += dg * dg;
        cyz += dg * db;
        czz += db * db;
    }

    float vx = max_r - min_r;
    float vy = max_g - min_g;
    float vz = max_b - min_b;
    for (uint32_t it = 0u; it < 4u; ++it) {
        float nx = cxx * vx + cxy * vy + cxz * vz;
        float ny = cxy * vx + cyy * vy + cyz * vz;
        float nz = cxz * vx + cyz * vy + czz * vz;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 1.0e-5f) {
            float inv = 1.0f / len;
            vx = nx * inv;
            vy = ny * inv;
            vz = nz * inv;
        }
    }

    float t_min =  1.0e9f;
    float t_max = -1.0e9f;
    for (uint32_t i = 0u; i < 16u; ++i) {
        float t = (r[i] - mr) * vx + (g[i] - mg) * vy + (b[i] - mb) * vz;
        if (t < t_min) t_min = t;
        if (t > t_max) t_max = t;
    }

    float c0r = mr + vx * t_min, c0g = mg + vy * t_min, c0b = mb + vz * t_min;
    float c1r = mr + vx * t_max, c1g = mg + vy * t_max, c1b = mb + vz * t_max;

    // 1-pass least-squares endpoint refinement over 2-bit weights {0, 1/3, 2/3, 1}
    float a00 = 0.0f, a01 = 0.0f, a11 = 0.0f;
    float b0r = 0.0f, b0g = 0.0f, b0b = 0.0f;
    float b1r = 0.0f, b1g = 0.0f, b1b = 0.0f;
    float d_r = c1r - c0r, d_g = c1g - c0g, d_b = c1b - c0b;
    float d_len2 = fmaxf(d_r * d_r + d_g * d_g + d_b * d_b, 1.0e-4f);
    for (uint32_t i = 0u; i < 16u; ++i) {
        float u = ((r[i] - c0r) * d_r + (g[i] - c0g) * d_g + (b[i] - c0b) * d_b) / d_len2;
        int q = (int)lrintf(fminf(fmaxf(u, 0.0f), 1.0f) * 3.0f);
        float w1 = (float)q * (1.0f / 3.0f);
        float w0 = 1.0f - w1;
        a00 += w0 * w0;
        a01 += w0 * w1;
        a11 += w1 * w1;
        b0r += w0 * r[i]; b0g += w0 * g[i]; b0b += w0 * b[i];
        b1r += w1 * r[i]; b1g += w1 * g[i]; b1b += w1 * b[i];
    }
    float det = a00 * a11 - a01 * a01;
    if (det > 1.0e-3f) {
        float inv_det = 1.0f / det;
        c0r = (a11 * b0r - a01 * b1r) * inv_det;
        c0g = (a11 * b0g - a01 * b1g) * inv_det;
        c0b = (a11 * b0b - a01 * b1b) * inv_det;
        c1r = (a00 * b1r - a01 * b0r) * inv_det;
        c1g = (a00 * b1g - a01 * b0g) * inv_det;
        c1b = (a00 * b1b - a01 * b0b) * inv_det;
    }

    uint16_t p0 = pack_rgb565_f(c0r, c0g, c0b);
    uint16_t p1 = pack_rgb565_f(c1r, c1g, c1b);
    float q0r, q0g, q0b, q1r, q1g, q1b;
    unpack_rgb565_f(p0, &q0r, &q0g, &q0b);
    unpack_rgb565_f(p1, &q1r, &q1g, &q1b);

    float pal_r[4], pal_g[4], pal_b[4];
    for (uint32_t s = 0u; s < 4u; ++s) {
        float w = (float)s * (1.0f / 3.0f);
        pal_r[s] = q0r + (q1r - q0r) * w;
        pal_g[s] = q0g + (q1g - q0g) * w;
        pal_b[s] = q0b + (q1b - q0b) * w;
    }

    uint32_t selectors = 0u;
    for (uint32_t i = 0u; i < 16u; ++i) {
        uint32_t best_s = 0u;
        float best_err = 1.0e30f;
        for (uint32_t s = 0u; s < 4u; ++s) {
            float er = r[i] - pal_r[s];
            float eg = (g[i] - pal_g[s]) * 1.25f; // Perceptual green weight
            float eb = b[i] - pal_b[s];
            float err = er * er + eg * eg + eb * eb;
            if (err < best_err) {
                best_err = err;
                best_s = s;
            }
        }
        selectors |= (best_s << (i * 2u));
    }

    *out_blk = (EpackColorBlock){
        .c0_rgb565 = p0,
        .c1_rgb565 = p1,
        .selectors = selectors,
    };
}

void backend_epack_encode_tile(const GpuMathTileBlock *src, EpackTileBlock *dst) {
    if (src == nullptr || dst == nullptr) {
        return;
    }

    // 1. Lossless perimeter seam ring (0.0000 m shared border height + exact RGB565 border color)
    for (uint32_t i = 0u; i < k_gmpack_tile_dim; ++i) {
        dst->border_height_u16[0][i] = src->height_u16[0u * k_gmpack_tile_dim + i];
        dst->border_height_u16[1][i] = src->height_u16[(k_gmpack_tile_dim - 1u) * k_gmpack_tile_dim + i];
        dst->border_height_u16[2][i] = src->height_u16[i * k_gmpack_tile_dim + 0u];
        dst->border_height_u16[3][i] = src->height_u16[i * k_gmpack_tile_dim + (k_gmpack_tile_dim - 1u)];

        uint32_t c_y0 = src->imagery_rgba8[0u * k_gmpack_tile_dim + i];
        uint32_t c_y1 = src->imagery_rgba8[(k_gmpack_tile_dim - 1u) * k_gmpack_tile_dim + i];
        uint32_t c_x0 = src->imagery_rgba8[i * k_gmpack_tile_dim + 0u];
        uint32_t c_x1 = src->imagery_rgba8[i * k_gmpack_tile_dim + (k_gmpack_tile_dim - 1u)];

        dst->border_rgb565[0][i] = pack_rgb565_f((float)(c_y0 & 0xFFu), (float)((c_y0 >> 8u) & 0xFFu), (float)((c_y0 >> 16u) & 0xFFu));
        dst->border_rgb565[1][i] = pack_rgb565_f((float)(c_y1 & 0xFFu), (float)((c_y1 >> 8u) & 0xFFu), (float)((c_y1 >> 16u) & 0xFFu));
        dst->border_rgb565[2][i] = pack_rgb565_f((float)(c_x0 & 0xFFu), (float)((c_x0 >> 8u) & 0xFFu), (float)((c_x0 >> 16u) & 0xFFu));
        dst->border_rgb565[3][i] = pack_rgb565_f((float)(c_x1 & 0xFFu), (float)((c_x1 >> 8u) & 0xFFu), (float)((c_x1 >> 16u) & 0xFFu));
    }

    // 2. 32x32 planar-residual 8x8 elevation patches (48 B each = 48 KiB)
    for (uint32_t py = 0u; py < k_epack_patch_grid; ++py) {
        for (uint32_t px = 0u; px < k_epack_patch_grid; ++px) {
            float h[8][8];
            float sum_xz = 0.0f;
            float sum_yz = 0.0f;
            for (uint32_t ly = 0u; ly < 8u; ++ly) {
                uint32_t sy = py * 8u + ly;
                float cy = (float)ly - 3.5f;
                for (uint32_t lx = 0u; lx < 8u; ++lx) {
                    uint32_t sx = px * 8u + lx;
                    float z = (float)src->height_u16[sy * k_gmpack_tile_dim + sx] * 0.0625f;
                    h[ly][lx] = z;
                    sum_xz += ((float)lx - 3.5f) * z;
                    sum_yz += cy * z;
                }
            }

            // Closed-form orthogonal OLS plane slopes: sum_{x,y}(x - 3.5)^2 = 8 * 42 = 336
            float sx_raw = fminf(fmaxf(sum_xz * (1.0f / 336.0f), -120.0f), 120.0f);
            float sy_raw = fminf(fmaxf(sum_yz * (1.0f / 336.0f), -120.0f), 120.0f);
            int16_t slope_x_i16 = clamp_i16_int((int)lrintf(sx_raw * 256.0f));
            int16_t slope_y_i16 = clamp_i16_int((int)lrintf(sy_raw * 256.0f));
            float sx_q = (float)slope_x_i16 * (1.0f / 256.0f);
            float sy_q = (float)slope_y_i16 * (1.0f / 256.0f);

            float d[8][8];
            float d_min =  1.0e30f;
            float d_max = -1.0e30f;
            for (uint32_t ly = 0u; ly < 8u; ++ly) {
                for (uint32_t lx = 0u; lx < 8u; ++lx) {
                    float dv = h[ly][lx] - (sx_q * (float)lx + sy_q * (float)ly);
                    d[ly][lx] = dv;
                    if (dv < d_min) d_min = dv;
                    if (dv > d_max) d_max = dv;
                }
            }

            uint16_t base_u16 = clamp_u16_int((int)floorf((d_min + 2048.0f) * 8.0f));
            float base_q = (float)base_u16 * 0.125f - 2048.0f;
            float r_max = fmaxf(d_max - base_q, 0.0f);
            uint16_t step_u16 = clamp_u16_int((int)ceilf((r_max / 31.0f) * 1024.0f));
            float step_q = (float)step_u16 * (1.0f / 1024.0f);

            EpackElevPatch patch = {
                .base_u16    = base_u16,
                .slope_x_i16 = slope_x_i16,
                .slope_y_i16 = slope_y_i16,
                .step_u16    = step_u16,
            };

            if (step_q > 0.0f) {
                float inv_step = 1.0f / step_q;
                for (uint32_t ly = 0u; ly < 8u; ++ly) {
                    uint32_t row_lo4 = 0u;
                    for (uint32_t lx = 0u; lx < 8u; ++lx) {
                        int q = (int)lrintf((d[ly][lx] - base_q) * inv_step);
                        uint32_t q5 = (uint32_t)(q < 0 ? 0 : (q > 31 ? 31 : q));
                        row_lo4 |= ((q5 & 0xFu) << (lx * 4u));
                        uint32_t bit = ly * 8u + lx;
                        patch.hi1[bit >> 5u] |= (((q5 >> 4u) & 1u) << (bit & 31u));
                    }
                    patch.lo4[ly] = row_lo4;
                }
            }

            dst->elev_patches[py * k_epack_patch_grid + px] = patch;
        }
    }

    // 3. 64x64 chroma-preserving BC1 color blocks (8 B each = 32 KiB)
    for (uint32_t by = 0u; by < k_epack_color_grid; ++by) {
        for (uint32_t bx = 0u; bx < k_epack_color_grid; ++bx) {
            encode_color_block_4x4(src->imagery_rgba8, bx, by, &dst->color_blocks[by * k_epack_color_grid + bx]);
        }
    }

    // 4. 128x128 8-bit horizon AO grid (16 KiB, initialized from 2x2 average of imagery alpha)
    for (uint32_t ay = 0u; ay < k_epack_ao_dim; ++ay) {
        uint32_t sy0 = ay * 2u;
        for (uint32_t ax = 0u; ax < k_epack_ao_dim; ++ax) {
            uint32_t sx0 = ax * 2u;
            uint32_t a00 = (src->imagery_rgba8[sy0 * k_gmpack_tile_dim + sx0] >> 24u) & 0xFFu;
            uint32_t a10 = (src->imagery_rgba8[sy0 * k_gmpack_tile_dim + sx0 + 1u] >> 24u) & 0xFFu;
            uint32_t a01 = (src->imagery_rgba8[(sy0 + 1u) * k_gmpack_tile_dim + sx0] >> 24u) & 0xFFu;
            uint32_t a11 = (src->imagery_rgba8[(sy0 + 1u) * k_gmpack_tile_dim + sx0 + 1u] >> 24u) & 0xFFu;
            uint32_t avg = (a00 + a10 + a01 + a11 + 2u) >> 2u;
            dst->ao_grid[ay * k_epack_ao_dim + ax] = (uint8_t)(avg == 0u ? 214u : avg);
        }
    }
}

float backend_epack_load_height_m(const EpackTileBlock *tile, uint32_t sx, uint32_t sy) {
    if (tile == nullptr) {
        return 0.0f;
    }
    uint32_t cx = sx < k_gmpack_tile_dim ? sx : (k_gmpack_tile_dim - 1u);
    uint32_t cy = sy < k_gmpack_tile_dim ? sy : (k_gmpack_tile_dim - 1u);

    if (cy == 0u) {
        return (float)tile->border_height_u16[0][cx] * 0.0625f;
    }
    if (cy == k_gmpack_tile_dim - 1u) {
        return (float)tile->border_height_u16[1][cx] * 0.0625f;
    }
    if (cx == 0u) {
        return (float)tile->border_height_u16[2][cy] * 0.0625f;
    }
    if (cx == k_gmpack_tile_dim - 1u) {
        return (float)tile->border_height_u16[3][cy] * 0.0625f;
    }

    uint32_t px  = cx >> 3u;
    uint32_t py  = cy >> 3u;
    uint32_t lx  = cx & 7u;
    uint32_t ly  = cy & 7u;
    uint32_t bit = ly * 8u + lx;
    const EpackElevPatch *p = &tile->elev_patches[py * k_epack_patch_grid + px];

    uint32_t lo4 = (p->lo4[ly] >> (lx * 4u)) & 0xFu;
    uint32_t hi1 = (p->hi1[bit >> 5u] >> (bit & 31u)) & 1u;
    uint32_t q5  = lo4 | (hi1 << 4u);

    float base_m = (float)p->base_u16 * 0.125f - 2048.0f;
    float sx_m   = (float)p->slope_x_i16 * (1.0f / 256.0f);
    float sy_m   = (float)p->slope_y_i16 * (1.0f / 256.0f);
    float step_m = (float)p->step_u16 * (1.0f / 1024.0f);
    return base_m + sx_m * (float)lx + sy_m * (float)ly + step_m * (float)q5;
}

uint32_t backend_epack_load_rgba8(const EpackTileBlock *tile, uint32_t sx, uint32_t sy) {
    if (tile == nullptr) {
        return 0xFF000000u;
    }
    uint32_t cx = sx < k_gmpack_tile_dim ? sx : (k_gmpack_tile_dim - 1u);
    uint32_t cy = sy < k_gmpack_tile_dim ? sy : (k_gmpack_tile_dim - 1u);

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (cy == 0u) {
        unpack_rgb565_f(tile->border_rgb565[0][cx], &r, &g, &b);
    } else if (cy == k_gmpack_tile_dim - 1u) {
        unpack_rgb565_f(tile->border_rgb565[1][cx], &r, &g, &b);
    } else if (cx == 0u) {
        unpack_rgb565_f(tile->border_rgb565[2][cy], &r, &g, &b);
    } else if (cx == k_gmpack_tile_dim - 1u) {
        unpack_rgb565_f(tile->border_rgb565[3][cy], &r, &g, &b);
    } else {
        uint32_t bx = cx >> 2u;
        uint32_t by = cy >> 2u;
        uint32_t lx = cx & 3u;
        uint32_t ly = cy & 3u;
        const EpackColorBlock *cb = &tile->color_blocks[by * k_epack_color_grid + bx];
        float c0r, c0g, c0b, c1r, c1g, c1b;
        unpack_rgb565_f(cb->c0_rgb565, &c0r, &c0g, &c0b);
        unpack_rgb565_f(cb->c1_rgb565, &c1r, &c1g, &c1b);
        uint32_t sel = (cb->selectors >> ((ly * 4u + lx) * 2u)) & 3u;
        float w = (float)sel * (1.0f / 3.0f);
        r = c0r + (c1r - c0r) * w;
        g = c0g + (c1g - c0g) * w;
        b = c0b + (c1b - c0b) * w;
    }

    uint32_t ax = cx >> 1u;
    uint32_t ay = cy >> 1u;
    uint32_t ao = tile->ao_grid[ay * k_epack_ao_dim + ax];
    uint32_t rb = (uint32_t)lrintf(fminf(fmaxf(r, 0.0f), 255.0f));
    uint32_t gb = (uint32_t)lrintf(fminf(fmaxf(g, 0.0f), 255.0f));
    uint32_t bb = (uint32_t)lrintf(fminf(fmaxf(b, 0.0f), 255.0f));
    return rb | (gb << 8u) | (bb << 16u) | (ao << 24u);
}

void backend_epack_decode_tile(const EpackTileBlock *src, GpuMathTileBlock *dst) {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    for (uint32_t sy = 0u; sy < k_gmpack_tile_dim; ++sy) {
        for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
            float hm = backend_epack_load_height_m(src, sx, sy);
            dst->height_u16[sy * k_gmpack_tile_dim + sx] =
                clamp_u16_int((int)lrintf(hm * 16.0f));
            dst->imagery_rgba8[sy * k_gmpack_tile_dim + sx] =
                backend_epack_load_rgba8(src, sx, sy);
        }
    }
}

int backend_terrain_ensure_epack(const char *epack_path, const char *gmpack_path) {
    if (epack_path == nullptr || epack_path[0] == '\0' ||
        gmpack_path == nullptr || gmpack_path[0] == '\0') {
        return -1;
    }
    if (backend_terrain_ensure_pack(gmpack_path) != 0) {
        return -1;
    }

    struct stat st_gm = {0};
    if (stat(gmpack_path, &st_gm) != 0) {
        return -1;
    }

    struct stat st_ep = {0};
    if (stat(epack_path, &st_ep) == 0 && (size_t)st_ep.st_size == k_epack_total_bytes) {
        bool newer_or_equal =
            (st_ep.st_mtim.tv_sec > st_gm.st_mtim.tv_sec) ||
            (st_ep.st_mtim.tv_sec == st_gm.st_mtim.tv_sec &&
             st_ep.st_mtim.tv_nsec >= st_gm.st_mtim.tv_nsec);
        if (newer_or_equal) {
            int fd = open(epack_path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                uint32_t magic = 0u;
                uint32_t ver   = 0u;
                if (pread_exact(fd, &magic, sizeof magic, 0) == 0 &&
                    pread_exact(fd, &ver, sizeof ver, 4) == 0 &&
                    magic == k_epack_magic && ver == k_epack_version) {
                    close(fd);
                    return 0;
                }
                close(fd);
            }
        }
    }

    GpuMathArchiveHeader *hdr = aligned_alloc(4096u, sizeof(GpuMathArchiveHeader));
    GpuMathTileBlock     *raw = aligned_alloc(4096u, sizeof(GpuMathTileBlock));
    EpackTileBlock       *cmp = aligned_alloc(4096u, sizeof(EpackTileBlock));
    if (hdr == nullptr || raw == nullptr || cmp == nullptr) {
        free(hdr);
        free(raw);
        free(cmp);
        return -1;
    }

    if (backend_terrain_read_header(gmpack_path, hdr) != 0) {
        free(hdr);
        free(raw);
        free(cmp);
        return -1;
    }

    hdr->dir.magic   = k_epack_magic;
    hdr->dir.version = k_epack_version;
    for (uint32_t idx = 0u; idx < k_gmpack_node_count; ++idx) {
        GpuMathQuadNode *node = &hdr->nodes[idx];
        int32_t ssd_blk = (int32_t)(k_gmpack_header_blocks + idx * k_epack_tile_blocks_4k);
        node->rel_ssd_payload = relptr2d_pack(ssd_blk, (int16_t)node->grid_x, (int16_t)node->grid_y, node->lod, 1u);
        uint32_t old_blk = 0u;
        uint16_t morton  = 0u;
        uint8_t  lod     = 0u;
        uint8_t  flags   = 0u;
        float    min_e   = 0.0f;
        float    max_e   = 0.0f;
        gpumath_tile_header_unpack(node->header, &old_blk, &morton, &lod, &flags, &min_e, &max_e);
        node->header = gpumath_tile_header_pack((uint32_t)ssd_blk, morton, lod, flags, min_e, max_e);
    }

    char tmp_path[560];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp.%d", epack_path, (int)getpid());
    int out_fd = open(tmp_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (out_fd < 0) {
        free(hdr);
        free(raw);
        free(cmp);
        return -1;
    }

    int rc = pwrite_exact_fd(out_fd, hdr, sizeof(GpuMathArchiveHeader), 0);
    for (uint32_t idx = 0u; rc == 0 && idx < k_gmpack_node_count; ++idx) {
        if (backend_terrain_read_tile(gmpack_path, idx, raw) != 0) {
            rc = -1;
            break;
        }
        backend_epack_encode_tile(raw, cmp);
        off_t dst_off = (off_t)(k_gmpack_header_bytes + (size_t)idx * k_epack_tile_bytes);
        rc = pwrite_exact_fd(out_fd, cmp, sizeof(EpackTileBlock), dst_off);
    }

    close(out_fd);
    free(hdr);
    free(raw);
    free(cmp);

    if (rc != 0) {
        (void)unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, epack_path) != 0) {
        (void)unlink(tmp_path);
        return -1;
    }

    printf("[Backend] Synthesized compressed .epack v1 '%s' (%.2f MiB, 3.84x smaller than %.2f MiB .gmpack)\n",
           epack_path,
           (double)k_epack_total_bytes / (1024.0 * 1024.0),
           (double)k_gmpack_total_bytes / (1024.0 * 1024.0));
    return 0;
}

int backend_terrain_read_epack_header(const char *epack_path,
                                      const char *gmpack_path,
                                      GpuMathArchiveHeader *out_hdr) {
    if (epack_path == nullptr || gmpack_path == nullptr || out_hdr == nullptr) {
        return -1;
    }
    if (backend_terrain_ensure_epack(epack_path, gmpack_path) != 0) {
        return -1;
    }
    int fd = open(epack_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    int rc = pread_exact(fd, out_hdr, sizeof(GpuMathArchiveHeader), 0);
    close(fd);
    if (rc != 0) {
        return -1;
    }
    if (out_hdr->dir.magic != k_epack_magic ||
        out_hdr->dir.version != k_epack_version ||
        out_hdr->dir.node_count != k_gmpack_node_count) {
        return -1;
    }
    return 0;
}

int backend_terrain_read_epack_tile(const char *epack_path, uint32_t node_index, EpackTileBlock *out_tile) {
    if (epack_path == nullptr || out_tile == nullptr || node_index >= k_gmpack_node_count) {
        return -1;
    }
    int fd = open(epack_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    GpuMathQuadNode node = {0};
    off_t node_off = (off_t)(sizeof(GpuMathPageDirectory) + node_index * sizeof(GpuMathQuadNode));
    if (pread_exact(fd, &node, sizeof node, node_off) != 0) {
        close(fd);
        return -1;
    }
    uint64_t ssd_byte_offset = relptr2d_resolve_addr(0ull, node.rel_ssd_payload);
    if (ssd_byte_offset < k_gmpack_header_bytes ||
        ssd_byte_offset + k_epack_tile_bytes > k_epack_total_bytes) {
        close(fd);
        return -1;
    }
    int rc = pread_exact(fd, out_tile, sizeof(EpackTileBlock), (off_t)ssd_byte_offset);
    close(fd);
    return rc;
}

int backend_terrain_write_epack_tile(const char *epack_path, uint32_t node_index, const EpackTileBlock *in_tile) {
    if (epack_path == nullptr || in_tile == nullptr || node_index >= k_gmpack_node_count) {
        return -1;
    }
    int fd = open(epack_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    GpuMathQuadNode node = {0};
    off_t node_off = (off_t)(sizeof(GpuMathPageDirectory) + node_index * sizeof(GpuMathQuadNode));
    if (pread_exact(fd, &node, sizeof node, node_off) != 0) {
        close(fd);
        return -1;
    }
    uint64_t ssd_byte_offset = relptr2d_resolve_addr(0ull, node.rel_ssd_payload);
    if (ssd_byte_offset < k_gmpack_header_bytes ||
        ssd_byte_offset + k_epack_tile_bytes > k_epack_total_bytes) {
        close(fd);
        return -1;
    }
    int rc = pwrite_exact_fd(fd, in_tile, sizeof(EpackTileBlock), (off_t)ssd_byte_offset);
    close(fd);
    return rc;
}
