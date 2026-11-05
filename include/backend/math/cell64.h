#ifndef ENGINE_BACKEND_MATH_CELL64_H
#define ENGINE_BACKEND_MATH_CELL64_H

#include "backend/math/relptr2d.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// 64-Byte Stateful Function Cell (Flat Closure + CHERI-D Inline Lifetime ID)
// ============================================================================
// Co-locates the 8-byte execution function pointer and 56 bytes of captured
// streaming/compute state inside a single 64-byte L1 cache line (alignas(64)).
// Eliminates heap closure indirection and false sharing across streaming slots.

typedef struct GpuMathStreamCell GpuMathStreamCell;
typedef int (*GpuMathCellFn)(GpuMathStreamCell *cell, void *ctx);

enum : uint8_t {
    k_cell_state_idle   = 0u,
    k_cell_state_armed  = 1u,
    k_cell_state_ready  = 2u,
    k_cell_state_poison = 0xFFu,
};

struct GpuMathStreamCell {
    alignas(64) GpuMathCellFn fn;  // Bytes 0..7:   Code pointer (single-L1-line flat closure)
    RelPtr2D      rel_tile;        // Bytes 8..15:  2D Self-Relative pointer to tile block
    uint64_t      gpu_bda_dst;     // Bytes 16..23: Target Vulkan BDA address in resident pool
    uint64_t      ssd_file_offset; // Bytes 24..31: Exact byte offset in .gmpack archive
    uint32_t      tile_id;         // Bytes 32..35: Quadtree node index (0..84)
    uint32_t      morton_key;      // Bytes 36..39: 2D Morton Z-order key
    uint32_t      bytes;           // Bytes 40..43: Payload size in bytes (24,576 B)
    uint32_t      slot_index;      // Bytes 44..47: Resident GPU cache slot index
    uint32_t      generation;      // Bytes 48..51: Temporal safety generation counter
    uint8_t       lod;             // Byte  52:     Quadtree LOD level (0..3)
    uint8_t       state;           // Byte  53:     k_cell_state_*
    uint8_t       epoch;           // Byte  54:     Expected allocation lifetime ID
    uint8_t       inline_id;       // Byte  55:     CHERI-D in-cache-line lifetime ID tag
    uint64_t      user_tag;        // Bytes 56..63: Correlation / completion stamp
};

static_assert(sizeof(GpuMathStreamCell) == 64u, "GpuMathStreamCell must be 1 L1 cache line (64B)");
static_assert(alignof(GpuMathStreamCell) == 64u, "GpuMathStreamCell must be 64-byte aligned");

void              cell64_init(GpuMathStreamCell *cell, uint8_t initial_epoch);
[[nodiscard]] int cell64_arm(GpuMathStreamCell *cell,
                             GpuMathCellFn      fn,
                             RelPtr2D           rel_tile,
                             uint64_t           gpu_bda_dst,
                             uint64_t           ssd_file_offset,
                             uint32_t           tile_id,
                             uint32_t           slot_index,
                             uint8_t            lod);
[[nodiscard]] int cell64_invoke(GpuMathStreamCell *cell, void *ctx);
void              cell64_poison_and_reincarnate(GpuMathStreamCell *cell);

#endif /* ENGINE_BACKEND_MATH_CELL64_H */
