#include "backend/math/cell64.h"
#include "backend/terrain/pack.h"

// ============================================================================
// 64-Byte Stateful Function Cell Dispatcher (Single-Cache-Line Flat Closure)
// ============================================================================

void cell64_init(GpuMathStreamCell *cell, uint8_t initial_epoch) {
    if (cell == nullptr) {
        return;
    }
    uint8_t ep = initial_epoch == 0u ? 1u : initial_epoch;
    *cell = (GpuMathStreamCell){
        .fn              = nullptr,
        .rel_tile        = k_relptr2d_null,
        .gpu_bda_dst     = 0ull,
        .ssd_file_offset = 0ull,
        .tile_id         = 0u,
        .morton_key      = 0u,
        .bytes           = 0u,
        .slot_index      = 0u,
        .generation      = 1u,
        .lod             = 0u,
        .state           = k_cell_state_idle,
        .epoch           = ep,
        .inline_id       = ep,
        .user_tag        = 0ull,
    };
}

int cell64_arm(GpuMathStreamCell *cell,
               GpuMathCellFn      fn,
               RelPtr2D           rel_tile,
               uint64_t           gpu_bda_dst,
               uint64_t           ssd_file_offset,
               uint32_t           tile_id,
               uint32_t           slot_index,
               uint8_t            lod) {
    if (cell == nullptr || fn == nullptr) {
        return -1;
    }
    // Verify in-cache-line 1-byte epoch tag before arming
    if (cell->state == k_cell_state_poison || cell->epoch != cell->inline_id) {
        return -1;
    }
    int16_t gx = 0;
    int16_t gy = 0;
    relptr2d_grid_xy(rel_tile, &gx, &gy, nullptr);

    cell->fn              = fn;
    cell->rel_tile        = rel_tile;
    cell->gpu_bda_dst     = gpu_bda_dst;
    cell->ssd_file_offset = ssd_file_offset;
    cell->tile_id         = tile_id;
    cell->morton_key      = (uint32_t)relptr2d_morton2d_encode((uint8_t)gx, (uint8_t)gy);
    cell->bytes           = k_epack_tile_bytes;
    cell->slot_index      = slot_index;
    cell->lod             = lod;
    cell->state           = k_cell_state_armed;
    cell->user_tag        = ((uint64_t)cell->generation << 32u) | (uint64_t)tile_id;
    return 0;
}

int cell64_invoke(GpuMathStreamCell *cell, void *ctx) {
    if (cell == nullptr || cell->fn == nullptr || cell->state != k_cell_state_armed) {
        return -1;
    }
    // Temporal generation check: inline_id in the same 64B cache line must match epoch
    if (cell->epoch != cell->inline_id || !relptr2d_verify(cell->rel_tile, cell->epoch)) {
        cell->state = k_cell_state_poison;
        return -1;
    }
    int rc = cell->fn(cell, ctx);
    if (rc == 0) {
        cell->state = k_cell_state_ready;
    } else {
        cell->state = k_cell_state_poison;
    }
    return rc;
}

void cell64_poison_and_reincarnate(GpuMathStreamCell *cell) {
    if (cell == nullptr) {
        return;
    }
    // Increment the 1-byte inline lifetime ID in O(1) to invalidate stale RelPtr2Ds
    uint8_t next_epoch = (uint8_t)(cell->inline_id + 1u);
    if (next_epoch == 0u || next_epoch == 0xFFu) {
        next_epoch = 1u;
    }
    cell->fn        = nullptr;
    cell->rel_tile  = k_relptr2d_null;
    cell->state     = k_cell_state_idle;
    cell->epoch     = next_epoch;
    cell->inline_id = next_epoch;
    cell->generation++;
    if (cell->generation == 0u) {
        cell->generation = 1u;
    }
}
