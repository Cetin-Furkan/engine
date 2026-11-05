#ifndef ENGINE_BACKEND_MATH_RELPTR2D_H
#define ENGINE_BACKEND_MATH_RELPTR2D_H

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Next-Generation 2D Self-Relative Spatial Block Pointers (RelPtr2D)
// & Project Lilliput-Style 64-Bit Unified Block Headers (GpuMathTileHeader)
// ============================================================================
//
// RelPtr2D Layout (64-bit):
//   [63:56] 8-bit Epoch / Integrity Seal Tag (epoch ^ 0xA5)
//   [55:52] 4-bit Quadtree LOD Level (0..15)
//   [51:32] 20-bit Signed 4KB Block Offset Delta (-524,288 .. +524,287 blocks = +/- 2 GiB)
//   [31:16] 16-bit Signed 2D Grid X Coordinate / Delta (Q8.8 or integer grid units)
//   [15:0]  16-bit Signed 2D Grid Y Coordinate / Delta (Q8.8 or integer grid units)

typedef uint64_t RelPtr2D;
typedef uint64_t GpuMathTileHeader;

enum : uint64_t {
    k_relptr2d_null       = 0ull,
    k_relptr2d_seal_magic = 0xA5ull,
    k_relptr2d_block_shift = 12ull, // 4096-byte (2^12) block alignment
    k_relptr2d_block_bytes = 4096ull,
};

enum : uint32_t {
    k_tile_flag_resident = 1u << 0,
    k_tile_flag_filtered = 1u << 1,
    k_tile_flag_pinned   = 1u << 2,
    k_tile_flag_subdiv   = 1u << 3,
};

[[nodiscard]] RelPtr2D relptr2d_pack(int32_t block_delta_4k, int16_t dx, int16_t dy,
                                     uint8_t lod, uint8_t epoch);
[[nodiscard]] int32_t  relptr2d_block_delta(RelPtr2D ptr);
[[nodiscard]] uint64_t relptr2d_resolve_addr(uint64_t self_base_addr, RelPtr2D ptr);
[[nodiscard]] bool     relptr2d_verify(RelPtr2D ptr, uint8_t expected_epoch);
void                   relptr2d_grid_xy(RelPtr2D ptr, int16_t *out_dx, int16_t *out_dy, uint8_t *out_lod);

[[nodiscard]] uint16_t relptr2d_morton2d_encode(uint8_t x, uint8_t y);
void                   relptr2d_morton2d_decode(uint16_t morton, uint8_t *out_x, uint8_t *out_y);

// Lilliput-Style 64-Bit Unified Tile Header Layout:
//   [63:44] 20-bit 4KB Block Index from Arena Base
//   [43:28] 16-bit 2D Morton Z-Order Key
//   [27:24] 4-bit Quadtree LOD (0..3)
//   [23:20] 4-bit Residency & Compute Flags
//   [19:10] 10-bit Quantized Max Elevation in meters (0..2047 m, 2m step)
//   [9:0]   10-bit Quantized Min Elevation in meters (0..2047 m, 2m step)
[[nodiscard]] GpuMathTileHeader gpumath_tile_header_pack(uint32_t block_idx_4k,
                                                         uint16_t morton_z,
                                                         uint8_t  lod,
                                                         uint8_t  flags,
                                                         float    min_elev_m,
                                                         float    max_elev_m);
void gpumath_tile_header_unpack(GpuMathTileHeader hdr,
                                uint32_t *out_block_idx_4k,
                                uint16_t *out_morton_z,
                                uint8_t  *out_lod,
                                uint8_t  *out_flags,
                                float    *out_min_elev_m,
                                float    *out_max_elev_m);

#endif /* ENGINE_BACKEND_MATH_RELPTR2D_H */
