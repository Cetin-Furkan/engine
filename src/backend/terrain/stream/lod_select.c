#include "backend/internal.h"
#include <math.h>
#include <string.h>

// ============================================================================
// Zoom-Driven Adaptive Quadtree Cut Selector (lod_select.c)
// ============================================================================

static float point_to_aabb_dist(float px, float py, const GpuMathQuadNode *n) {
    float dx = 0.0f;
    if (px < n->world_min_x) {
        dx = n->world_min_x - px;
    } else if (px > n->world_max_x) {
        dx = px - n->world_max_x;
    }
    float dy = 0.0f;
    if (py < n->world_min_y) {
        dy = n->world_min_y - py;
    } else if (py > n->world_max_y) {
        dy = py - n->world_max_y;
    }
    return sqrtf(dx * dx + dy * dy);
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

bool backend_stream_update_lod(GpuMathArchiveHeader *hdr,
                               float                 focus_x,
                               float                 focus_y,
                               float                 zoom,
                               uint32_t             *out_needed_nodes,
                               uint32_t             *out_needed_count) {
    if (hdr == nullptr || out_needed_nodes == nullptr || out_needed_count == nullptr) {
        return false;
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

static bool tile_aabb_in_frustum_cpu(const GpuMathQuadNode   *node,
                                     const TerrainCullParams *cull,
                                     uint32_t                *out_tile_verts) {
    if (node == nullptr || cull == nullptr) {
        return false;
    }
    if (out_tile_verts != nullptr) {
        *out_tile_verts = cull->verts_per_tile;
    }
    uint32_t blk      = 0u;
    uint16_t morton   = 0u;
    uint8_t  lod      = 0u;
    uint8_t  flags    = 0u;
    float    min_elev = 0.0f;
    float    max_elev = 0.0f;
    gpumath_tile_header_unpack(node->header, &blk, &morton, &lod, &flags, &min_elev, &max_elev);
    (void)blk;
    (void)morton;
    (void)lod;
    (void)flags;

    if (max_elev <= min_elev) {
        min_elev = 800.0f;
        max_elev = 1720.0f;
    } else {
        min_elev = fmaxf(min_elev - 32.0f, 0.0f);
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
    uint32_t count = hdr->dir.active_leaf_count;
    if (count > k_gmpack_active_cut_cap) {
        count = k_gmpack_active_cut_cap;
    }

    uint32_t visible = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t node_idx = hdr->dir.active_leaves[i];
        if (node_idx >= k_gmpack_node_count) {
            continue;
        }
        const GpuMathQuadNode *node = &hdr->nodes[node_idx];
        uint32_t tile_verts = cull->verts_per_tile;
        if (tile_aabb_in_frustum_cpu(node, cull, &tile_verts)) {
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
