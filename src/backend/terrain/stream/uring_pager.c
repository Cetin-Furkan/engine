#include "backend/internal.h"
#include "ring/ring.h"
#include "wayland/wayland.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// 64-Byte Stateful Function Cell SSD -> GPU BDA Demand Pager (uring_pager.c)
// ============================================================================

static constexpr uint64_t k_pager_ud_base = 0x504147450000ULL; // "PAGE\0\0"

typedef struct PagerExecCtx {
    int                   fd;
    size_t                already_read;
    GpuMathArchiveHeader *hdr;
    GpuMathTileBlock     *gpu_tile_pool;
} PagerExecCtx;

static int ssd_tile_cell_exec(GpuMathStreamCell *cell, void *user_ctx) {
    PagerExecCtx *ctx = (PagerExecCtx *)user_ctx;
    if (cell == nullptr || ctx == nullptr ||
        cell->tile_id >= k_gmpack_node_count || cell->slot_index >= k_gmpack_gpu_slot_cap) {
        return -1;
    }

    uint32_t tid = cell->tile_id;
    // Fast path: if this tile was already fused with photogrammetric survey data,
    // copy the 384 KiB fused block directly from the in-RAM cache in < 0.04 ms!
    if (!g_backend.survey.persist_gmpack &&
        g_backend.node_fuse_state[tid] == 2u &&
        g_backend.fused_cache[tid] != nullptr) {
        memcpy(&ctx->gpu_tile_pool[cell->slot_index],
               g_backend.fused_cache[tid],
               sizeof(GpuMathTileBlock));
    } else {
        if (ctx->fd < 0) {
            return -1;
        }
        // Resolve the exact 4KB-aligned SSD file offset directly from the 2D Self-Relative Pointer
        uint64_t ssd_off = relptr2d_resolve_addr(0ull, cell->rel_tile);
        if (ssd_off < k_gmpack_header_bytes || ssd_off + k_gmpack_tile_bytes > k_gmpack_total_bytes) {
            return -1;
        }

        uint8_t *dst = (uint8_t *)&ctx->gpu_tile_pool[cell->slot_index];
        size_t done = ctx->already_read <= cell->bytes ? ctx->already_read : 0u;
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

        // Fuse active photogrammetric survey photos, MVS stereo depth, and geodetic GCPs once per tile
        if (!g_backend.survey.persist_gmpack && g_backend.node_fuse_state[tid] == 0u) {
            int changed = backend_survey_fuse_tile(&g_backend.survey,
                                                   ctx->hdr,
                                                   tid,
                                                   &ctx->gpu_tile_pool[cell->slot_index],
                                                   &g_backend.survey.last_stats);
            if (changed > 0) {
                if (g_backend.fused_cache[tid] == nullptr) {
                    g_backend.fused_cache[tid] = (GpuMathTileBlock *)malloc(sizeof(GpuMathTileBlock));
                }
                if (g_backend.fused_cache[tid] != nullptr) {
                    memcpy(g_backend.fused_cache[tid],
                           &ctx->gpu_tile_pool[cell->slot_index],
                           sizeof(GpuMathTileBlock));
                    g_backend.node_fuse_state[tid] = 2u;
                } else {
                    g_backend.node_fuse_state[tid] = 1u;
                }
            } else {
                g_backend.node_fuse_state[tid] = 1u;
            }
        }
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
                                  uint32_t              node_idx,
                                  const uint32_t       *needed,
                                  uint32_t              needed_count) {
    // Direct 1:1 non-evicting slot mapping when the BDA arena holds all quadtree nodes
    if (node_idx < k_gmpack_gpu_slot_cap &&
        (g_backend.slot_to_node[node_idx] == UINT32_MAX || g_backend.slot_to_node[node_idx] == node_idx)) {
        return (int)node_idx;
    }
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

static void submit_uring_tile_batch(RingContext       *ring,
                                    bool               fixed_file,
                                    GpuMathStreamCell *cells,
                                    uint32_t           batch_count,
                                    GpuMathTileBlock  *gpu_tile_pool,
                                    size_t            *out_bytes_read,
                                    uint32_t          *out_uring_turns) {
    if (ring == nullptr || !ring->is_initialized || !fixed_file || batch_count == 0u) {
        return;
    }

    uint32_t held   = ring->sq_pending;
    uint32_t queued = 0u;
    for (uint32_t b = 0u; b < batch_count; ++b) {
        GpuMathStreamCell *cell = &cells[b];
        if (!g_backend.survey.persist_gmpack &&
            cell->tile_id < k_gmpack_node_count &&
            g_backend.node_fuse_state[cell->tile_id] == 2u &&
            g_backend.fused_cache[cell->tile_id] != nullptr) {
            out_bytes_read[b] = cell->bytes;
            continue;
        }
        struct io_uring_sqe *sqe = ring_sqe(ring);
        if (sqe == nullptr) {
            ring_sqe_unget(ring, queued);
            ring->sq_pending = held;
            return;
        }
        sqe->opcode    = IORING_OP_READ;
        sqe->flags     = IOSQE_FIXED_FILE;
        sqe->fd        = ENG_SCRATCH_FD_SLOT;
        sqe->addr      = (uint64_t)(uintptr_t)&gpu_tile_pool[cell->slot_index];
        sqe->len       = cell->bytes;
        sqe->off       = cell->ssd_file_offset;
        sqe->user_data = k_pager_ud_base + (uint64_t)b;
        queued++;
    }
    if (queued == 0u) {
        return;
    }

    unsigned to_submit = ring_submit(ring);
    if (to_submit == 0u) {
        return;
    }
    (*out_uring_turns)++;

    WaylandContext *wl = wayland_get_context();
    uint32_t remaining = queued;
    uint64_t deadline  = ring_mono_ns() + 2000000000ull;

    int rc = ring_enter(ring, to_submit, remaining, 250);
    (void)rc;

    while (remaining > 0u) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data >= k_pager_ud_base &&
                cqe.user_data < k_pager_ud_base + (uint64_t)batch_count) {
                uint32_t idx = (uint32_t)(cqe.user_data - k_pager_ud_base);
                if (cqe.res > 0) {
                    out_bytes_read[idx] = (size_t)cqe.res;
                }
                if (remaining > 0u) {
                    remaining--;
                }
            } else if (wl != nullptr) {
                wayland_on_cqe(wl, &cqe);
            }
        }
        if (remaining == 0u || ring_mono_ns() > deadline) {
            break;
        }
        unsigned sub = ring_submit(ring);
        int wrc = ring_enter(ring, sub, 1u, 100);
        if (wrc < 0 && wrc != -ETIME && wrc != -EINTR) {
            break;
        }
    }
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

    int          fd          = g_backend.pack_fd;
    RingContext *ring        = ring_get_context();
    bool         fixed_ready = false;
    uint32_t     paged_in    = 0u;
    uint32_t     uring_turns = 0u;

    uint32_t i = 0u;
    while (i < needed_count) {
        uint32_t batch_count = 0u;
        size_t   batch_bytes_read[k_backend_cell_ring_cap] = {0};

        for (; i < needed_count && batch_count < k_backend_cell_ring_cap; ++i) {
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

            int slot = (node_idx == 0u) ? 0 : allocate_gpu_tile_slot(hdr, node_idx, needed_nodes, needed_count);
            if (slot < 0) {
                continue;
            }
            if (fd < 0) {
                fd = open(pack_path, O_RDONLY | O_CLOEXEC);
                if (fd < 0) {
                    return -1;
                }
                g_backend.pack_fd = fd;
            }
            if (!fixed_ready && ring != nullptr && ring->is_initialized) {
                fixed_ready = (ring_update_file(ring, ENG_SCRATCH_FD_SLOT, fd) == 0);
            }

            GpuMathStreamCell *cell = &g_backend.cells[batch_count];
            int32_t  ssd_blk = relptr2d_block_delta(node->rel_ssd_payload);
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
            // Reserve the slot immediately so subsequent tiles in the same batch do not collide
            g_backend.slot_to_node[slot]   = node_idx;
            g_backend.slot_lru_clock[slot] = ++g_backend.lru_tick;
            batch_count++;
        }

        if (batch_count == 0u) {
            break;
        }

        // Submit all armed cell reads in a single batched io_uring turn (with pread fallback)
        submit_uring_tile_batch(ring, fixed_ready, g_backend.cells, batch_count,
                                gpu_tile_pool, batch_bytes_read, &uring_turns);

        for (uint32_t b = 0u; b < batch_count; ++b) {
            GpuMathStreamCell *cell = &g_backend.cells[b];
            uint32_t           slot = cell->slot_index;
            uint32_t           nidx = cell->tile_id;
            PagerExecCtx exec_ctx = {
                .fd            = fd,
                .already_read  = batch_bytes_read[b],
                .hdr           = hdr,
                .gpu_tile_pool = gpu_tile_pool,
            };
            if (cell64_invoke(cell, &exec_ctx) == 0) {
                *out_dirty_slots_mask |= (1ull << (slot & 63u));
                if (slot < k_gmpack_gpu_slot_cap) {
                    g_backend.slot_dirty[slot] = 1u;
                    g_backend.dirty_slot_count++;
                }
                hdr->dir.stream_loads_total++;
                paged_in++;
            } else {
                g_backend.slot_to_node[slot] = UINT32_MAX;
                (void)nidx;
            }
            cell64_poison_and_reincarnate(cell);
        }
    }

    if (fixed_ready && ring != nullptr) {
        (void)ring_update_file(ring, ENG_SCRATCH_FD_SLOT, -1);
    }

    uint32_t res_cnt = 0u;
    for (uint32_t s = 0u; s < k_gmpack_gpu_slot_cap; ++s) {
        if (g_backend.slot_to_node[s] != UINT32_MAX) {
            res_cnt++;
        }
    }
    hdr->dir.resident_tile_count = res_cnt;

    if (paged_in > 0u && (hdr->dir.stream_loads_total <= k_gmpack_active_cut_cap || getenv("ENGINE_DEBUG_STREAM") != nullptr)) {
        printf("[Backend Stream] Paged %u tile(s) from SSD via RelPtr2D + 64B Cells (%s, turns=%u, active leaves=%u, deepest LOD=%u, resident=%u/%u)\n",
               paged_in,
               uring_turns > 0u ? "io_uring batch" : "RAM cache / pread",
               uring_turns > 0u ? uring_turns : paged_in,
               hdr->dir.active_leaf_count,
               hdr->dir.deepest_lod_active,
               hdr->dir.resident_tile_count,
               k_gmpack_gpu_slot_cap);
        fflush(stdout);
    }
    return 0;
}
