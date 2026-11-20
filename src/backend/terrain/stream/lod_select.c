#include "backend/internal.h"
#include <math.h>
#include <string.h>

// ============================================================================
// Zoom-Driven Adaptive Quadtree Cut Selector (lod_select.c)
// ============================================================================

static float point_to_aabb_dist_bounds(float px, float py,
                                       float min_x, float min_y,
                                       float max_x, float max_y) {
    float dx = 0.0f;
    if (px < min_x) {
        dx = min_x - px;
    } else if (px > max_x) {
        dx = px - max_x;
    }
    float dy = 0.0f;
    if (py < min_y) {
        dy = min_y - py;
    } else if (py > max_y) {
        dy = py - max_y;
    }
    return sqrtf(dx * dx + dy * dy);
}

static float point_to_aabb_dist(float px, float py, const GpuMathQuadNode *n) {
    return point_to_aabb_dist_bounds(px, py,
                                     n->world_min_x, n->world_min_y,
                                     n->world_max_x, n->world_max_y);
}

static void v2_disk_node_bounds(const EpackDiskNode *dn,
                                float                world_half,
                                float               *out_min_x,
                                float               *out_min_y,
                                float               *out_max_x,
                                float               *out_max_y) {
    float span = (2.0f * world_half) / (float)(1u << (dn->lod & 15u));
    float x0   = -world_half + (float)dn->grid_x * span;
    float y0   = -world_half + (float)dn->grid_y * span;
    *out_min_x = x0;
    *out_min_y = y0;
    *out_max_x = x0 + span;
    *out_max_y = y0 + span;
}

static float v2_node_subdivide_priority(const EpackDiskNode *nodes,
                                        uint32_t             node_count,
                                        uint32_t             disk_idx,
                                        float                world_half,
                                        float                focus_x,
                                        float                focus_y,
                                        float                zoom) {
    if (nodes == nullptr || disk_idx >= node_count) {
        return -1.0f;
    }
    const EpackDiskNode *dn = &nodes[disk_idx];
    if (dn->first_child_idx == UINT32_MAX || dn->first_child_idx + 3u >= node_count || dn->lod >= 14u) {
        return -1.0f;
    }
    bool any_child_data = false;
    for (uint32_t c = 0u; c < 4u; ++c) {
        if (nodes[dn->first_child_idx + c].ssd_offset_bytes != 0ull) {
            any_child_data = true;
            break;
        }
    }
    if (!any_child_data) {
        return -1.0f;
    }

    float min_x, min_y, max_x, max_y;
    v2_disk_node_bounds(dn, world_half, &min_x, &min_y, &max_x, &max_y);
    float span      = fmaxf(max_x - min_x, 0.5f);
    float z         = fmaxf(zoom, 0.015f);
    float screen_px = span * z;
    float view_r    = fmaxf(900.0f / z, 180.0f);
    float dist      = point_to_aabb_dist_bounds(focus_x, focus_y, min_x, min_y, max_x, max_y);
    float falloff   = 1.0f + powf(dist / view_r, 1.45f);
    float eff_px    = screen_px / falloff;
    if (eff_px < 180.0f) {
        return -1.0f;
    }
    return (1000000.0f / (50.0f + dist)) + (float)(16u - dn->lod) * 200.0f;
}

static uint32_t find_finest_dem_ancestor(const EpackDiskNode *nodes,
                                         uint32_t             node_count,
                                         uint32_t             disk_idx) {
    if (nodes == nullptr || disk_idx >= node_count) {
        return 0u;
    }
    const EpackDiskNode *dn = &nodes[disk_idx];
    uint32_t cur = 0u;
    uint32_t dem = 0u;
    for (uint8_t step_l = 0u; step_l <= dn->lod && cur < node_count; ++step_l) {
        if (nodes[cur].has_dem != 0u && nodes[cur].ssd_offset_bytes != 0ull) {
            dem = cur;
        }
        if (step_l == dn->lod || nodes[cur].first_child_idx == UINT32_MAX) {
            break;
        }
        uint32_t shift = (uint32_t)(dn->lod - step_l - 1u);
        uint32_t qx    = ((uint32_t)dn->grid_x >> shift) & 1u;
        uint32_t qy    = ((uint32_t)dn->grid_y >> shift) & 1u;
        cur = nodes[cur].first_child_idx + (qy * 2u + qx);
    }
    return dem;
}

void backend_stream_refresh_v2_leaf_bindings(GpuMathArchiveHeader *hdr) {
    if (hdr == nullptr || !backend_archive_is_v2(hdr) ||
        !g_backend.is_v2 || g_backend.v2_nodes == nullptr) {
        return;
    }
    const EpackDiskNode *nodes    = g_backend.v2_nodes;
    const uint32_t       ncnt     = g_backend.v2_node_count;
    const uint32_t       slot_cap = backend_slot_cap();
    const float          half     = fmaxf(hdr->dir.world_half_extent, 1.0f);

    uint32_t leaf_cnt = hdr->dir.active_leaf_count;
    if (leaf_cnt > k_epack_v2_cut_cap) {
        leaf_cnt = k_epack_v2_cut_cap;
    }
    EpackV2Meta *meta = backend_epack_v2_meta(hdr);
    meta->active_leaf_ext_cnt = leaf_cnt;

    for (uint32_t i = 0u; i < leaf_cnt; ++i) {
        uint32_t disk_idx = g_backend.v2_leaf_disk_ids[i];
        if (disk_idx >= ncnt) {
            disk_idx = 0u;
        }
        const EpackDiskNode *dn = &nodes[disk_idx];

        // Walk root -> disk_idx (<= 14 steps) to bind finest COMPLETED resident DEM & RGB tiles
        uint32_t cur     = 0u;
        uint32_t dem_nid = 0u;
        uint32_t rgb_nid = 0u;
        for (uint8_t step_l = 0u; step_l <= dn->lod && cur < ncnt; ++step_l) {
            const EpackDiskNode *cn = &nodes[cur];
            uint32_t cslot = g_backend.v2_node_to_slot[cur];
            if (cslot < slot_cap && g_backend.slot_to_node[cslot] == cur &&
                g_backend.slot_inflight[cslot] == 0u) {
                if (cn->has_dem != 0u) {
                    dem_nid = cur;
                }
                if (cn->has_rgb != 0u) {
                    rgb_nid = cur;
                }
            }
            if (step_l == dn->lod || cn->first_child_idx == UINT32_MAX) {
                break;
            }
            uint32_t shift = (uint32_t)(dn->lod - step_l - 1u);
            uint32_t qx    = ((uint32_t)dn->grid_x >> shift) & 1u;
            uint32_t qy    = ((uint32_t)dn->grid_y >> shift) & 1u;
            cur = cn->first_child_idx + (qy * 2u + qx);
        }

        uint32_t dem_slot = (g_backend.v2_node_to_slot[dem_nid] < slot_cap)
            ? g_backend.v2_node_to_slot[dem_nid] : 0u;
        uint32_t rgb_slot = (g_backend.v2_node_to_slot[rgb_nid] < slot_cap)
            ? g_backend.v2_node_to_slot[rgb_nid] : 0u;
        const EpackDiskNode *dem_dn = &nodes[dem_nid];
        const EpackDiskNode *rgb_dn = &nodes[rgb_nid];

        float min_x, min_y, max_x, max_y;
        v2_disk_node_bounds(dn, half, &min_x, &min_y, &max_x, &max_y);

        uint32_t dir_node_idx = 1u + i;
        GpuMathQuadNode *gn   = &hdr->nodes[dir_node_idx];

        gn->rel_ssd_payload = relptr2d_pack(
            (int32_t)(k_gmpack_header_blocks + dem_slot * k_epack_tile_blocks_4k),
            (int16_t)dem_dn->grid_x, (int16_t)dem_dn->grid_y, dem_dn->lod, 1u);
        gn->rel_gpu_payload = relptr2d_pack(
            (int32_t)(k_gmpack_header_blocks + rgb_slot * k_epack_tile_blocks_4k),
            (int16_t)rgb_dn->grid_x, (int16_t)rgb_dn->grid_y, rgb_dn->lod, 1u);

        uint32_t emin_bits = 0u;
        uint32_t emax_bits = 0u;
        memcpy(&emin_bits, &dn->elev_min_m, sizeof emin_bits);
        memcpy(&emax_bits, &dn->elev_max_m, sizeof emax_bits);
        gn->rel_children = (RelPtr2D)((uint64_t)emin_bits | ((uint64_t)emax_bits << 32u));

        float hdr_min = fmaxf(dn->elev_min_m, 0.0f);
        float hdr_max = fmaxf(dn->elev_max_m, hdr_min + 1.0f);
        gn->header = gpumath_tile_header_pack(
            k_gmpack_header_blocks + rgb_slot * k_epack_tile_blocks_4k,
            0u, dn->lod, k_tile_flag_resident, hdr_min, hdr_max);
        gn->world_min_x  = min_x;
        gn->world_min_y  = min_y;
        gn->world_max_x  = max_x;
        gn->world_max_y  = max_y;
        gn->node_index   = (uint16_t)dir_node_idx;
        gn->parent_index = 0u;
        gn->gpu_slot     = (uint16_t)rgb_slot;
        gn->lod          = dn->lod;
        gn->grid_x       = (uint8_t)(dn->grid_x & 0xFFu);
        gn->grid_y       = (uint8_t)(dn->grid_y & 0xFFu);
        gn->resident     = 1u;
        gn->pad_tail     = (uint16_t)dem_slot;
        gn->elev_mean_m  = dn->elev_mean_m;

        if (i < k_gmpack_active_cut_cap) {
            hdr->dir.active_leaves[i] = (uint16_t)dir_node_idx;
        } else {
            meta->active_leaves_hi[i - k_gmpack_active_cut_cap] = (uint16_t)dir_node_idx;
        }
    }
}

typedef struct V2NeededCand {
    uint32_t disk_idx;
    float    dist;
} V2NeededCand;

static void add_unique_needed(uint32_t node_idx, uint32_t *list, uint32_t *count, uint32_t cap) {
    for (uint32_t i = 0u; i < *count; ++i) {
        if (list[i] == node_idx) {
            return;
        }
    }
    if (*count < cap) {
        list[(*count)++] = node_idx;
    }
}

static bool update_lod_v2(GpuMathArchiveHeader *hdr,
                          float                 focus_x,
                          float                 focus_y,
                          float                 zoom,
                          uint32_t             *out_needed_nodes,
                          uint32_t             *out_needed_count) {
    const EpackDiskNode *nodes = g_backend.v2_nodes;
    const uint32_t       ncnt  = g_backend.v2_node_count;
    const float          half  = fmaxf(hdr->dir.world_half_extent, 1.0f);

    uint32_t prev_count = hdr->dir.active_leaf_count;
    if (prev_count > k_epack_v2_cut_cap) {
        prev_count = k_epack_v2_cut_cap;
    }
    uint32_t prev_disk[k_epack_v2_cut_cap];
    memcpy(prev_disk, g_backend.v2_leaf_disk_ids, prev_count * sizeof(uint32_t));

    uint32_t next_disk[k_epack_v2_cut_cap] = {0};
    uint32_t next_count = 1u;
    next_disk[0] = 0u;

    while (next_count + 3u <= k_epack_v2_cut_cap) {
        int32_t best_slot = -1;
        float   best_prio = 0.0f;
        for (uint32_t i = 0u; i < next_count; ++i) {
            float prio = v2_node_subdivide_priority(nodes, ncnt, next_disk[i],
                                                    half, focus_x, focus_y, zoom);
            if (prio > best_prio) {
                best_prio = prio;
                best_slot = (int32_t)i;
            }
        }
        if (best_slot < 0) {
            break;
        }
        uint32_t fc = nodes[next_disk[best_slot]].first_child_idx;
        next_disk[best_slot]      = fc + 0u;
        next_disk[next_count]     = fc + 1u;
        next_disk[next_count + 1] = fc + 2u;
        next_disk[next_count + 2] = fc + 3u;
        next_count += 3u;
    }

    uint32_t deepest_lod = 0u;
    V2NeededCand cands[k_epack_v2_cut_cap * 2u];
    uint32_t     cand_cnt = 0u;
    for (uint32_t i = 0u; i < next_count; ++i) {
        uint32_t did = next_disk[i];
        if (did >= ncnt) {
            continue;
        }
        if (nodes[did].lod > deepest_lod) {
            deepest_lod = nodes[did].lod;
        }
        float x0, y0, x1, y1;
        v2_disk_node_bounds(&nodes[did], half, &x0, &y0, &x1, &y1);
        float dist = point_to_aabb_dist_bounds(focus_x, focus_y, x0, y0, x1, y1);
        if (nodes[did].ssd_offset_bytes != 0ull) {
            cands[cand_cnt++] = (V2NeededCand){ .disk_idx = did, .dist = dist };
        }
        if (nodes[did].has_dem == 0u) {
            uint32_t dem_anc = find_finest_dem_ancestor(nodes, ncnt, did);
            if (dem_anc != 0u && nodes[dem_anc].ssd_offset_bytes != 0ull) {
                cands[cand_cnt++] = (V2NeededCand){ .disk_idx = dem_anc, .dist = dist * 0.98f };
            }
        }
    }

    // Insertion-sort candidates closest-first so cache-constrained UMA locks the viewport core
    for (uint32_t a = 1u; a < cand_cnt; ++a) {
        V2NeededCand key = cands[a];
        uint32_t b = a;
        while (b > 0u && cands[b - 1u].dist > key.dist) {
            cands[b] = cands[b - 1u];
            --b;
        }
        cands[b] = key;
    }

    const uint32_t slot_cap   = backend_slot_cap();
    const uint32_t needed_cap = (slot_cap > 4u && slot_cap - 4u < k_gmpack_gpu_slot_cap)
        ? (slot_cap - 4u)
        : k_gmpack_gpu_slot_cap;
    uint32_t needed_cnt = 0u;
    add_unique_needed(0u, out_needed_nodes, &needed_cnt, needed_cap);
    for (uint32_t c = 0u; c < cand_cnt && needed_cnt < needed_cap; ++c) {
        add_unique_needed(cands[c].disk_idx, out_needed_nodes, &needed_cnt, needed_cap);
    }

    memcpy(g_backend.v2_leaf_disk_ids, next_disk, next_count * sizeof(uint32_t));
    hdr->dir.active_leaf_count  = next_count;
    hdr->dir.deepest_lod_active = deepest_lod;
    hdr->dir.focus_world_x      = focus_x;
    hdr->dir.focus_world_y      = focus_y;
    hdr->dir.focus_zoom         = zoom;
    hdr->dir.frame_epoch++;
    *out_needed_count = needed_cnt;

    backend_stream_refresh_v2_leaf_bindings(hdr);

    return (next_count != prev_count) ||
           (memcmp(prev_disk, next_disk, next_count * sizeof(uint32_t)) != 0);
}

static float node_subdivide_priority(const GpuMathQuadNode *n, float focus_x, float focus_y, float zoom) {
    if (n == nullptr || n->lod + 1u >= k_gmpack_lod_count || n->rel_children == k_relptr2d_null) {
        return -1.0f;
    }
    float span_x = fmaxf(n->world_max_x - n->world_min_x, 1.0f);
    float screen_px = span_x * zoom;
    float dist = point_to_aabb_dist(focus_x, focus_y, n);
    float dist_px = dist * zoom;

    bool want = false;
    switch (n->lod) {
    case 0u:
        want = screen_px >= 240.0f;
        break;
    case 1u:
        want = screen_px >= 240.0f && (dist <= span_x * 1.65f || dist_px <= 2200.0f);
        break;
    case 2u:
        want = screen_px >= 240.0f && (dist <= span_x * 1.65f || dist_px <= 1900.0f);
        break;
    case 3u:
        want = screen_px >= 220.0f && (dist <= span_x * 2.25f || dist_px <= 2100.0f);
        break;
    default:
        want = false;
        break;
    }
    if (!want) {
        return -1.0f;
    }
    // Strictly prioritize tiles closest to the camera focus so the focal area
    // always reaches LOD 4 before outer peripheral rings consume leaf slots.
    return (100000.0f / (250.0f + dist)) + (float)(4u - n->lod) * 25.0f;
}

bool backend_stream_update_lod(GpuMathArchiveHeader *hdr,
                               float                 focus_x,
                               float                 focus_y,
                               float                 zoom,
                               uint32_t             *out_needed_nodes,
                               uint32_t             *out_needed_count) {
    if (hdr == nullptr || out_needed_nodes == nullptr || out_needed_count == nullptr) {
        return false;
    }
    if (backend_archive_is_v2(hdr) && g_backend.is_v2 && g_backend.v2_nodes != nullptr) {
        return update_lod_v2(hdr, focus_x, focus_y, zoom, out_needed_nodes, out_needed_count);
    }
    uint16_t prev_leaves[k_gmpack_active_cut_cap];
    uint32_t prev_count = hdr->dir.active_leaf_count;
    if (prev_count > k_gmpack_active_cut_cap) {
        prev_count = k_gmpack_active_cut_cap;
    }
    memcpy(prev_leaves, hdr->dir.active_leaves, prev_count * sizeof(uint16_t));

    // Iterative priority-driven quadtree cut: guarantees a 100% gap-free partition
    // of the terrain while subdividing tiles closest to (focus_x, focus_y) all the way to LOD 4.
    uint16_t next_leaves[k_gmpack_active_cut_cap] = {0};
    uint32_t next_count = 1u;
    next_leaves[0] = 0u;

    while (next_count + 3u <= k_gmpack_active_cut_cap) {
        int32_t best_slot = -1;
        float   best_prio = 0.0f;
        for (uint32_t i = 0u; i < next_count; ++i) {
            uint32_t idx = next_leaves[i];
            if (idx >= k_gmpack_node_count) {
                continue;
            }
            float prio = node_subdivide_priority(&hdr->nodes[idx], focus_x, focus_y, zoom);
            if (prio > best_prio) {
                best_prio = prio;
                best_slot = (int32_t)i;
            }
        }
        if (best_slot < 0) {
            break;
        }
        const GpuMathQuadNode *parent = &hdr->nodes[next_leaves[best_slot]];
        uint8_t child_lod = (uint8_t)(parent->lod + 1u);
        uint8_t cx0       = (uint8_t)(parent->grid_x * 2u);
        uint8_t cy0       = (uint8_t)(parent->grid_y * 2u);
        uint16_t c00 = (uint16_t)backend_quadtree_node_index(child_lod, cx0, cy0);
        uint16_t c10 = (uint16_t)backend_quadtree_node_index(child_lod, (uint8_t)(cx0 + 1u), cy0);
        uint16_t c01 = (uint16_t)backend_quadtree_node_index(child_lod, cx0, (uint8_t)(cy0 + 1u));
        uint16_t c11 = (uint16_t)backend_quadtree_node_index(child_lod, (uint8_t)(cx0 + 1u), (uint8_t)(cy0 + 1u));
        if (c00 == 0u || c10 == 0u || c01 == 0u || c11 == 0u) {
            break;
        }
        next_leaves[best_slot]      = c00;
        next_leaves[next_count]     = c10;
        next_leaves[next_count + 1] = c01;
        next_leaves[next_count + 2] = c11;
        next_count += 3u;
    }

    uint32_t needed_cnt  = 0u;
    uint32_t deepest_lod = 0u;
    // Root node 0 is always pinned in slot 0
    add_unique_needed(0u, out_needed_nodes, &needed_cnt, k_gmpack_gpu_slot_cap);
    for (uint32_t i = 0u; i < next_count; ++i) {
        uint32_t idx = next_leaves[i];
        add_unique_needed(idx, out_needed_nodes, &needed_cnt, k_gmpack_gpu_slot_cap);
        if (idx < k_gmpack_node_count && hdr->nodes[idx].lod > deepest_lod) {
            deepest_lod = hdr->nodes[idx].lod;
        }
    }

    hdr->dir.active_leaf_count  = next_count;
    hdr->dir.deepest_lod_active = deepest_lod;
    hdr->dir.focus_world_x      = focus_x;
    hdr->dir.focus_world_y      = focus_y;
    hdr->dir.focus_zoom         = zoom;
    hdr->dir.frame_epoch++;
    memcpy(hdr->dir.active_leaves, next_leaves, sizeof next_leaves);
    *out_needed_count = needed_cnt;

    bool cut_changed = (next_count != prev_count) ||
                       (memcmp(prev_leaves, next_leaves, next_count * sizeof(uint16_t)) != 0);
    return cut_changed;
}

static bool tile_aabb_in_frustum_cpu(const GpuMathArchiveHeader *hdr,
                                     const GpuMathQuadNode      *node,
                                     const TerrainCullParams    *cull,
                                     bool                        is_v2,
                                     uint32_t                   *out_tile_verts) {
    if (hdr == nullptr || node == nullptr || cull == nullptr) {
        return false;
    }
    if (out_tile_verts != nullptr) {
        *out_tile_verts = cull->verts_per_tile;
    }
    float min_elev = 0.0f;
    float max_elev = 0.0f;
    if (is_v2 && node->rel_children != 0ull) {
        uint32_t lo = (uint32_t)(node->rel_children & 0xFFFFFFFFull);
        uint32_t hi = (uint32_t)(node->rel_children >> 32u);
        memcpy(&min_elev, &lo, sizeof min_elev);
        memcpy(&max_elev, &hi, sizeof max_elev);
    } else {
        uint32_t blk    = 0u;
        uint16_t morton = 0u;
        uint8_t  lod    = 0u;
        uint8_t  flags  = 0u;
        gpumath_tile_header_unpack(node->header, &blk, &morton, &lod, &flags, &min_elev, &max_elev);
        (void)blk;
        (void)morton;
        (void)lod;
        (void)flags;
    }

    if (max_elev <= min_elev) {
        if (hdr->dir.elev_max_m > hdr->dir.elev_min_m) {
            min_elev = hdr->dir.elev_min_m - 16.0f;
            max_elev = hdr->dir.elev_max_m + 16.0f;
        } else {
            min_elev = 800.0f;
            max_elev = 1720.0f;
        }
    } else {
        min_elev = min_elev - 32.0f;
        max_elev = max_elev + 32.0f;
    }

    float z0 = (min_elev - cull->elev_base_m) * cull->vertical_scale;
    float z1 = (max_elev - cull->elev_base_m) * cull->vertical_scale;

    float sx = fmaxf((float)cull->width, 1.0f) * 0.5f;
    float sy = fmaxf((float)cull->height, 1.0f) * 0.5f;

    float min_nx =  1.0e9f;
    float min_ny =  1.0e9f;
    float min_nz =  1.0e9f;
    float max_nx = -1.0e9f;
    float max_ny = -1.0e9f;
    float max_nz = -1.0e9f;

    for (uint32_t corner = 0u; corner < 8u; ++corner) {
        float wx = ((corner & 1u) != 0u ? node->world_max_x : node->world_min_x) - cull->cam_x;
        float wy = ((corner & 2u) != 0u ? node->world_max_y : node->world_min_y) - cull->cam_y;
        float wz = ((corner & 4u) != 0u ? z1 : z0) - cull->cam_z;

        // t = 2.0 * cross(q.xyz, v)
        float tx = 2.0f * (cull->qy * wz - cull->qz * wy);
        float ty = 2.0f * (cull->qz * wx - cull->qx * wz);
        float tz = 2.0f * (cull->qx * wy - cull->qy * wx);

        // vpos = v + q.w * t + cross(q.xyz, t)
        float vx = wx + cull->qw * tx + (cull->qy * tz - cull->qz * ty);
        float vy = wy + cull->qw * ty + (cull->qz * tx - cull->qx * tz);
        float vz = wz + cull->qw * tz + (cull->qx * ty - cull->qy * tx);

        float nx = (vx * cull->zoom) / sx;
        float ny = (-vy * cull->zoom) / sy;
        float nz = 0.5f - vz / 65536.0f;

        if (nx < min_nx) min_nx = nx;
        if (ny < min_ny) min_ny = ny;
        if (nz < min_nz) min_nz = nz;
        if (nx > max_nx) max_nx = nx;
        if (ny > max_ny) max_ny = ny;
        if (nz > max_nz) max_nz = nz;
    }

    const float k_guard = 1.08f;
    if (max_nx < -k_guard || min_nx > k_guard ||
        max_ny < -k_guard || min_ny > k_guard ||
        max_nz < 0.0f     || min_nz > 1.0f) {
        return false;
    }

    if (out_tile_verts != nullptr) {
        float px_span = fmaxf((max_nx - min_nx) * sx, (max_ny - min_ny) * sy);
        uint32_t v = cull->verts_per_tile;
        if (px_span < 160.0f && v > (16u * 16u + 64u) * 6u) {
            v = (16u * 16u + 64u) * 6u;
        } else if (px_span < 340.0f && v > (32u * 32u + 64u) * 6u) {
            v = (32u * 32u + 64u) * 6u;
        }
        *out_tile_verts = v;
    }
    return true;
}

uint32_t backend_stream_cull_frustum(const GpuMathArchiveHeader *hdr,
                                     const TerrainCullParams    *cull,
                                     GpuMathIndirectBuffer      *out_indirect) {
    if (hdr == nullptr || cull == nullptr || out_indirect == nullptr) {
        return 0u;
    }
    const bool is_v2   = backend_archive_is_v2(hdr);
    const uint32_t cap = is_v2 ? k_epack_v2_cut_cap : k_gmpack_active_cut_cap;
    const EpackV2Meta *meta = is_v2 ? backend_epack_v2_meta_const(hdr) : nullptr;
    uint32_t count = hdr->dir.active_leaf_count;
    if (count > cap) {
        count = cap;
    }

    uint32_t visible = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t node_idx = (i < k_gmpack_active_cut_cap)
            ? hdr->dir.active_leaves[i]
            : meta->active_leaves_hi[i - k_gmpack_active_cut_cap];
        if (node_idx >= k_gmpack_node_count) {
            continue;
        }
        const GpuMathQuadNode *node = &hdr->nodes[node_idx];
        uint32_t tile_verts = cull->verts_per_tile;
        if (tile_aabb_in_frustum_cpu(hdr, node, cull, is_v2, &tile_verts)) {
            uint32_t slot = visible++;
            out_indirect->visible_nodes[slot] = node_idx;
            out_indirect->cmds[slot] = (GpuMathDrawCmd){
                .vertex_count   = tile_verts,
                .instance_count = 1u,
                .first_vertex   = 0u,
                .first_instance = slot,
            };
        }
    }

    out_indirect->draw_count        = visible;
    out_indirect->culled_count      = count - visible;
    out_indirect->active_leaf_total = count;
    out_indirect->pad0              = 0u;
    out_indirect->single_cmd        = (GpuMathDrawCmd){
        .vertex_count   = cull->verts_per_tile,
        .instance_count = visible,
        .first_vertex   = 0u,
        .first_instance = 0u,
    };
    return visible;
}
