#include "backend/internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

// ============================================================================
// 64-Byte Stateful Function Cell SSD -> GPU BDA Demand Pager (uring_pager.c)
// ============================================================================

typedef struct PagerExecCtx {
    int                   fd;
    GpuMathArchiveHeader *hdr;
    GpuMathTileBlock     *gpu_tile_pool;
} PagerExecCtx;

static int ssd_tile_cell_exec(GpuMathStreamCell *cell, void *user_ctx) {
    PagerExecCtx *ctx = (PagerExecCtx *)user_ctx;
    if (cell == nullptr || ctx == nullptr || ctx->fd < 0 ||
        cell->tile_id >= k_gmpack_node_count || cell->slot_index >= k_gmpack_gpu_slot_cap) {
        return -1;
    }

    // Resolve the exact 4KB-aligned SSD file offset directly from the 2D Self-Relative Pointer
    uint64_t ssd_off = relptr2d_resolve_addr(0ull, cell->rel_tile);
    if (ssd_off < k_gmpack_header_bytes || ssd_off + k_gmpack_tile_bytes > k_gmpack_total_bytes) {
        return -1;
    }

    uint8_t *dst = (uint8_t *)&ctx->gpu_tile_pool[cell->slot_index];
    size_t done = 0u;
    while (done < cell->bytes) {
        ssize_t n = pread(ctx->fd, dst + done, cell->bytes - done, (off_t)(ssd_off + done));
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

    // Update the node's GPU BDA RelPtr2D and Lilliput 64-bit unified header
    GpuMathQuadNode *node = &ctx->hdr->nodes[cell->tile_id];
    int32_t gpu_block_4k = (int32_t)(k_gmpack_header_blocks + cell->slot_index * k_gmpack_tile_blocks_4k);
    node->rel_gpu_payload = relptr2d_pack(gpu_block_4k,
                                          (int16_t)node->grid_x,
                                          (int16_t)node->grid_y,
                                          node->lod,
                                          cell->epoch);

    uint32_t old_blk = 0u;
    uint16_t morton  = 0u;
    uint8_t  lod     = 0u;
    uint8_t  flags   = 0u;
    float    min_e   = 0.0f;
    float    max_e   = 0.0f;
    gpumath_tile_header_unpack(node->header, &old_blk, &morton, &lod, &flags, &min_e, &max_e);
    (void)old_blk;
    flags |= k_tile_flag_resident;
    if (cell->slot_index == 0u) {
        flags |= k_tile_flag_pinned;
    }
    node->header   = gpumath_tile_header_pack((uint32_t)gpu_block_4k, morton, lod, flags, min_e, max_e);
    node->gpu_slot = (uint16_t)cell->slot_index;
    node->resident = 1u;
    return 0;
}

static bool node_in_needed_list(uint32_t node_idx, const uint32_t *needed, uint32_t needed_count) {
    for (uint32_t i = 0u; i < needed_count; ++i) {
        if (needed[i] == node_idx) {
            return true;
        }
    }
    return false;
}

static int allocate_gpu_tile_slot(GpuMathArchiveHeader *hdr,
                                  const uint32_t       *needed,
                                  uint32_t              needed_count) {
    // 1. Prefer an unoccupied GPU slot in [1 .. k_gmpack_gpu_slot_cap - 1]
    for (uint32_t s = 1u; s < k_gmpack_gpu_slot_cap; ++s) {
        if (g_backend.slot_to_node[s] == UINT32_MAX) {
            return (int)s;
        }
    }
    // 2. Evict the least-recently-used slot not in the current needed list
    uint32_t best_slot = 0u;
    uint32_t best_lru  = UINT32_MAX;
    for (uint32_t s = 1u; s < k_gmpack_gpu_slot_cap; ++s) {
        uint32_t occupant = g_backend.slot_to_node[s];
        if (node_in_needed_list(occupant, needed, needed_count)) {
            continue;
        }
        if (g_backend.slot_lru_clock[s] < best_lru) {
            best_lru  = g_backend.slot_lru_clock[s];
            best_slot = s;
        }
    }
    if (best_slot == 0u) {
        return -1;
    }
    uint32_t evicted_idx = g_backend.slot_to_node[best_slot];
    if (evicted_idx < k_gmpack_node_count) {
        GpuMathQuadNode *ev = &hdr->nodes[evicted_idx];
        uint32_t blk   = 0u;
        uint16_t mrt   = 0u;
        uint8_t  lod   = 0u;
        uint8_t  flags = 0u;
        float    min_e = 0.0f;
        float    max_e = 0.0f;
        gpumath_tile_header_unpack(ev->header, &blk, &mrt, &lod, &flags, &min_e, &max_e);
        flags &= (uint8_t)~(k_tile_flag_resident | k_tile_flag_filtered);
        ev->header          = gpumath_tile_header_pack(0u, mrt, lod, flags, min_e, max_e);
        ev->rel_gpu_payload = k_relptr2d_null;
        ev->gpu_slot        = 0u;
        ev->resident        = 0u;
        if (hdr->dir.resident_tile_count > 1u) {
            hdr->dir.resident_tile_count--;
        }
    }
    g_backend.slot_to_node[best_slot] = UINT32_MAX;
    return (int)best_slot;
}

int backend_stream_page_ssd_tiles(const char           *pack_path,
                                  GpuMathArchiveHeader *hdr,
                                  GpuMathTileBlock     *gpu_tile_pool,
                                  const uint32_t       *needed_nodes,
                                  uint32_t              needed_count,
                                  uint64_t             *out_dirty_slots_mask) {
    if (pack_path == nullptr || hdr == nullptr || gpu_tile_pool == nullptr ||
        needed_nodes == nullptr || out_dirty_slots_mask == nullptr) {
        return -1;
    }
    *out_dirty_slots_mask = 0ull;

    int fd = -1;
    uint32_t cell_cursor = 0u;
    uint32_t paged_in = 0u;

    for (uint32_t i = 0u; i < needed_count; ++i) {
        uint32_t node_idx = needed_nodes[i];
        if (node_idx >= k_gmpack_node_count) {
            continue;
        }
        GpuMathQuadNode *node = &hdr->nodes[node_idx];
        if (node->resident != 0u && node->gpu_slot < k_gmpack_gpu_slot_cap &&
            g_backend.slot_to_node[node->gpu_slot] == node_idx) {
            g_backend.slot_lru_clock[node->gpu_slot] = ++g_backend.lru_tick;
            continue;
        }

        int slot = (node_idx == 0u) ? 0 : allocate_gpu_tile_slot(hdr, needed_nodes, needed_count);
        if (slot < 0) {
            continue;
        }
        if (fd < 0) {
            fd = open(pack_path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return -1;
            }
        }

        GpuMathStreamCell *cell = &g_backend.cells[cell_cursor % k_backend_cell_ring_cap];
        cell_cursor++;

        // Re-tag the node's SSD RelPtr2D with the cell's current lifetime epoch
        int32_t ssd_blk = relptr2d_block_delta(node->rel_ssd_payload);
        RelPtr2D rel_ssd = relptr2d_pack(ssd_blk, (int16_t)node->grid_x, (int16_t)node->grid_y,
                                         node->lod, cell->epoch);
        uint64_t ssd_off = relptr2d_resolve_addr(0ull, rel_ssd);
        uint64_t gpu_dst = g_backend.bda_buf.address != 0ull
            ? (g_backend.bda_buf.address + k_gmpack_header_bytes + (uint64_t)slot * k_gmpack_tile_bytes)
            : 0ull;

        if (cell64_arm(cell, ssd_tile_cell_exec, rel_ssd, gpu_dst, ssd_off,
                       node_idx, (uint32_t)slot, node->lod) != 0) {
            continue;
        }

        PagerExecCtx exec_ctx = {
            .fd            = fd,
            .hdr           = hdr,
            .gpu_tile_pool = gpu_tile_pool,
        };
        if (cell64_invoke(cell, &exec_ctx) == 0) {
            g_backend.slot_to_node[slot]   = node_idx;
            g_backend.slot_lru_clock[slot] = ++g_backend.lru_tick;
            *out_dirty_slots_mask |= (1ull << (uint32_t)slot);
            hdr->dir.stream_loads_total++;
            paged_in++;
        }
        cell64_poison_and_reincarnate(cell);
    }

    if (fd >= 0) {
        close(fd);
    }

    uint32_t res_cnt = 0u;
    for (uint32_t s = 0u; s < k_gmpack_gpu_slot_cap; ++s) {
        if (g_backend.slot_to_node[s] != UINT32_MAX) {
            res_cnt++;
        }
    }
    hdr->dir.resident_tile_count = res_cnt;

    if (paged_in > 0u) {
        printf("[Backend Stream] Paged %u tile(s) from SSD via RelPtr2D + 64B Cells (active leaves=%u, deepest LOD=%u, resident=%u/%u)\n",
               paged_in, hdr->dir.active_leaf_count, hdr->dir.deepest_lod_active,
               hdr->dir.resident_tile_count, k_gmpack_gpu_slot_cap);
        fflush(stdout);
    }
    return 0;
}
