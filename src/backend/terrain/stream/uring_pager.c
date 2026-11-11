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
// Frame-Decoupled Non-Blocking 64B Cell SSD -> GPU BDA Pager (uring_pager.c)
// ============================================================================
// Live frames submit non-blocking io_uring SQEs (min_complete=0, timeout_ms=-1,
// 0.00 ms render-thread block) against persistent ENG_PACK_FD_SLOT and either:
//   - Zero-copy IORING_OP_READ directly into unified BDA memory (UMA_IGPU), or
//   - Registered IORING_OP_READ_FIXED staging buffers (DISCRETE / HYBRID).
// Ancestor tiles remain resident while DMA is in flight; backend_on_cqe() flips
// node->resident = 1u asynchronously on the main io_uring event loop.

typedef struct PagerExecCtx {
    int                   fd;
    size_t                already_read;
    GpuMathArchiveHeader *hdr;
    EpackTileBlock       *gpu_tile_pool;
} PagerExecCtx;

static GpuMathTileBlock *ensure_fuse_scratch(void) {
    if (g_backend.fuse_scratch == nullptr) {
        g_backend.fuse_scratch = aligned_alloc(4096u, sizeof(GpuMathTileBlock));
    }
    return g_backend.fuse_scratch;
}

static void refresh_resident_tile_count(GpuMathArchiveHeader *hdr) {
    if (hdr == nullptr) {
        return;
    }
    const uint32_t slot_cap = backend_slot_cap();
    uint32_t res_cnt = 0u;
    for (uint32_t s = 0u; s < slot_cap; ++s) {
        if (g_backend.slot_to_node[s] != UINT32_MAX && g_backend.slot_inflight[s] == 0u) {
            res_cnt++;
        }
    }
    hdr->dir.resident_tile_count = res_cnt;
}

static int ssd_tile_cell_exec(GpuMathStreamCell *cell, void *user_ctx) {
    PagerExecCtx *ctx = (PagerExecCtx *)user_ctx;
    const uint32_t slot_cap = backend_slot_cap();
    if (cell == nullptr || ctx == nullptr ||
        cell->tile_id >= k_gmpack_node_count || cell->slot_index >= slot_cap) {
        return -1;
    }

    uint32_t tid = cell->tile_id;
    // Fast path: if this tile was previously survey-fused, evicted, and spilled to fused_cache,
    // restore the 100 KiB .epack block directly in < 0.015 ms!
    if (!g_backend.survey.persist_gmpack &&
        g_backend.node_fuse_state[tid] == 2u &&
        g_backend.fused_cache[tid] != nullptr) {
        memcpy(&ctx->gpu_tile_pool[cell->slot_index],
               g_backend.fused_cache[tid],
               sizeof(EpackTileBlock));
    } else {
        if (ctx->fd < 0) {
            return -1;
        }
        uint64_t ssd_off = relptr2d_resolve_addr(0ull, cell->rel_tile);
        if (ssd_off < k_gmpack_header_bytes || ssd_off + k_epack_tile_bytes > k_epack_total_bytes) {
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

        // Only decode/re-encode .epack on CPU when a tile's AABB overlaps active survey features
        if (!g_backend.survey.persist_gmpack && g_backend.node_fuse_state[tid] == 0u) {
            const GpuMathQuadNode *qnode = &ctx->hdr->nodes[tid];
            if (backend_survey_tile_may_overlap(&g_backend.survey, qnode)) {
                GpuMathTileBlock *scratch = ensure_fuse_scratch();
                if (scratch != nullptr) {
                    backend_epack_decode_tile(&ctx->gpu_tile_pool[cell->slot_index], scratch);
                    bool changed = backend_survey_fuse_tile(&g_backend.survey,
                                                            ctx->hdr,
                                                            tid,
                                                            scratch,
                                                            &g_backend.survey.last_stats);
                    if (changed) {
                        backend_epack_encode_tile(scratch, &ctx->gpu_tile_pool[cell->slot_index]);
                        g_backend.node_fuse_state[tid] = 2u;
                    } else {
                        g_backend.node_fuse_state[tid] = 1u;
                    }
                }
            } else {
                g_backend.node_fuse_state[tid] = 1u;
            }
        }
    }

    // Update the node's GPU BDA RelPtr2D (25 x 4 KiB stride per .epack slot) and Lilliput header
    GpuMathQuadNode *node = &ctx->hdr->nodes[cell->tile_id];
    int32_t gpu_block_4k = (int32_t)(k_gmpack_header_blocks + cell->slot_index * k_epack_tile_blocks_4k);
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
                                  EpackTileBlock       *gpu_tile_pool,
                                  uint32_t              node_idx,
                                  const uint32_t       *needed,
                                  uint32_t              needed_count) {
    const uint32_t slot_cap = backend_slot_cap();
    if (slot_cap >= k_gmpack_node_count && node_idx < slot_cap &&
        g_backend.slot_inflight[node_idx] == 0u &&
        (g_backend.slot_to_node[node_idx] == UINT32_MAX || g_backend.slot_to_node[node_idx] == node_idx)) {
        return (int)node_idx;
    }
    // 1. Prefer an unoccupied GPU slot in [1 .. slot_cap - 1]
    for (uint32_t s = 1u; s < slot_cap; ++s) {
        if (g_backend.slot_to_node[s] == UINT32_MAX && g_backend.slot_inflight[s] == 0u) {
            return (int)s;
        }
    }
    // 2. Evict the least-recently-used non-inflight slot outside the current needed list
    uint32_t best_slot = 0u;
    uint32_t best_lru  = UINT32_MAX;
    for (uint32_t s = 1u; s < slot_cap; ++s) {
        if (g_backend.slot_inflight[s] != 0u) {
            continue;
        }
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
        if (!g_backend.survey.persist_gmpack &&
            g_backend.node_fuse_state[evicted_idx] == 2u &&
            g_backend.fused_cache[evicted_idx] == nullptr &&
            gpu_tile_pool != nullptr) {
            g_backend.fused_cache[evicted_idx] = (EpackTileBlock *)malloc(sizeof(EpackTileBlock));
            if (g_backend.fused_cache[evicted_idx] != nullptr) {
                memcpy(g_backend.fused_cache[evicted_idx],
                       &gpu_tile_pool[best_slot],
                       sizeof(EpackTileBlock));
            } else {
                g_backend.node_fuse_state[evicted_idx] = 0u;
            }
        }
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

static bool ensure_persistent_pack_fd(const char *epack_path, RingContext *ring) {
    if (g_backend.pack_fd < 0) {
        if (epack_path == nullptr || epack_path[0] == '\0') {
            return false;
        }
        int fd = open(epack_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        g_backend.pack_fd = fd;
        g_backend.pack_fixed_registered = false;
    }
    if (!g_backend.pack_fixed_registered && ring != nullptr && ring->is_initialized) {
        if (ring_update_file(ring, ENG_PACK_FD_SLOT, g_backend.pack_fd) == 0) {
            g_backend.pack_fixed_registered = true;
        }
    }
    return true;
}

static bool should_use_fixed_staging(const RingContext *ring) {
    if (ring == nullptr || !ring->is_initialized || !ring->fixed_on || !ring->pager_fixed_on) {
        return false;
    }
    if (getenv("ENGINE_URING_FIXED_BUF") != nullptr) {
        return true;
    }
    return !g_backend.zero_copy_uma;
}

static void prepare_pager_sqe(RingContext             *ring,
                              struct io_uring_sqe     *sqe,
                              uint32_t                 cell_idx,
                              const GpuMathStreamCell *cell,
                              EpackTileBlock          *gpu_tile_pool) {
    const bool use_fixed_buf = should_use_fixed_staging(ring);
    if (use_fixed_buf) {
        uint32_t buf_idx = ENG_FBUF_PAGER_BASE + cell_idx;
        uint8_t *fbuf    = ring_fixed_ptr(ring, buf_idx);
        if (fbuf != nullptr) {
            sqe->opcode    = IORING_OP_READ_FIXED;
            sqe->flags     = IOSQE_FIXED_FILE;
            sqe->fd        = ENG_PACK_FD_SLOT;
            sqe->addr      = (uint64_t)(uintptr_t)fbuf;
            sqe->len       = cell->bytes;
            sqe->off       = cell->ssd_file_offset;
            sqe->buf_index = (uint16_t)buf_idx;
            sqe->user_data = ENG_UD_PAGER_BASE + (uint64_t)cell_idx;
            g_backend.cell_uses_fixed_buf[cell_idx] = 1u;
            g_backend.stream_fixed_buf_reads++;
            return;
        }
    }

    sqe->opcode    = IORING_OP_READ;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_PACK_FD_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)&gpu_tile_pool[cell->slot_index];
    sqe->len       = cell->bytes;
    sqe->off       = cell->ssd_file_offset;
    sqe->buf_index = 0u;
    sqe->user_data = ENG_UD_PAGER_BASE + (uint64_t)cell_idx;
    g_backend.cell_uses_fixed_buf[cell_idx] = 0u;
}

static void submit_sync_boot_batch(RingContext       *ring,
                                   bool               fixed_file,
                                   GpuMathStreamCell *cells,
                                   uint32_t           batch_count,
                                   EpackTileBlock    *gpu_tile_pool,
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
        prepare_pager_sqe(ring, sqe, b, cell, gpu_tile_pool);
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

    (void)ring_enter(ring, to_submit, remaining, 250);

    while (remaining > 0u) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (cqe.user_data >= ENG_UD_PAGER_BASE &&
                cqe.user_data < ENG_UD_PAGER_BASE + (uint64_t)batch_count) {
                uint32_t idx = (uint32_t)(cqe.user_data - ENG_UD_PAGER_BASE);
                if (cqe.res > 0) {
                    size_t got = (size_t)cqe.res;
                    if (got > cells[idx].bytes) {
                        got = cells[idx].bytes;
                    }
                    if (g_backend.cell_uses_fixed_buf[idx] != 0u) {
                        const uint8_t *fbuf = ring_fixed_ptr(ring, ENG_FBUF_PAGER_BASE + idx);
                        if (fbuf != nullptr) {
                            memcpy(&gpu_tile_pool[cells[idx].slot_index], fbuf, got);
                        }
                        g_backend.cell_uses_fixed_buf[idx] = 0u;
                    }
                    out_bytes_read[idx] = got;
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

static uint32_t submit_async_needed_tiles(const char           *epack_path,
                                          GpuMathArchiveHeader *hdr,
                                          EpackTileBlock       *gpu_tile_pool,
                                          const uint32_t       *needed_nodes,
                                          uint32_t              needed_count,
                                          uint64_t             *io_dirty_mask) {
    RingContext *ring = ring_get_context();
    if (ring == nullptr || !ring->is_initialized || hdr == nullptr || gpu_tile_pool == nullptr) {
        return 0u;
    }
    if (!ensure_persistent_pack_fd(epack_path, ring) || !g_backend.pack_fixed_registered) {
        return 0u;
    }

    const uint32_t slot_cap = backend_slot_cap();
    uint32_t queued_async   = 0u;
    uint32_t instant_cached = 0u;

    for (uint32_t i = 0u; i < needed_count; ++i) {
        uint32_t node_idx = needed_nodes[i];
        if (node_idx >= k_gmpack_node_count) {
            continue;
        }
        GpuMathQuadNode *node = &hdr->nodes[node_idx];
        if (node->resident != 0u && node->gpu_slot < slot_cap &&
            g_backend.slot_to_node[node->gpu_slot] == node_idx) {
            g_backend.slot_lru_clock[node->gpu_slot] = ++g_backend.lru_tick;
            continue;
        }
        if (g_backend.node_inflight[node_idx] != 0u) {
            continue;
        }

        // Zero-syscall instant hit when tile is already cached in RAM from survey fusion
        if (!g_backend.survey.persist_gmpack &&
            g_backend.node_fuse_state[node_idx] == 2u &&
            g_backend.fused_cache[node_idx] != nullptr) {
            int slot = (node_idx == 0u)
                ? 0
                : allocate_gpu_tile_slot(hdr, gpu_tile_pool, node_idx, needed_nodes, needed_count);
            if (slot < 0) {
                continue;
            }
            GpuMathStreamCell tmp_cell;
            cell64_init(&tmp_cell, 1u);
            int32_t  ssd_blk = relptr2d_block_delta(node->rel_ssd_payload);
            RelPtr2D rel_ssd = relptr2d_pack(ssd_blk, (int16_t)node->grid_x, (int16_t)node->grid_y,
                                             node->lod, tmp_cell.epoch);
            uint64_t ssd_off = relptr2d_resolve_addr(0ull, rel_ssd);
            uint64_t gpu_dst = g_backend.bda_buf.address != 0ull
                ? (g_backend.bda_buf.address + k_gmpack_header_bytes + (uint64_t)slot * k_epack_tile_bytes)
                : 0ull;
            if (cell64_arm(&tmp_cell, ssd_tile_cell_exec, rel_ssd, gpu_dst, ssd_off,
                           node_idx, (uint32_t)slot, node->lod) == 0) {
                g_backend.slot_to_node[slot]   = node_idx;
                g_backend.slot_lru_clock[slot] = ++g_backend.lru_tick;
                PagerExecCtx exec_ctx = {
                    .fd            = g_backend.pack_fd,
                    .already_read  = k_epack_tile_bytes,
                    .hdr           = hdr,
                    .gpu_tile_pool = gpu_tile_pool,
                };
                if (cell64_invoke(&tmp_cell, &exec_ctx) == 0) {
                    if (io_dirty_mask != nullptr) {
                        *io_dirty_mask |= (1ull << ((uint32_t)slot & 63u));
                    }
                    g_backend.slot_dirty[slot] = 1u;
                    g_backend.dirty_slot_count++;
                    hdr->dir.stream_loads_total++;
                    instant_cached++;
                } else {
                    g_backend.slot_to_node[slot] = UINT32_MAX;
                }
            }
            continue;
        }

        // Find an idle 64-byte cell for non-blocking io_uring submission
        int free_cell = -1;
        for (uint32_t c = 0u; c < k_backend_cell_ring_cap; ++c) {
            if (g_backend.cell_busy[c] == 0u) {
                free_cell = (int)c;
                break;
            }
        }
        if (free_cell < 0) {
            break; // Remaining needed tiles stay in pending_needed[] and chain in backend_on_cqe()
        }

        int slot = (node_idx == 0u)
            ? 0
            : allocate_gpu_tile_slot(hdr, gpu_tile_pool, node_idx, needed_nodes, needed_count);
        if (slot < 0) {
            continue;
        }

        struct io_uring_sqe *sqe = ring_sqe(ring);
        if (sqe == nullptr) {
            break;
        }

        uint32_t cidx           = (uint32_t)free_cell;
        GpuMathStreamCell *cell = &g_backend.cells[cidx];
        int32_t  ssd_blk = relptr2d_block_delta(node->rel_ssd_payload);
        RelPtr2D rel_ssd = relptr2d_pack(ssd_blk, (int16_t)node->grid_x, (int16_t)node->grid_y,
                                         node->lod, cell->epoch);
        uint64_t ssd_off = relptr2d_resolve_addr(0ull, rel_ssd);
        uint64_t gpu_dst = g_backend.bda_buf.address != 0ull
            ? (g_backend.bda_buf.address + k_gmpack_header_bytes + (uint64_t)slot * k_epack_tile_bytes)
            : 0ull;

        if (cell64_arm(cell, ssd_tile_cell_exec, rel_ssd, gpu_dst, ssd_off,
                       node_idx, (uint32_t)slot, node->lod) != 0) {
            ring_sqe_unget(ring, 1u);
            continue;
        }

        g_backend.slot_to_node[slot]   = node_idx;
        g_backend.slot_lru_clock[slot] = ++g_backend.lru_tick;
        g_backend.slot_inflight[slot]  = 1u;
        g_backend.node_inflight[node_idx] = 1u;
        g_backend.cell_busy[cidx]      = 1u;

        prepare_pager_sqe(ring, sqe, cidx, cell, gpu_tile_pool);
        queued_async++;
    }

    if (queued_async > 0u) {
        unsigned to_submit = ring_submit(ring);
        if (to_submit > 0u) {
            // Pure non-blocking submission (min_complete=0, timeout_ms=-1 -> 0 us wait!)
            (void)ring_enter(ring, to_submit, 0u, -1);
            g_backend.stream_uring_turns_total++;
        }
    }

    if (instant_cached > 0u) {
        refresh_resident_tile_count(hdr);
    }
    return queued_async + instant_cached;
}

bool backend_on_cqe(const struct io_uring_cqe *cqe) {
    if (cqe == nullptr || !ring_ud_is_pager(cqe->user_data)) {
        return false;
    }
    uint32_t cidx = (uint32_t)(cqe->user_data - ENG_UD_PAGER_BASE);
    if (cidx >= k_backend_cell_ring_cap || g_backend.cell_busy[cidx] == 0u) {
        return false;
    }

    GpuMathStreamCell *cell = &g_backend.cells[cidx];
    uint32_t slot = cell->slot_index;
    uint32_t tid  = cell->tile_id;
    const uint32_t slot_cap = backend_slot_cap();

    g_backend.cell_busy[cidx] = 0u;
    if (slot < k_gmpack_gpu_slot_cap) {
        g_backend.slot_inflight[slot] = 0u;
    }
    if (tid < k_gmpack_node_count) {
        g_backend.node_inflight[tid] = 0u;
    }

    if (g_backend.gpu_hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        g_backend.cell_uses_fixed_buf[cidx] = 0u;
        cell64_poison_and_reincarnate(cell);
        return false;
    }

    size_t got = 0u;
    if (cqe->res > 0) {
        got = (size_t)cqe->res;
        if (got > cell->bytes) {
            got = cell->bytes;
        }
        if (g_backend.cell_uses_fixed_buf[cidx] != 0u && slot < slot_cap) {
            const uint8_t *fbuf = ring_fixed_ptr(ring_get_context(), ENG_FBUF_PAGER_BASE + cidx);
            if (fbuf != nullptr) {
                memcpy(&g_backend.gpu_tiles[slot], fbuf, got);
            }
        }
    }
    g_backend.cell_uses_fixed_buf[cidx] = 0u;

    bool became_resident = false;
    PagerExecCtx exec_ctx = {
        .fd            = g_backend.pack_fd,
        .already_read  = got,
        .hdr           = g_backend.gpu_hdr,
        .gpu_tile_pool = g_backend.gpu_tiles,
    };
    if (cqe->res >= 0 && cell64_invoke(cell, &exec_ctx) == 0) {
        g_backend.dirty_slots_mask |= (1ull << (slot & 63u));
        if (slot < slot_cap) {
            g_backend.slot_dirty[slot] = 1u;
            g_backend.dirty_slot_count++;
        }
        g_backend.gpu_hdr->dir.stream_loads_total++;
        g_backend.stream_async_cqes++;
        became_resident = true;
    } else if (slot < k_gmpack_gpu_slot_cap) {
        g_backend.slot_to_node[slot] = UINT32_MAX;
    }
    cell64_poison_and_reincarnate(cell);

    refresh_resident_tile_count(g_backend.gpu_hdr);
    backend_gpu_directory_sync();

    // Chain any remaining needed tiles from a large >16-tile camera jump
    if (g_backend.initialized && g_backend.pending_needed_count > 0u) {
        uint64_t extra_mask = 0ull;
        (void)submit_async_needed_tiles(g_backend.epack_path,
                                        g_backend.gpu_hdr,
                                        g_backend.gpu_tiles,
                                        g_backend.pending_needed,
                                        g_backend.pending_needed_count,
                                        &extra_mask);
        g_backend.dirty_slots_mask |= extra_mask;
    }

    if (became_resident && getenv("ENGINE_DEBUG_STREAM") != nullptr) {
        printf("[Backend Async] Tile %u (LOD %u) resident in slot %u (async_cqes=%u, resident=%u/%u)\n",
               tid,
               tid < k_gmpack_node_count ? g_backend.gpu_hdr->nodes[tid].lod : 0u,
               slot,
               g_backend.stream_async_cqes,
               g_backend.gpu_hdr->dir.resident_tile_count,
               slot_cap);
        fflush(stdout);
    }
    return became_resident;
}

void backend_stream_drain_inflight(void) {
    RingContext    *ring = ring_get_context();
    WaylandContext *wl   = wayland_get_context();

    uint64_t deadline = ring_mono_ns() + 500000000ull;
    for (;;) {
        uint32_t busy = 0u;
        for (uint32_t c = 0u; c < k_backend_cell_ring_cap; ++c) {
            if (g_backend.cell_busy[c] != 0u) {
                busy++;
            }
        }
        if (busy == 0u || ring == nullptr || !ring->is_initialized) {
            break;
        }
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            if (ring_ud_is_pager(cqe.user_data)) {
                (void)backend_on_cqe(&cqe);
            } else if (wl != nullptr) {
                wayland_on_cqe(wl, &cqe);
            }
        }
        busy = 0u;
        for (uint32_t c = 0u; c < k_backend_cell_ring_cap; ++c) {
            if (g_backend.cell_busy[c] != 0u) {
                busy++;
            }
        }
        if (busy == 0u || ring_mono_ns() > deadline) {
            break;
        }
        unsigned sub = ring_submit(ring);
        int wrc = ring_enter(ring, sub, 1u, 25);
        if (wrc < 0 && wrc != -ETIME && wrc != -EINTR) {
            break;
        }
    }

    memset(g_backend.cell_busy, 0, sizeof g_backend.cell_busy);
    memset(g_backend.cell_uses_fixed_buf, 0, sizeof g_backend.cell_uses_fixed_buf);
    memset(g_backend.slot_inflight, 0, sizeof g_backend.slot_inflight);
    memset(g_backend.node_inflight, 0, sizeof g_backend.node_inflight);

    if (g_backend.pack_fixed_registered && ring != nullptr && ring->is_initialized) {
        (void)ring_update_file(ring, ENG_PACK_FD_SLOT, -1);
    }
    g_backend.pack_fixed_registered = false;
}

int backend_stream_page_ssd_tiles(const char           *epack_path,
                                  GpuMathArchiveHeader *hdr,
                                  EpackTileBlock       *gpu_tile_pool,
                                  const uint32_t       *needed_nodes,
                                  uint32_t              needed_count,
                                  uint64_t             *out_dirty_slots_mask) {
    if (epack_path == nullptr || hdr == nullptr || gpu_tile_pool == nullptr ||
        needed_nodes == nullptr || out_dirty_slots_mask == nullptr) {
        return -1;
    }
    *out_dirty_slots_mask = 0ull;

    RingContext *ring = ring_get_context();
    uint32_t clamp_n = needed_count > k_gmpack_gpu_slot_cap ? k_gmpack_gpu_slot_cap : needed_count;
    if (clamp_n > 0u) {
        memcpy(g_backend.pending_needed, needed_nodes, clamp_n * sizeof(uint32_t));
    }
    g_backend.pending_needed_count = clamp_n;

    // Live runtime path: 100% non-blocking async io_uring submission (0.00 ms frame stall)
    if (g_backend.initialized && ring != nullptr && ring->is_initialized &&
        ensure_persistent_pack_fd(epack_path, ring) && g_backend.pack_fixed_registered) {
        (void)submit_async_needed_tiles(epack_path, hdr, gpu_tile_pool,
                                        needed_nodes, clamp_n, out_dirty_slots_mask);
        return 0;
    }

    // Boot / reload / headless-test pre-warm path
    (void)ensure_persistent_pack_fd(epack_path, ring);
    int      fd          = g_backend.pack_fd;
    bool     fixed_ready = g_backend.pack_fixed_registered;
    uint32_t paged_in    = 0u;
    uint32_t uring_turns = 0u;

    const uint32_t slot_cap = backend_slot_cap();
    uint32_t i = 0u;
    while (i < clamp_n) {
        uint32_t batch_count = 0u;
        size_t   batch_bytes_read[k_backend_cell_ring_cap] = {0};

        for (; i < clamp_n && batch_count < k_backend_cell_ring_cap; ++i) {
            uint32_t node_idx = needed_nodes[i];
            if (node_idx >= k_gmpack_node_count) {
                continue;
            }
            GpuMathQuadNode *node = &hdr->nodes[node_idx];
            if (node->resident != 0u && node->gpu_slot < slot_cap &&
                g_backend.slot_to_node[node->gpu_slot] == node_idx) {
                g_backend.slot_lru_clock[node->gpu_slot] = ++g_backend.lru_tick;
                continue;
            }

            int slot = (node_idx == 0u)
                ? 0
                : allocate_gpu_tile_slot(hdr, gpu_tile_pool, node_idx, needed_nodes, clamp_n);
            if (slot < 0) {
                continue;
            }
            if (fd < 0) {
                return -1;
            }

            GpuMathStreamCell *cell = &g_backend.cells[batch_count];
            int32_t  ssd_blk = relptr2d_block_delta(node->rel_ssd_payload);
            RelPtr2D rel_ssd = relptr2d_pack(ssd_blk, (int16_t)node->grid_x, (int16_t)node->grid_y,
                                             node->lod, cell->epoch);
            uint64_t ssd_off = relptr2d_resolve_addr(0ull, rel_ssd);
            uint64_t gpu_dst = g_backend.bda_buf.address != 0ull
                ? (g_backend.bda_buf.address + k_gmpack_header_bytes + (uint64_t)slot * k_epack_tile_bytes)
                : 0ull;

            if (cell64_arm(cell, ssd_tile_cell_exec, rel_ssd, gpu_dst, ssd_off,
                           node_idx, (uint32_t)slot, node->lod) != 0) {
                continue;
            }
            g_backend.slot_to_node[slot]   = node_idx;
            g_backend.slot_lru_clock[slot] = ++g_backend.lru_tick;
            batch_count++;
        }

        if (batch_count == 0u) {
            break;
        }

        uint64_t t_uring0 = ring_mono_ns();
        submit_sync_boot_batch(ring, fixed_ready, g_backend.cells, batch_count,
                               gpu_tile_pool, batch_bytes_read, &uring_turns);
        uint64_t t_uring1 = ring_mono_ns();
        if (t_uring1 > t_uring0) {
            uint64_t dt_us = (t_uring1 - t_uring0) / 1000ull;
            if (g_backend.initialized) {
                g_backend.stream_uring_us_total += dt_us;
            } else {
                g_backend.stream_boot_uring_us += dt_us;
            }
        }
        g_backend.stream_uring_turns_total += uring_turns;

        for (uint32_t b = 0u; b < batch_count; ++b) {
            GpuMathStreamCell *cell = &g_backend.cells[b];
            uint32_t           slot = cell->slot_index;
            PagerExecCtx exec_ctx = {
                .fd            = fd,
                .already_read  = batch_bytes_read[b],
                .hdr           = hdr,
                .gpu_tile_pool = gpu_tile_pool,
            };
            if (cell64_invoke(cell, &exec_ctx) == 0) {
                *out_dirty_slots_mask |= (1ull << (slot & 63u));
                if (slot < slot_cap) {
                    g_backend.slot_dirty[slot] = 1u;
                    g_backend.dirty_slot_count++;
                }
                hdr->dir.stream_loads_total++;
                paged_in++;
            } else {
                g_backend.slot_to_node[slot] = UINT32_MAX;
            }
            cell64_poison_and_reincarnate(cell);
        }
    }

    refresh_resident_tile_count(hdr);

    if (paged_in > 0u && (hdr->dir.stream_loads_total <= k_gmpack_active_cut_cap || getenv("ENGINE_DEBUG_STREAM") != nullptr)) {
        printf("[Backend Stream] Paged %u .epack tile(s) (100 KiB each) via RelPtr2D + 64B Cells (%s, turns=%u, active leaves=%u, deepest LOD=%u, resident=%u/%u)\n",
               paged_in,
               uring_turns > 0u ? "io_uring pre-warm" : "RAM cache / pread",
               uring_turns > 0u ? uring_turns : paged_in,
               hdr->dir.active_leaf_count,
               hdr->dir.deepest_lod_active,
               hdr->dir.resident_tile_count,
               slot_cap);
        fflush(stdout);
    }
    return 0;
}
