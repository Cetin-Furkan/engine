#include "backend/internal.h"
#include "system/system.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// GPU-Math & Terrain Streaming Backend Lifecycle & Ray-DEM Solver (lifecycle.c)
// ============================================================================

BackendState g_backend;

static const char *k_default_pack_rel = "examples/demo/maps/baklan_besparmak.gmpack";

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

static bool load_archive_into_bda(void) {
    if (g_backend.gpu_hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        return false;
    }
    for (uint32_t s = 0u; s < k_gmpack_gpu_slot_cap; ++s) {
        g_backend.slot_to_node[s]   = UINT32_MAX;
        g_backend.slot_lru_clock[s] = 0u;
    }
    for (uint32_t c = 0u; c < k_backend_cell_ring_cap; ++c) {
        cell64_init(&g_backend.cells[c], (uint8_t)(c + 1u));
    }

    if (backend_terrain_read_header(g_backend.pack_path, g_backend.gpu_hdr) != 0) {
        return false;
    }
    if (backend_terrain_read_tile(g_backend.pack_path, 0u, &g_backend.gpu_tiles[0]) != 0) {
        return false;
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

    g_backend.slot_to_node[0]   = 0u;
    g_backend.slot_lru_clock[0] = ++g_backend.lru_tick;
    g_backend.dirty_slots_mask  = 1u;
    g_backend.have_focus        = false;
    backend_gpu_directory_sync();

    // Perform initial LOD cut for default opening pose over Beşparmak Dağı
    (void)backend_update_camera_zoom(1595.0f, -650.0f, 0.16f);
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
    g_backend.vk = vk;
    resolve_pack_path(g_backend.pack_path, sizeof g_backend.pack_path);

    if (da_buffer_create(vk, k_backend_bda_bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &g_backend.bda_buf) != 0) {
        fprintf(stderr, "[Backend Error] Failed to allocate %u KiB GPU BDA terrain arena\n",
                k_backend_bda_bytes / 1024u);
        return -1;
    }
    system_vma_set_name(g_backend.bda_buf.map, k_backend_bda_bytes, "engine_gpu_math_terrain");
    memset(g_backend.bda_buf.map, 0, k_backend_bda_bytes);

    g_backend.gpu_hdr   = (GpuMathArchiveHeader *)g_backend.bda_buf.map;
    g_backend.gpu_tiles = (GpuMathTileBlock *)((uint8_t *)g_backend.bda_buf.map + k_gmpack_header_bytes);

    if (backend_gpu_directory_init(vk) != 0) {
        da_buffer_destroy(vk, &g_backend.bda_buf);
        return -1;
    }

    if (!load_archive_into_bda()) {
        backend_gpu_directory_shutdown(vk);
        da_buffer_destroy(vk, &g_backend.bda_buf);
        return -1;
    }

    g_backend.initialized = true;
    printf("[Backend] GPU-Math Terrain Streamer ready: %s | BDA=0x%llx | %u RelPtr2D nodes (256x256) | Baklan & Beşparmak Dağı (%.0f..%.0f m)\n",
           g_backend.pack_path,
           (unsigned long long)g_backend.bda_buf.address,
           k_gmpack_node_count,
           (double)g_backend.gpu_hdr->dir.elev_min_m,
           (double)g_backend.gpu_hdr->dir.elev_max_m);
    fflush(stdout);
    return 0;
}

void backend_shutdown(VulkanContext *vk) {
    if (!g_backend.initialized) {
        return;
    }
    backend_gpu_directory_shutdown(vk);
    da_buffer_destroy(vk, &g_backend.bda_buf);
    memset(&g_backend, 0, sizeof g_backend);
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

void backend_record_compute(VkCommandBuffer cmd) {
    if (!g_backend.initialized) {
        return;
    }
    backend_gpu_record_filter(cmd);
}

uint64_t backend_directory_bda(void) {
    return g_backend.initialized ? g_backend.bda_buf.address : 0ull;
}

uint32_t backend_active_leaf_count(void) {
    if (!g_backend.initialized || g_backend.gpu_hdr == nullptr) {
        return 0u;
    }
    return g_backend.gpu_hdr->dir.active_leaf_count;
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

bool backend_reload_pack(void) {
    if (!g_backend.initialized) {
        return false;
    }
    bool ok = load_archive_into_bda();
    if (ok) {
        printf("[Backend] Reloaded Baklan & Beşparmak Dağı .gmpack archive from SSD.\n");
        fflush(stdout);
    }
    return ok;
}
