#include "backend/internal.h"
#include "system/system.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// GPU-Math & Terrain Streaming Backend Lifecycle & Ray-DEM Solver (lifecycle.c)
// ============================================================================

BackendState g_backend;
static char  s_custom_overlay_path[1024];

static const char *k_default_pack_rel    = "examples/demo/maps/baklan_besparmak.gmpack";
static const char *k_default_survey_rel  = "examples/demo/maps/besparmak_survey.manifest";
static const char *k_default_overlay_rel = "examples/demo/maps/overlay.png";

static void resolve_pack_path(char *out, size_t cap) {
    int wrote = snprintf(out, cap, "%s", k_default_pack_rel);
    if (wrote > 0 && (size_t)wrote < cap && access(out, R_OK) == 0) {
        return;
    }
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n > 0 && (size_t)n < sizeof exe) {
        exe[n] = '\0';
        for (uint32_t up = 0u; up < 6u; ++up) {
            char *slash = strrchr(exe, '/');
            if (slash == nullptr || slash == exe) {
                break;
            }
            *slash = '\0';
            int w = snprintf(out, cap, "%s/%s", exe, k_default_pack_rel);
            if (w > 0 && (size_t)w < cap && access(out, R_OK) == 0) {
                return;
            }
        }
    }
    snprintf(out, cap, "%s", k_default_pack_rel);
}

static void resolve_survey_manifest_path(const char *pack_path, char *out, size_t cap) {
    if (pack_path != nullptr && pack_path[0] != '\0') {
        const char *slash = strrchr(pack_path, '/');
        if (slash != nullptr) {
            size_t dir_len = (size_t)(slash - pack_path);
            if (dir_len + 28u < cap) {
                memcpy(out, pack_path, dir_len);
                snprintf(out + dir_len, cap - dir_len, "/besparmak_survey.manifest");
                return;
            }
        }
    }
    snprintf(out, cap, "%s", k_default_survey_rel);
}

static void resolve_overlay_image_path(const char *pack_path, char *out, size_t cap) {
    if (s_custom_overlay_path[0] != '\0') {
        snprintf(out, cap, "%s", s_custom_overlay_path);
        return;
    }
    if (pack_path != nullptr && pack_path[0] != '\0') {
        const char *slash = strrchr(pack_path, '/');
        if (slash != nullptr) {
            size_t dir_len = (size_t)(slash - pack_path);
            if (dir_len + 20u < cap) {
                static const char *const k_candidates[] = {
                    "/overlay.png",
                    "/overlay.jpg",
                    "/overlay.jpeg",
                };
                for (size_t i = 0u; i < 3u; ++i) {
                    memcpy(out, pack_path, dir_len);
                    snprintf(out + dir_len, cap - dir_len, "%s", k_candidates[i]);
                    if (access(out, R_OK) == 0) {
                        return;
                    }
                }
                memcpy(out, pack_path, dir_len);
                snprintf(out + dir_len, cap - dir_len, "/overlay.png");
                return;
            }
        }
    }
    snprintf(out, cap, "%s", k_default_overlay_rel);
}

static bool overlay_paths_match(const char *a, const char *b) {
    if (a == nullptr || b == nullptr || a[0] == '\0' || b[0] == '\0') {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }
    const char *ba = strrchr(a, '/');
    const char *bb = strrchr(b, '/');
    ba = (ba != nullptr) ? (ba + 1) : a;
    bb = (bb != nullptr) ? (bb + 1) : b;
    return strcmp(ba, bb) == 0;
}

static void sync_committed_ortho_layers_to_bda(void) {
    if (g_backend.gpu_hdr == nullptr || g_backend.overlay_gpu_pixels == nullptr ||
        g_backend.overlay_buf.address == 0ull) {
        return;
    }
    memset(&g_backend.gpu_hdr->ortho_layers, 0, sizeof g_backend.gpu_hdr->ortho_layers);
    g_backend.gpu_hdr->ortho_layer_count = 0u;

    size_t   arena_off_bytes = (size_t)k_backend_overlay_live_bytes;
    size_t   arena_cap_bytes = (size_t)k_backend_overlay_bda_bytes;
    uint8_t *base_ptr        = (uint8_t *)g_backend.overlay_gpu_pixels;
    uint64_t base_bda        = g_backend.overlay_buf.address;

    uint32_t out_cnt = 0u;
    for (uint32_t i = 0u;
         i < g_backend.survey.ortho_overlay_count && out_cnt < k_gmpack_max_ortho_layers;
         ++i) {
        const TerrainOrthoOverlay *ov = &g_backend.survey.ortho_overlays[i];
        if (ov->rgba_pixels == nullptr || ov->width_px == 0u || ov->height_px == 0u) {
            continue;
        }
        uint32_t dst_w = ov->width_px > k_backend_overlay_max_dim ? k_backend_overlay_max_dim : ov->width_px;
        uint32_t dst_h = ov->height_px > k_backend_overlay_max_dim ? k_backend_overlay_max_dim : ov->height_px;
        size_t   bytes = (size_t)dst_w * (size_t)dst_h * sizeof(uint32_t);
        size_t   aligned_off = (arena_off_bytes + 63u) & ~(size_t)63u;
        if (aligned_off + bytes > arena_cap_bytes) {
            break;
        }
        uint32_t *dst_px = (uint32_t *)(base_ptr + aligned_off);
        if (dst_w == ov->width_px && dst_h == ov->height_px) {
            memcpy(dst_px, ov->rgba_pixels, bytes);
        } else {
            for (uint32_t y = 0u; y < dst_h; ++y) {
                uint32_t sy = (uint32_t)(((uint64_t)y * ov->height_px) / dst_h);
                for (uint32_t x = 0u; x < dst_w; ++x) {
                    uint32_t sx = (uint32_t)(((uint64_t)x * ov->width_px) / dst_w);
                    dst_px[y * dst_w + x] = ov->rgba_pixels[sy * ov->width_px + sx];
                }
            }
        }
        // If the live alignment preview is actively shown for this exact image, suppress the
        // committed layer temporarily so WASD movement and O/P transparency show cleanly against
        // the underlying map without a frozen duplicate copy underneath.
        bool is_being_edited = g_backend.overlay_active && !g_backend.overlay_preview_hidden &&
                               overlay_paths_match(ov->image_path, g_backend.overlay_image_path);
        g_backend.gpu_hdr->ortho_layers[out_cnt] = (GpuMathOrthoOverlay){
            .pixels_bda    = base_bda + (uint64_t)aligned_off,
            .width_px      = dst_w,
            .height_px     = dst_h,
            .center_x      = ov->center_x,
            .center_y      = ov->center_y,
            .half_span_x   = ov->half_span_x,
            .half_span_y   = ov->half_span_y,
            .opacity       = fminf(fmaxf(ov->blend_weight, 0.0f), 1.0f),
            .active        = is_being_edited ? 0u : 1u,
            .scale_pct_x   = 100.0f,
            .scale_pct_y   = 100.0f,
            .committed_cnt = out_cnt + 1u,
        };
        arena_off_bytes = aligned_off + bytes;
        out_cnt++;
    }
    g_backend.gpu_hdr->ortho_layer_count = out_cnt;
}

static void sync_gpu_overlay_header(void) {
    if (g_backend.gpu_hdr == nullptr) {
        return;
    }
    if (!g_backend.overlay_active || g_backend.overlay_w == 0u || g_backend.overlay_h == 0u ||
        g_backend.overlay_buf.address == 0ull) {
        memset(&g_backend.gpu_hdr->overlay, 0, sizeof g_backend.gpu_hdr->overlay);
        return;
    }
    float hx = g_backend.overlay_base_half_x * g_backend.overlay_scale_x;
    float hy = g_backend.overlay_base_half_y * g_backend.overlay_scale_y;
    g_backend.gpu_hdr->overlay = (GpuMathOrthoOverlay){
        .pixels_bda    = g_backend.overlay_buf.address,
        .width_px      = g_backend.overlay_w,
        .height_px     = g_backend.overlay_h,
        .center_x      = g_backend.overlay_center_x,
        .center_y      = g_backend.overlay_center_y,
        .half_span_x   = hx,
        .half_span_y   = hy,
        .opacity       = g_backend.overlay_opacity,
        .active        = g_backend.overlay_preview_hidden ? 0u : 1u,
        .scale_pct_x   = g_backend.overlay_scale_x * 100.0f,
        .scale_pct_y   = g_backend.overlay_scale_y * 100.0f,
        .committed_cnt = g_backend.overlay_committed_cnt,
    };
}

static uint32_t sample_tile_rgba_bilinear_raw(const GpuMathTileBlock *blk, float u, float v) {
    float tx = fminf(fmaxf(u, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u);
    float ty = fminf(fmaxf(v, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u);
    uint32_t x0 = (uint32_t)floorf(tx);
    uint32_t y0 = (uint32_t)floorf(ty);
    uint32_t x1 = x0 + 1u < k_gmpack_tile_dim ? x0 + 1u : k_gmpack_tile_dim - 1u;
    uint32_t y1 = y0 + 1u < k_gmpack_tile_dim ? y0 + 1u : k_gmpack_tile_dim - 1u;
    float fx = tx - (float)x0;
    float fy = ty - (float)y0;

    uint32_t c00 = blk->imagery_rgba8[y0 * k_gmpack_tile_dim + x0];
    uint32_t c10 = blk->imagery_rgba8[y0 * k_gmpack_tile_dim + x1];
    uint32_t c01 = blk->imagery_rgba8[y1 * k_gmpack_tile_dim + x0];
    uint32_t c11 = blk->imagery_rgba8[y1 * k_gmpack_tile_dim + x1];

    uint32_t out_rgba = 0xFF000000u;
    for (uint32_t ch = 0u; ch < 3u; ++ch) {
        uint32_t shift = ch * 8u;
        float v00 = (float)((c00 >> shift) & 0xFFu);
        float v10 = (float)((c10 >> shift) & 0xFFu);
        float v01 = (float)((c01 >> shift) & 0xFFu);
        float v11 = (float)((c11 >> shift) & 0xFFu);
        float vx0 = v00 + (v10 - v00) * fx;
        float vx1 = v01 + (v11 - v01) * fx;
        float val = vx0 + (vx1 - vx0) * fy;
        uint32_t byte_v = (uint32_t)fminf(fmaxf(lrintf(val), 0L), 255L);
        out_rgba |= (byte_v << shift);
    }
    return out_rgba;
}

static void ensure_default_overlay_image(const char *img_path) {
    if (img_path == nullptr || img_path[0] == '\0' || access(img_path, R_OK) == 0) {
        return;
    }
    if (g_backend.gpu_hdr == nullptr || g_backend.pack_path[0] == '\0') {
        return;
    }
    constexpr uint32_t k_dim = 512u;
    constexpr float    k_cx  = 1595.0f;
    constexpr float    k_cy  = -650.0f;
    constexpr float    k_hx  = 650.0f;
    constexpr float    k_hy  = 650.0f;

    uint32_t *base_px = (uint32_t *)malloc((size_t)k_dim * (size_t)k_dim * sizeof(uint32_t));
    uint32_t *out_px  = (uint32_t *)malloc((size_t)k_dim * (size_t)k_dim * sizeof(uint32_t));
    if (base_px == nullptr || out_px == nullptr) {
        free(base_px);
        free(out_px);
        return;
    }

    // Load up to 4 overlapping LOD-4 tiles covering [k_cx - k_hx, k_cx + k_hx] x [k_cy - k_hy, k_cy + k_hy]
    uint32_t          leaf_ids[4]   = {0u, 0u, 0u, 0u};
    GpuMathTileBlock *leaf_tiles[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t          leaf_cnt      = 0u;
    for (uint32_t idx = 85u; idx < k_gmpack_node_count && leaf_cnt < 4u; ++idx) {
        const GpuMathQuadNode *n = &g_backend.gpu_hdr->nodes[idx];
        if (n->world_max_x < k_cx - k_hx || n->world_min_x > k_cx + k_hx ||
            n->world_max_y < k_cy - k_hy || n->world_min_y > k_cy + k_hy) {
            continue;
        }
        GpuMathTileBlock *blk = (GpuMathTileBlock *)malloc(sizeof(GpuMathTileBlock));
        if (blk != nullptr && backend_terrain_read_tile(g_backend.pack_path, idx, blk) == 0) {
            leaf_ids[leaf_cnt]   = idx;
            leaf_tiles[leaf_cnt] = blk;
            leaf_cnt++;
        } else {
            free(blk);
        }
    }

    for (uint32_t py = 0u; py < k_dim; ++py) {
        float v_img = ((float)py + 0.5f) / (float)k_dim;
        float wy    = k_cy + (0.5f - v_img) * (2.0f * k_hy);
        for (uint32_t px = 0u; px < k_dim; ++px) {
            float u_img = ((float)px + 0.5f) / (float)k_dim;
            float wx    = k_cx + (u_img - 0.5f) * (2.0f * k_hx);

            const GpuMathQuadNode  *node = &g_backend.gpu_hdr->nodes[0];
            const GpuMathTileBlock *blk  = &g_backend.gpu_tiles[0];
            for (uint32_t k = 0u; k < leaf_cnt; ++k) {
                const GpuMathQuadNode *cand = &g_backend.gpu_hdr->nodes[leaf_ids[k]];
                if (wx >= cand->world_min_x && wx <= cand->world_max_x &&
                    wy >= cand->world_min_y && wy <= cand->world_max_y) {
                    node = cand;
                    blk  = leaf_tiles[k];
                    break;
                }
            }
            float u_tile = (wx - node->world_min_x) / fmaxf(node->world_max_x - node->world_min_x, 1.0f);
            float v_tile = (wy - node->world_min_y) / fmaxf(node->world_max_y - node->world_min_y, 1.0f);
            base_px[py * k_dim + px] = sample_tile_rgba_bilinear_raw(blk, u_tile, v_tile);
        }
    }

    // Apply unsharp mask + sub-meter aerial micro-detail enhancement so default overlay.png is crisp HD
    for (uint32_t py = 0u; py < k_dim; ++py) {
        uint32_t ym = py > 0u ? py - 1u : 0u;
        uint32_t yp = py + 1u < k_dim ? py + 1u : k_dim - 1u;
        for (uint32_t px = 0u; px < k_dim; ++px) {
            uint32_t xm = px > 0u ? px - 1u : 0u;
            uint32_t xp = px + 1u < k_dim ? px + 1u : k_dim - 1u;
            uint32_t cc = base_px[py * k_dim + px];
            uint32_t cl = base_px[py * k_dim + xm];
            uint32_t cr = base_px[py * k_dim + xp];
            uint32_t cd = base_px[ym * k_dim + px];
            uint32_t cu = base_px[yp * k_dim + px];

            uint32_t h = (px * 374761393u + py * 668265263u) ^ 0x9E3779B9u;
            h = (h ^ (h >> 13u)) * 1274126177u;
            float grain = ((float)(h & 0xFFu) / 255.0f - 0.5f) * 7.0f;

            uint32_t packed = 0xFF000000u;
            for (uint32_t ch = 0u; ch < 3u; ++ch) {
                uint32_t s = ch * 8u;
                float vc   = (float)((cc >> s) & 0xFFu);
                float blur = 0.25f * (float)(((cl >> s) & 0xFFu) + ((cr >> s) & 0xFFu) +
                                             ((cd >> s) & 0xFFu) + ((cu >> s) & 0xFFu));
                float sharp = vc + (vc - blur) * 1.45f + grain;
                uint32_t bv = (uint32_t)fminf(fmaxf(lrintf(sharp), 0L), 255L);
                packed |= (bv << s);
            }
            out_px[py * k_dim + px] = packed;
        }
    }

    (void)backend_survey_save_png_file(img_path, out_px, k_dim, k_dim);
    for (uint32_t k = 0u; k < leaf_cnt; ++k) {
        free(leaf_tiles[k]);
    }
    free(base_px);
    free(out_px);
}

static void load_overlay_into_bda(void) {
    if (g_backend.overlay_gpu_pixels == nullptr || g_backend.gpu_hdr == nullptr) {
        return;
    }
    resolve_overlay_image_path(g_backend.pack_path,
                               g_backend.overlay_image_path,
                               sizeof g_backend.overlay_image_path);
    ensure_default_overlay_image(g_backend.overlay_image_path);

    uint32_t  raw_w      = 0u;
    uint32_t  raw_h      = 0u;
    uint32_t *raw_pixels = nullptr;
    if (backend_survey_load_image_file(g_backend.overlay_image_path, &raw_pixels, &raw_w, &raw_h) != 0 ||
        raw_pixels == nullptr || raw_w == 0u || raw_h == 0u) {
        g_backend.overlay_active = false;
        sync_gpu_overlay_header();
        return;
    }

    uint32_t dst_w = raw_w > k_backend_overlay_max_dim ? k_backend_overlay_max_dim : raw_w;
    uint32_t dst_h = raw_h > k_backend_overlay_max_dim ? k_backend_overlay_max_dim : raw_h;
    uint32_t *fit_pixels = raw_pixels;
    if (dst_w != raw_w || dst_h != raw_h) {
        fit_pixels = (uint32_t *)malloc((size_t)dst_w * (size_t)dst_h * sizeof(uint32_t));
        if (fit_pixels == nullptr) {
            free(raw_pixels);
            return;
        }
        for (uint32_t y = 0u; y < dst_h; ++y) {
            uint32_t sy = (uint32_t)(((uint64_t)y * raw_h) / dst_h);
            for (uint32_t x = 0u; x < dst_w; ++x) {
                uint32_t sx = (uint32_t)(((uint64_t)x * raw_w) / dst_w);
                fit_pixels[y * dst_w + x] = raw_pixels[sy * raw_w + sx];
            }
        }
        free(raw_pixels);
    }

    free(g_backend.overlay_cpu_pixels);
    g_backend.overlay_cpu_pixels = fit_pixels;
    g_backend.overlay_w          = dst_w;
    g_backend.overlay_h          = dst_h;
    memcpy(g_backend.overlay_gpu_pixels, fit_pixels, (size_t)dst_w * (size_t)dst_h * sizeof(uint32_t));

    if (!g_backend.overlay_active) {
        float aspect = (float)dst_h / fmaxf((float)dst_w, 1.0f);
        g_backend.overlay_center_x       = 1595.0f;
        g_backend.overlay_center_y       = -650.0f;
        g_backend.overlay_base_half_x    = 650.0f;
        g_backend.overlay_base_half_y    = 650.0f * aspect;
        g_backend.overlay_scale_x        = 1.0f;
        g_backend.overlay_scale_y        = 1.0f;
        g_backend.overlay_opacity        = 0.50f; // Starts at 50% transparency (50% opacity)
        g_backend.overlay_preview_hidden = false;
        for (uint32_t i = 0u; i < g_backend.survey.ortho_overlay_count; ++i) {
            const TerrainOrthoOverlay *saved = &g_backend.survey.ortho_overlays[i];
            if (overlay_paths_match(saved->image_path, g_backend.overlay_image_path)) {
                g_backend.overlay_center_x       = saved->center_x;
                g_backend.overlay_center_y       = saved->center_y;
                g_backend.overlay_scale_x        = saved->half_span_x / fmaxf(g_backend.overlay_base_half_x, 1.0f);
                g_backend.overlay_scale_y        = saved->half_span_y / fmaxf(g_backend.overlay_base_half_y, 1.0f);
                g_backend.overlay_preview_hidden = true;
            }
        }
        g_backend.overlay_active = true;
    } else {
        float aspect = (float)dst_h / fmaxf((float)dst_w, 1.0f);
        g_backend.overlay_base_half_y = g_backend.overlay_base_half_x * aspect;
    }
    sync_committed_ortho_layers_to_bda();
    sync_gpu_overlay_header();
}

static bool load_archive_into_bda(void) {
    if (g_backend.gpu_hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        return false;
    }
    for (uint32_t s = 0u; s < k_gmpack_gpu_slot_cap; ++s) {
        g_backend.slot_to_node[s]   = UINT32_MAX;
        g_backend.slot_lru_clock[s] = 0u;
        g_backend.slot_dirty[s]     = 0u;
    }
    g_backend.dirty_slot_count = 0u;
    for (uint32_t c = 0u; c < k_backend_cell_ring_cap; ++c) {
        cell64_init(&g_backend.cells[c], (uint8_t)(c + 1u));
    }

    if (g_backend.pack_fd >= 0) {
        close(g_backend.pack_fd);
        g_backend.pack_fd = -1;
    }
    memset(g_backend.node_fuse_state, 0, sizeof g_backend.node_fuse_state);

    if (backend_terrain_read_header(g_backend.pack_path, g_backend.gpu_hdr) != 0) {
        return false;
    }
    if (backend_terrain_read_tile(g_backend.pack_path, 0u, &g_backend.gpu_tiles[0]) != 0) {
        return false;
    }

    resolve_survey_manifest_path(g_backend.pack_path,
                                 g_backend.survey_manifest_path,
                                 sizeof g_backend.survey_manifest_path);
    backend_survey_dataset_free(&g_backend.survey);
    backend_survey_dataset_init(&g_backend.survey);
    (void)backend_survey_ensure_default_manifest(g_backend.survey_manifest_path, &g_backend.gpu_hdr->dir);
    (void)backend_survey_load_manifest(&g_backend.survey,
                                       &g_backend.gpu_hdr->dir,
                                       g_backend.survey_manifest_path);
    if (g_backend.survey.persist_gmpack) {
        if (backend_survey_apply_to_pack_file(&g_backend.survey,
                                              g_backend.pack_path,
                                              &g_backend.survey.last_stats) == 0) {
            (void)backend_terrain_read_header(g_backend.pack_path, g_backend.gpu_hdr);
            (void)backend_terrain_read_tile(g_backend.pack_path, 0u, &g_backend.gpu_tiles[0]);
        }
    } else {
        int changed = backend_survey_fuse_tile(&g_backend.survey,
                                               g_backend.gpu_hdr,
                                               0u,
                                               &g_backend.gpu_tiles[0],
                                               &g_backend.survey.last_stats);
        if (changed > 0) {
            if (g_backend.fused_cache[0] == nullptr) {
                g_backend.fused_cache[0] = (GpuMathTileBlock *)malloc(sizeof(GpuMathTileBlock));
            }
            if (g_backend.fused_cache[0] != nullptr) {
                memcpy(g_backend.fused_cache[0], &g_backend.gpu_tiles[0], sizeof(GpuMathTileBlock));
                g_backend.node_fuse_state[0] = 2u;
            }
        } else {
            g_backend.node_fuse_state[0] = 1u;
        }
    }

    // Mark only root node 0 resident in slot 0 initially; finer LODs stream on demand
    for (uint32_t i = 0u; i < k_gmpack_node_count; ++i) {
        GpuMathQuadNode *n = &g_backend.gpu_hdr->nodes[i];
        uint8_t dec_x = 0u;
        uint8_t dec_y = 0u;
        uint32_t blk   = 0u;
        uint16_t mrt   = 0u;
        uint8_t  lod   = 0u;
        uint8_t  flags = 0u;
        float    min_e = 0.0f;
        float    max_e = 0.0f;
        gpumath_tile_header_unpack(n->header, &blk, &mrt, &lod, &flags, &min_e, &max_e);
        relptr2d_morton2d_decode(mrt, &dec_x, &dec_y);
        if (i == 0u) {
            n->rel_gpu_payload = relptr2d_pack((int32_t)k_gmpack_header_blocks, (int16_t)dec_x, (int16_t)dec_y, 0u, 1u);
            n->gpu_slot        = 0u;
            n->resident        = 1u;
        } else {
            n->rel_gpu_payload = k_relptr2d_null;
            n->gpu_slot        = 0u;
            n->resident        = 0u;
        }
    }

    float default_fx = 1595.0f;
    float default_fy = -650.0f;
    if (g_backend.survey.ortho_overlay_count > 0u) {
        const TerrainOrthoOverlay *last_ov =
            &g_backend.survey.ortho_overlays[g_backend.survey.ortho_overlay_count - 1u];
        default_fx = last_ov->center_x;
        default_fy = last_ov->center_y;
    }
    float restore_fx   = g_backend.have_focus ? g_backend.last_focus_x : default_fx;
    float restore_fy   = g_backend.have_focus ? g_backend.last_focus_y : default_fy;
    float restore_zoom = g_backend.have_focus ? g_backend.last_zoom    : 0.16f;

    g_backend.slot_to_node[0]   = 0u;
    g_backend.slot_lru_clock[0] = ++g_backend.lru_tick;
    g_backend.slot_dirty[0]     = 1u;
    g_backend.dirty_slot_count  = 1u;
    g_backend.dirty_slots_mask  = 1u;
    g_backend.have_focus        = false;
    backend_gpu_directory_sync();

    // Perform initial LOD cut for default opening pose (or preserved focus on hot-reload)
    (void)backend_update_camera_zoom(restore_fx, restore_fy, restore_zoom);

    // Load the Bird's-Eye Orthophoto Overlay into GPU BDA and set 50% transparency
    load_overlay_into_bda();

    if (g_backend.indirect_hdr != nullptr) {
        TerrainCullParams init_cull = {
            .width          = 1920u,
            .height         = 1080u,
            .elev_base_m    = k_besparmak_elev_min,
            .vertical_scale = 1.15f,
            .verts_per_tile = (64u * 64u + 64u) * 6u,
            .qx             = 0.0f,
            .qy             = 0.0f,
            .qz             = 0.0f,
            .qw             = 1.0f,
            .cam_x          = restore_fx,
            .cam_y          = restore_fy,
            .cam_z          = 180.0f,
            .zoom           = restore_zoom,
        };
        (void)backend_stream_cull_frustum(g_backend.gpu_hdr, &init_cull, g_backend.indirect_hdr);
    }
    return true;
}

int backend_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (g_backend.initialized) {
        return 0;
    }
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.vk      = vk;
    g_backend.pack_fd = -1;
    resolve_pack_path(g_backend.pack_path, sizeof g_backend.pack_path);
    resolve_survey_manifest_path(g_backend.pack_path,
                                 g_backend.survey_manifest_path,
                                 sizeof g_backend.survey_manifest_path);

    if (da_buffer_create(vk, k_backend_bda_bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &g_backend.bda_buf) != 0) {
        fprintf(stderr, "[Backend Error] Failed to allocate %u KiB GPU BDA terrain arena\n",
                k_backend_bda_bytes / 1024u);
        return -1;
    }
    system_vma_set_name(g_backend.bda_buf.map, k_backend_bda_bytes, "engine_gpu_math_terrain");
    memset(g_backend.bda_buf.map, 0, k_backend_bda_bytes);

    if (da_buffer_create(vk, sizeof(GpuMathIndirectBuffer),
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &g_backend.indirect_buf) != 0) {
        fprintf(stderr, "[Backend Error] Failed to allocate GPU BDA indirect draw buffer\n");
        da_buffer_destroy(vk, &g_backend.bda_buf);
        return -1;
    }
    system_vma_set_name(g_backend.indirect_buf.map, sizeof(GpuMathIndirectBuffer), "engine_gpu_math_indirect");
    memset(g_backend.indirect_buf.map, 0, sizeof(GpuMathIndirectBuffer));

    if (da_buffer_create(vk, k_backend_overlay_bda_bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &g_backend.overlay_buf) != 0) {
        fprintf(stderr, "[Backend Error] Failed to allocate GPU BDA bird's-eye overlay buffer\n");
        da_buffer_destroy(vk, &g_backend.indirect_buf);
        da_buffer_destroy(vk, &g_backend.bda_buf);
        return -1;
    }
    system_vma_set_name(g_backend.overlay_buf.map, k_backend_overlay_bda_bytes, "engine_gpu_math_overlay");
    memset(g_backend.overlay_buf.map, 0, k_backend_overlay_bda_bytes);

    g_backend.gpu_hdr            = (GpuMathArchiveHeader *)g_backend.bda_buf.map;
    g_backend.gpu_tiles          = (GpuMathTileBlock *)((uint8_t *)g_backend.bda_buf.map + k_gmpack_header_bytes);
    g_backend.indirect_hdr       = (GpuMathIndirectBuffer *)g_backend.indirect_buf.map;
    g_backend.overlay_gpu_pixels = (uint32_t *)g_backend.overlay_buf.map;

    if (backend_gpu_directory_init(vk) != 0) {
        da_buffer_destroy(vk, &g_backend.overlay_buf);
        da_buffer_destroy(vk, &g_backend.indirect_buf);
        da_buffer_destroy(vk, &g_backend.bda_buf);
        return -1;
    }

    if (!load_archive_into_bda()) {
        backend_gpu_directory_shutdown(vk);
        da_buffer_destroy(vk, &g_backend.overlay_buf);
        da_buffer_destroy(vk, &g_backend.indirect_buf);
        da_buffer_destroy(vk, &g_backend.bda_buf);
        return -1;
    }

    g_backend.initialized = true;
    printf("[Backend] GPU-Math Terrain Streamer ready: %s | BDA=0x%llx | IndirectBDA=0x%llx | %u RelPtr2D nodes (256x256) | Baklan & Beşparmak Dağı (%.0f..%.0f m) | Survey: %u photo(s), %u GCP(s), %u tie-pt(s), %u ortho upload(s) | Bird's-Eye Overlay: %s (%ux%u @ %.0f%% transparency)\n",
           g_backend.pack_path,
           (unsigned long long)g_backend.bda_buf.address,
           (unsigned long long)g_backend.indirect_buf.address,
           k_gmpack_node_count,
           (double)g_backend.gpu_hdr->dir.elev_min_m,
           (double)g_backend.gpu_hdr->dir.elev_max_m,
           g_backend.survey.last_stats.photos_loaded,
           g_backend.survey.last_stats.survey_points_loaded,
           g_backend.survey.last_stats.tiepoints_triangulated,
           g_backend.survey.ortho_overlay_count,
           g_backend.overlay_image_path,
           g_backend.overlay_w,
           g_backend.overlay_h,
           (double)((1.0f - g_backend.overlay_opacity) * 100.0f));
    fflush(stdout);
    return 0;
}

void backend_shutdown(VulkanContext *vk) {
    if (!g_backend.initialized) {
        return;
    }
    if (g_backend.pack_fd >= 0) {
        close(g_backend.pack_fd);
        g_backend.pack_fd = -1;
    }
    for (uint32_t i = 0u; i < k_gmpack_node_count; ++i) {
        if (g_backend.fused_cache[i] != nullptr) {
            free(g_backend.fused_cache[i]);
            g_backend.fused_cache[i] = nullptr;
        }
    }
    free(g_backend.overlay_cpu_pixels);
    g_backend.overlay_cpu_pixels = nullptr;
    backend_survey_dataset_free(&g_backend.survey);
    backend_gpu_directory_shutdown(vk);
    da_buffer_destroy(vk, &g_backend.overlay_buf);
    da_buffer_destroy(vk, &g_backend.indirect_buf);
    da_buffer_destroy(vk, &g_backend.bda_buf);
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.pack_fd = -1;
}

bool backend_update_camera_zoom(float focus_x, float focus_y, float zoom) {
    if (g_backend.gpu_hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        return false;
    }
    if (g_backend.have_focus &&
        fabsf(focus_x - g_backend.last_focus_x) < 4.0f &&
        fabsf(focus_y - g_backend.last_focus_y) < 4.0f &&
        fabsf(zoom - g_backend.last_zoom) < 0.015f * fmaxf(zoom, 0.005f)) {
        return false;
    }
    g_backend.last_focus_x = focus_x;
    g_backend.last_focus_y = focus_y;
    g_backend.last_zoom    = zoom;
    g_backend.have_focus   = true;

    uint32_t needed[k_gmpack_gpu_slot_cap] = {0};
    uint32_t needed_count = 0u;
    bool cut_changed = backend_stream_update_lod(g_backend.gpu_hdr, focus_x, focus_y, zoom,
                                                 needed, &needed_count);
    uint64_t newly_paged_mask = 0ull;
    (void)backend_stream_page_ssd_tiles(g_backend.pack_path,
                                        g_backend.gpu_hdr,
                                        g_backend.gpu_tiles,
                                        needed,
                                        needed_count,
                                        &newly_paged_mask);
    g_backend.dirty_slots_mask |= newly_paged_mask;
    return cut_changed || (newly_paged_mask != 0ull);
}

void backend_record_compute(VkCommandBuffer cmd, const TerrainCullParams *cull) {
    if (!g_backend.initialized) {
        return;
    }
    backend_gpu_record_filter(cmd);
    if (cull != nullptr) {
        backend_gpu_record_cull(cmd, cull);
    }
}

uint64_t backend_directory_bda(void) {
    return g_backend.initialized ? g_backend.bda_buf.address : 0ull;
}

uint64_t backend_indirect_bda(void) {
    return g_backend.initialized ? g_backend.indirect_buf.address : 0ull;
}

VkBuffer backend_indirect_buffer(VkDeviceSize *out_base_offset) {
    if (out_base_offset != nullptr) {
        *out_base_offset = g_backend.initialized ? g_backend.indirect_buf.offset : 0u;
    }
    return g_backend.initialized ? g_backend.indirect_buf.buffer : VK_NULL_HANDLE;
}

uint32_t backend_active_leaf_count(void) {
    if (!g_backend.initialized || g_backend.gpu_hdr == nullptr) {
        return 0u;
    }
    return g_backend.gpu_hdr->dir.active_leaf_count;
}

uint32_t backend_visible_leaf_count(void) {
    if (!g_backend.initialized || g_backend.indirect_hdr == nullptr) {
        return 0u;
    }
    uint32_t vis = g_backend.indirect_hdr->draw_count;
    if (vis == 0u && g_backend.gpu_hdr != nullptr) {
        return g_backend.gpu_hdr->dir.active_leaf_count;
    }
    return vis;
}

static float sample_tile_bilinear_m(const GpuMathTileBlock *blk, float u, float v) {
    float tx = fminf(fmaxf(u, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u);
    float ty = fminf(fmaxf(v, 0.0f), 1.0f) * (float)(k_gmpack_tile_dim - 1u);
    uint32_t x0 = (uint32_t)floorf(tx);
    uint32_t y0 = (uint32_t)floorf(ty);
    uint32_t x1 = x0 + 1u < k_gmpack_tile_dim ? x0 + 1u : k_gmpack_tile_dim - 1u;
    uint32_t y1 = y0 + 1u < k_gmpack_tile_dim ? y0 + 1u : k_gmpack_tile_dim - 1u;
    float fx = tx - (float)x0;
    float fy = ty - (float)y0;

    float h00 = (float)blk->height_u16[y0 * k_gmpack_tile_dim + x0] * 0.0625f;
    float h10 = (float)blk->height_u16[y0 * k_gmpack_tile_dim + x1] * 0.0625f;
    float h01 = (float)blk->height_u16[y1 * k_gmpack_tile_dim + x0] * 0.0625f;
    float h11 = (float)blk->height_u16[y1 * k_gmpack_tile_dim + x1] * 0.0625f;
    float hx0 = h00 + (h10 - h00) * fx;
    float hx1 = h01 + (h11 - h01) * fx;
    return hx0 + (hx1 - hx0) * fy;
}

bool backend_sample_world_xy(float wx, float wy, TerrainSampleInfo *out) {
    if (!g_backend.initialized || g_backend.gpu_hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        return false;
    }
    float half = g_backend.gpu_hdr->dir.world_half_extent;
    if (wx < -half || wx > half || wy < -half || wy > half) {
        return false;
    }

    // Find the finest resident quadtree node covering (wx, wy)
    uint32_t best_node = 0u;
    uint32_t leaf_cnt  = g_backend.gpu_hdr->dir.active_leaf_count;
    for (uint32_t i = 0u; i < leaf_cnt; ++i) {
        uint32_t idx = g_backend.gpu_hdr->dir.active_leaves[i];
        if (idx >= k_gmpack_node_count) {
            continue;
        }
        const GpuMathQuadNode *n = &g_backend.gpu_hdr->nodes[idx];
        if (wx >= n->world_min_x && wx <= n->world_max_x &&
            wy >= n->world_min_y && wy <= n->world_max_y) {
            best_node = idx;
            break;
        }
    }
    const GpuMathQuadNode *node = &g_backend.gpu_hdr->nodes[best_node];
    while (node->resident == 0u && node->lod > 0u) {
        best_node = node->parent_index;
        node = &g_backend.gpu_hdr->nodes[best_node];
    }

    uint32_t slot = node->gpu_slot < k_gmpack_gpu_slot_cap ? node->gpu_slot : 0u;
    const GpuMathTileBlock *blk = &g_backend.gpu_tiles[slot];

    float span_x = fmaxf(node->world_max_x - node->world_min_x, 1.0f);
    float span_y = fmaxf(node->world_max_y - node->world_min_y, 1.0f);
    float u = (wx - node->world_min_x) / span_x;
    float v = (wy - node->world_min_y) / span_y;

    float elev_m = sample_tile_bilinear_m(blk, u, v);
    float du = 1.0f / (float)(k_gmpack_tile_dim - 1u);
    float hl = sample_tile_bilinear_m(blk, u - du, v);
    float hr = sample_tile_bilinear_m(blk, u + du, v);
    float hd = sample_tile_bilinear_m(blk, u, v - du);
    float hu = sample_tile_bilinear_m(blk, u, v + du);
    float step_m = 24000.0f / (float)((1u << node->lod) * (k_gmpack_tile_dim - 1u));
    float dzdx = (hr - hl) / (2.0f * step_m);
    float dzdy = (hu - hd) / (2.0f * step_m);
    float slope_deg = atanf(sqrtf(dzdx * dzdx + dzdy * dzdy)) * 57.2957795f;

    float global_u = (wx + half) / (2.0f * half);
    float global_v = (wy + half) / (2.0f * half);
    double lon = g_backend.gpu_hdr->dir.lon_min_deg +
                 (double)global_u * (g_backend.gpu_hdr->dir.lon_max_deg - g_backend.gpu_hdr->dir.lon_min_deg);
    double lat = g_backend.gpu_hdr->dir.lat_min_deg +
                 (double)global_v * (g_backend.gpu_hdr->dir.lat_max_deg - g_backend.gpu_hdr->dir.lat_min_deg);

    if (out != nullptr) {
        *out = (TerrainSampleInfo){
            .latitude_deg  = lat,
            .longitude_deg = lon,
            .world_x       = wx,
            .world_y       = wy,
            .world_z       = (elev_m - k_besparmak_elev_min) * g_backend.gpu_hdr->dir.vertical_scale,
            .elevation_m   = elev_m,
            .slope_deg     = slope_deg,
            .node_index    = best_node,
            .lod           = node->lod,
            .tile_gx       = node->grid_x,
            .tile_gy       = node->grid_y,
        };
    }
    return true;
}

float backend_sample_elevation_world_z(float wx, float wy) {
    TerrainSampleInfo info = {0};
    if (backend_sample_world_xy(wx, wy, &info)) {
        return info.world_z;
    }
    return 180.0f;
}

bool backend_ray_pick_terrain(float ox, float oy, float oz,
                              float dx, float dy, float dz,
                              TerrainSampleInfo *out) {
    if (!g_backend.initialized || g_backend.gpu_hdr == nullptr) {
        return false;
    }
    // Ray-march from the top of Beşparmak Dağı's world Z envelope down to the z=0 Baklan plain
    float max_z = (g_backend.gpu_hdr->dir.elev_max_m - k_besparmak_elev_min) *
                  g_backend.gpu_hdr->dir.vertical_scale + 40.0f;
    float min_z = -200.0f;
    if (fabsf(dz) < 1.0e-5f) {
        return false;
    }
    float t_start = (max_z - oz) / dz;
    float t_end   = (min_z - oz) / dz;
    if (t_start > t_end) {
        float tmp = t_start;
        t_start = t_end;
        t_end = tmp;
    }
    if (t_end < 0.0f) {
        return false;
    }
    if (t_start < 0.0f) {
        t_start = 0.0f;
    }

    constexpr uint32_t k_steps = 96u;
    float dt = (t_end - t_start) / (float)k_steps;
    float prev_t = t_start;
    for (uint32_t s = 0u; s <= k_steps; ++s) {
        float t = t_start + (float)s * dt;
        float rx = ox + dx * t;
        float ry = oy + dy * t;
        float rz = oz + dz * t;
        TerrainSampleInfo info = {0};
        if (backend_sample_world_xy(rx, ry, &info)) {
            if (rz <= info.world_z) {
                // Binary search refinement across [prev_t, t]
                float lo = prev_t;
                float hi = t;
                for (int b = 0; b < 8; ++b) {
                    float mid = 0.5f * (lo + hi);
                    TerrainSampleInfo m_info = {0};
                    if (backend_sample_world_xy(ox + dx * mid, oy + dy * mid, &m_info) &&
                        (oz + dz * mid) <= m_info.world_z) {
                        hi = mid;
                        info = m_info;
                    } else {
                        lo = mid;
                    }
                }
                if (out != nullptr) {
                    *out = info;
                }
                return true;
            }
        }
        prev_t = t;
    }
    return false;
}

bool backend_compute_terrain_profile(float ax, float ay,
                                     float bx, float by,
                                     TerrainProfileInfo *out) {
    if (out == nullptr) {
        return false;
    }
    TerrainSampleInfo sa = {0};
    TerrainSampleInfo sb = {0};
    if (!backend_sample_world_xy(ax, ay, &sa) || !backend_sample_world_xy(bx, by, &sb)) {
        return false;
    }

    float dx = sb.world_x - sa.world_x;
    float dy = sb.world_y - sa.world_y;
    float horiz_dist_m = sqrtf(dx * dx + dy * dy);
    float elev_delta_m = sb.elevation_m - sa.elevation_m;

    // Compass bearing: 0° = North (+Y), 90° = East (+X), 180° = South (-Y), 270° = West (-X)
    float bearing_deg = atan2f(dx, dy) * 57.2957795f;
    if (bearing_deg < 0.0f) {
        bearing_deg += 360.0f;
    }

    uint32_t steps = (uint32_t)ceilf(horiz_dist_m / 12.0f);
    if (steps < 16u) {
        steps = 16u;
    } else if (steps > 256u) {
        steps = 256u;
    }

    float dist_3d_m     = 0.0f;
    float elev_gain_m   = 0.0f;
    float elev_loss_m   = 0.0f;
    float min_elev_m    = fminf(sa.elevation_m, sb.elevation_m);
    float max_elev_m    = fmaxf(sa.elevation_m, sb.elevation_m);
    float max_grade_pct = 0.0f;
    float max_slope_deg = fmaxf(sa.slope_deg, sb.slope_deg);

    float prev_x = sa.world_x;
    float prev_y = sa.world_y;
    float prev_e = sa.elevation_m;

    for (uint32_t i = 1u; i <= steps; ++i) {
        float t  = (float)i / (float)steps;
        float sx = sa.world_x + dx * t;
        float sy = sa.world_y + dy * t;
        TerrainSampleInfo s = {0};
        if (!backend_sample_world_xy(sx, sy, &s)) {
            continue;
        }
        float seg_dx = sx - prev_x;
        float seg_dy = sy - prev_y;
        float seg_h  = sqrtf(seg_dx * seg_dx + seg_dy * seg_dy);
        float seg_de = s.elevation_m - prev_e;

        dist_3d_m += sqrtf(seg_h * seg_h + seg_de * seg_de);
        if (seg_de > 0.0f) {
            elev_gain_m += seg_de;
        } else {
            elev_loss_m += -seg_de;
        }
        if (s.elevation_m < min_elev_m) {
            min_elev_m = s.elevation_m;
        }
        if (s.elevation_m > max_elev_m) {
            max_elev_m = s.elevation_m;
        }
        if (s.slope_deg > max_slope_deg) {
            max_slope_deg = s.slope_deg;
        }
        if (seg_h > 0.1f) {
            float grade = (fabsf(seg_de) / seg_h) * 100.0f;
            if (grade > max_grade_pct) {
                max_grade_pct = grade;
            }
        }
        prev_x = sx;
        prev_y = sy;
        prev_e = s.elevation_m;
    }

    float avg_grade_pct = horiz_dist_m > 0.1f ? (elev_delta_m / horiz_dist_m) * 100.0f : 0.0f;

    *out = (TerrainProfileInfo){
        .point_a       = sa,
        .point_b       = sb,
        .horiz_dist_m  = horiz_dist_m,
        .dist_3d_m     = dist_3d_m,
        .elev_delta_m  = elev_delta_m,
        .elev_gain_m   = elev_gain_m,
        .elev_loss_m   = elev_loss_m,
        .min_elev_m    = min_elev_m,
        .max_elev_m    = max_elev_m,
        .avg_grade_pct = avg_grade_pct,
        .max_grade_pct = max_grade_pct,
        .max_slope_deg = max_slope_deg,
        .bearing_deg   = bearing_deg,
        .sample_count  = steps + 1u,
    };
    return true;
}

bool backend_reload_pack(void) {
    if (!g_backend.initialized) {
        return false;
    }
    if (g_backend.suppress_manifest_reload) {
        g_backend.suppress_manifest_reload = false;
        return true;
    }
    bool ok = load_archive_into_bda();
    if (ok) {
        printf("[Backend] Reloaded Baklan & Beşparmak Dağı .gmpack + Survey (%u photo(s), %u GCP(s), %u tie-pt(s), %u ortho overlay(s), %u tiles fused).\n",
               g_backend.survey.last_stats.photos_loaded,
               g_backend.survey.last_stats.survey_points_loaded,
               g_backend.survey.last_stats.tiepoints_triangulated,
               g_backend.survey.ortho_overlay_count,
               g_backend.survey.last_stats.tiles_updated);
        fflush(stdout);
    }
    return ok;
}

const char *backend_survey_manifest_path(void) {
    if (g_backend.survey_manifest_path[0] == '\0') {
        resolve_pack_path(g_backend.pack_path, sizeof g_backend.pack_path);
        resolve_survey_manifest_path(g_backend.pack_path,
                                     g_backend.survey_manifest_path,
                                     sizeof g_backend.survey_manifest_path);
    }
    return g_backend.survey_manifest_path;
}

bool backend_survey_get_stats(TerrainSurveyStats *out_stats) {
    if (!g_backend.initialized) {
        return false;
    }
    if (out_stats != nullptr) {
        *out_stats = g_backend.survey.last_stats;
    }
    return true;
}

void backend_overlay_set_custom_path(const char *image_path) {
    if (image_path == nullptr || image_path[0] == '\0') {
        s_custom_overlay_path[0] = '\0';
        return;
    }
    snprintf(s_custom_overlay_path, sizeof s_custom_overlay_path, "%s", image_path);
    if (g_backend.initialized) {
        g_backend.overlay_active = false;
        load_overlay_into_bda();
    }
}

bool backend_overlay_move(float dx_m, float dy_m, bool log_state) {
    if (!g_backend.initialized || !g_backend.overlay_active || g_backend.gpu_hdr == nullptr) {
        return false;
    }
    if (dx_m == 0.0f && dy_m == 0.0f) {
        return false;
    }
    bool was_hidden = g_backend.overlay_preview_hidden;
    g_backend.overlay_preview_hidden = false;
    float half = g_backend.gpu_hdr->dir.world_half_extent;
    g_backend.overlay_center_x = fminf(fmaxf(g_backend.overlay_center_x + dx_m, -half), half);
    g_backend.overlay_center_y = fminf(fmaxf(g_backend.overlay_center_y + dy_m, -half), half);
    if (was_hidden) {
        sync_committed_ortho_layers_to_bda();
    }
    sync_gpu_overlay_header();
    if (log_state) {
        double lat = 0.0;
        double lon = 0.0;
        backend_survey_world_to_wgs84(&g_backend.gpu_hdr->dir,
                                      g_backend.overlay_center_x,
                                      g_backend.overlay_center_y,
                                      &lat,
                                      &lon);
        printf("[Bird's-Eye Overlay] Moved parallel to map -> Center=(%.1f, %.1f m | %.5f°N, %.5f°E) [Hold WASD for smooth glide]\n",
               (double)g_backend.overlay_center_x,
               (double)g_backend.overlay_center_y,
               lat,
               lon);
        fflush(stdout);
    }
    return true;
}

bool backend_overlay_adjust_transparency(float delta_transparency) {
    if (!g_backend.initialized || !g_backend.overlay_active || g_backend.gpu_hdr == nullptr) {
        return false;
    }
    bool was_hidden = g_backend.overlay_preview_hidden;
    g_backend.overlay_preview_hidden = false;
    // transparency = 1.0 - opacity; positive delta_transparency makes the image MORE transparent
    float old_opacity  = g_backend.overlay_opacity;
    float next_opacity = fminf(fmaxf(old_opacity - delta_transparency, 0.0f), 1.0f);
    next_opacity       = roundf(next_opacity * 100.0f) * 0.01f;
    if (!was_hidden && fabsf(next_opacity - old_opacity) < 1.0e-4f) {
        printf("[Bird's-Eye Overlay] Transparency already at %.0f%% (Opacity: %.0f%%)\n",
               (double)((1.0f - old_opacity) * 100.0f),
               (double)(old_opacity * 100.0f));
        fflush(stdout);
        return false;
    }
    g_backend.overlay_opacity = next_opacity;
    if (was_hidden) {
        sync_committed_ortho_layers_to_bda();
    }
    sync_gpu_overlay_header();
    printf("[Bird's-Eye Overlay] Transparency: %.0f%% (Opacity: %.0f%%) | Key 'O' = more transparent, Key 'P' = less transparent\n",
           (double)((1.0f - next_opacity) * 100.0f),
           (double)(next_opacity * 100.0f));
    fflush(stdout);
    return true;
}

bool backend_overlay_stretch_2d(int step_x, int step_y) {
    if (!g_backend.initialized || !g_backend.overlay_active || g_backend.gpu_hdr == nullptr) {
        return false;
    }
    if (step_x == 0 && step_y == 0) {
        return false;
    }
    bool was_hidden = g_backend.overlay_preview_hidden;
    g_backend.overlay_preview_hidden = false;
    // Exact 0.5% (0.005) stretch step in 2 dimensions
    float next_sx = fminf(fmaxf(g_backend.overlay_scale_x + (float)step_x * 0.005f, 0.05f), 20.0f);
    float next_sy = fminf(fmaxf(g_backend.overlay_scale_y + (float)step_y * 0.005f, 0.05f), 20.0f);
    next_sx = roundf(next_sx * 1000.0f) * 0.001f;
    next_sy = roundf(next_sy * 1000.0f) * 0.001f;
    g_backend.overlay_scale_x = next_sx;
    g_backend.overlay_scale_y = next_sy;
    if (was_hidden) {
        sync_committed_ortho_layers_to_bda();
    }
    sync_gpu_overlay_header();

    float span_x = 2.0f * g_backend.overlay_base_half_x * next_sx;
    float span_y = 2.0f * g_backend.overlay_base_half_y * next_sy;
    printf("[Bird's-Eye Overlay] 2D Stretch (0.5%% step): X=%.1f%% (%.1f m), Y=%.1f%% (%.1f m) | Center=(%.1f, %.1f m)\n",
           (double)(next_sx * 100.0f),
           (double)span_x,
           (double)(next_sy * 100.0f),
           (double)span_y,
           (double)g_backend.overlay_center_x,
           (double)g_backend.overlay_center_y);
    fflush(stdout);
    return true;
}

bool backend_overlay_commit_upload(void) {
    if (!g_backend.initialized || !g_backend.overlay_active ||
        g_backend.overlay_cpu_pixels == nullptr || g_backend.gpu_hdr == nullptr) {
        return false;
    }
    float hx = g_backend.overlay_base_half_x * g_backend.overlay_scale_x;
    float hy = g_backend.overlay_base_half_y * g_backend.overlay_scale_y;
    TerrainOrthoOverlay ov = {
        .center_x     = g_backend.overlay_center_x,
        .center_y     = g_backend.overlay_center_y,
        .half_span_x  = hx,
        .half_span_y  = hy,
        .width_px     = g_backend.overlay_w,
        .height_px    = g_backend.overlay_h,
        .rgba_pixels  = g_backend.overlay_cpu_pixels,
        .blend_weight = 1.0f,
    };
    snprintf(ov.image_path, sizeof ov.image_path, "%.*s",
             (int)(sizeof ov.image_path - 1u), g_backend.overlay_image_path);
    (void)backend_survey_add_ortho_overlay(&g_backend.survey, &ov);

    uint32_t gpu_slots_baked = 0u;
    uint32_t ssd_tiles_saved = 0u;
    uint32_t texels_baked    = 0u;
    for (uint32_t s = 0u; s < k_gmpack_gpu_slot_cap; ++s) {
        uint32_t node_idx = g_backend.slot_to_node[s];
        if (node_idx >= k_gmpack_node_count) {
            continue;
        }
        const GpuMathQuadNode *node = &g_backend.gpu_hdr->nodes[node_idx];
        uint32_t n_tex = backend_survey_bake_ortho_tile(node,
                                                        &g_backend.gpu_tiles[s],
                                                        ov.rgba_pixels,
                                                        ov.width_px,
                                                        ov.height_px,
                                                        ov.center_x,
                                                        ov.center_y,
                                                        ov.half_span_x,
                                                        ov.half_span_y,
                                                        ov.blend_weight);
        if (n_tex > 0u) {
            g_backend.slot_dirty[s]     = 1u;
            g_backend.dirty_slots_mask |= (1ull << s);
            gpu_slots_baked++;
            texels_baked += n_tex;
            if (g_backend.fused_cache[node_idx] == nullptr) {
                g_backend.fused_cache[node_idx] = (GpuMathTileBlock *)malloc(sizeof(GpuMathTileBlock));
            }
            if (g_backend.fused_cache[node_idx] != nullptr) {
                memcpy(g_backend.fused_cache[node_idx], &g_backend.gpu_tiles[s], sizeof(GpuMathTileBlock));
                g_backend.node_fuse_state[node_idx] = 2u;
            }
            if (g_backend.pack_path[0] != '\0' &&
                backend_terrain_write_tile(g_backend.pack_path, node_idx, &g_backend.gpu_tiles[s]) == 0) {
                ssd_tiles_saved++;
            }
        }
    }

    // Also bake and save any non-resident quadtree tiles overlapping the bird's-eye rectangle
    for (uint32_t idx = 0u; idx < k_gmpack_node_count; ++idx) {
        const GpuMathQuadNode *node = &g_backend.gpu_hdr->nodes[idx];
        if (node->resident != 0u && node->gpu_slot < k_gmpack_gpu_slot_cap &&
            g_backend.slot_to_node[node->gpu_slot] == idx) {
            continue;
        }
        if (node->world_max_x < ov.center_x - hx || node->world_min_x > ov.center_x + hx ||
            node->world_max_y < ov.center_y - hy || node->world_min_y > ov.center_y + hy) {
            continue;
        }
        if (g_backend.fused_cache[idx] == nullptr) {
            g_backend.fused_cache[idx] = (GpuMathTileBlock *)malloc(sizeof(GpuMathTileBlock));
            if (g_backend.fused_cache[idx] != nullptr && g_backend.pack_path[0] != '\0') {
                (void)backend_terrain_read_tile(g_backend.pack_path, idx, g_backend.fused_cache[idx]);
            }
        }
        if (g_backend.fused_cache[idx] != nullptr) {
            uint32_t n_tex = backend_survey_bake_ortho_tile(node,
                                                            g_backend.fused_cache[idx],
                                                            ov.rgba_pixels,
                                                            ov.width_px,
                                                            ov.height_px,
                                                            ov.center_x,
                                                            ov.center_y,
                                                            ov.half_span_x,
                                                            ov.half_span_y,
                                                            ov.blend_weight);
            if (n_tex > 0u) {
                g_backend.node_fuse_state[idx] = 2u;
                texels_baked += n_tex;
                if (g_backend.pack_path[0] != '\0' &&
                    backend_terrain_write_tile(g_backend.pack_path, idx, g_backend.fused_cache[idx]) == 0) {
                    ssd_tiles_saved++;
                }
            }
        }
    }

    // Persist the uploaded bird's-eye placement to the survey manifest (quoted path for spaces)
    if (g_backend.survey_manifest_path[0] != '\0') {
        const char *rel_img = g_backend.overlay_image_path;
        const char *slash   = strrchr(g_backend.overlay_image_path, '/');
        const char *mslash  = strrchr(g_backend.survey_manifest_path, '/');
        if (slash != nullptr && mslash != nullptr &&
            (size_t)(slash - g_backend.overlay_image_path) == (size_t)(mslash - g_backend.survey_manifest_path) &&
            memcmp(g_backend.overlay_image_path, g_backend.survey_manifest_path,
                   (size_t)(slash - g_backend.overlay_image_path)) == 0) {
            rel_img = slash + 1;
        }
        g_backend.suppress_manifest_reload = true;
        FILE *fp = fopen(g_backend.survey_manifest_path, "a");
        if (fp != nullptr) {
            fprintf(fp,
                    "\northo_overlay \"%s\" wx=%.2f wy=%.2f hx=%.2f hy=%.2f weight=1.00\n",
                    rel_img,
                    (double)ov.center_x,
                    (double)ov.center_y,
                    (double)ov.half_span_x,
                    (double)ov.half_span_y);
            fclose(fp);
        }
    }

    g_backend.overlay_committed_cnt++;
    g_backend.overlay_preview_hidden = true;
    sync_committed_ortho_layers_to_bda();
    sync_gpu_overlay_header();

    double lat = 0.0;
    double lon = 0.0;
    backend_survey_world_to_wgs84(&g_backend.gpu_hdr->dir, ov.center_x, ov.center_y, &lat, &lon);
    printf("[Bird's-Eye Upload] SAVED '%s' (%ux%u) onto map from bird's-eye view! | Center: (%.1f, %.1f m | %.5f°N, %.5f°E) | Size: %.1fx%.1f m (Stretch X=%.1f%%, Y=%.1f%%) | Baked %u GPU tiles + %u SSD .gmpack tiles (%u texels) & saved to %s\n",
           g_backend.overlay_image_path,
           ov.width_px,
           ov.height_px,
           (double)ov.center_x,
           (double)ov.center_y,
           lat,
           lon,
           (double)(2.0f * hx),
           (double)(2.0f * hy),
           (double)(g_backend.overlay_scale_x * 100.0f),
           (double)(g_backend.overlay_scale_y * 100.0f),
           gpu_slots_baked,
           ssd_tiles_saved,
           texels_baked,
           g_backend.survey_manifest_path);
    fflush(stdout);
    return true;
}

bool backend_overlay_get_center(float *out_wx, float *out_wy) {
    if (!g_backend.initialized || !g_backend.overlay_active) {
        return false;
    }
    if (out_wx != nullptr) {
        *out_wx = g_backend.overlay_center_x;
    }
    if (out_wy != nullptr) {
        *out_wy = g_backend.overlay_center_y;
    }
    return true;
}
