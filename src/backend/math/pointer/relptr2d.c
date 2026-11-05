#include "backend/math/relptr2d.h"
#include <math.h>

// ============================================================================
// 2D Self-Relative Spatial Block Pointer (RelPtr2D) & Lilliput Header Math
// ============================================================================

RelPtr2D relptr2d_pack(int32_t block_delta_4k, int16_t dx, int16_t dy,
                       uint8_t lod, uint8_t epoch) {
    // Clamp signed 20-bit block delta [-524288, +524287]
    if (block_delta_4k < -524288) {
        block_delta_4k = -524288;
    } else if (block_delta_4k > 524287) {
        block_delta_4k = 524287;
    }
    uint64_t u_delta = (uint64_t)((uint32_t)block_delta_4k & 0xFFFFFu);
    uint64_t u_dx    = (uint64_t)(uint16_t)dx;
    uint64_t u_dy    = (uint64_t)(uint16_t)dy;
    uint64_t u_lod   = (uint64_t)(lod & 0x0Fu);
    uint64_t u_tag   = (uint64_t)(epoch ^ (uint8_t)k_relptr2d_seal_magic);

    return (u_tag << 56u) | (u_lod << 52u) | (u_delta << 32u) | (u_dx << 16u) | u_dy;
}

int32_t relptr2d_block_delta(RelPtr2D ptr) {
    uint32_t raw20 = (uint32_t)((ptr >> 32u) & 0xFFFFFu);
    if ((raw20 & 0x80000u) != 0u) {
        raw20 |= 0xFFF00000u; // Sign-extend 20-bit two's complement
    }
    return (int32_t)raw20;
}

uint64_t relptr2d_resolve_addr(uint64_t self_base_addr, RelPtr2D ptr) {
    if (ptr == k_relptr2d_null) {
        return 0ull;
    }
    int64_t byte_delta = (int64_t)relptr2d_block_delta(ptr) << k_relptr2d_block_shift;
    return (uint64_t)((int64_t)self_base_addr + byte_delta);
}

bool relptr2d_verify(RelPtr2D ptr, uint8_t expected_epoch) {
    if (ptr == k_relptr2d_null) {
        return false;
    }
    uint8_t tag = (uint8_t)((ptr >> 56u) & 0xFFu);
    return (uint8_t)(tag ^ (uint8_t)k_relptr2d_seal_magic) == expected_epoch;
}

void relptr2d_grid_xy(RelPtr2D ptr, int16_t *out_dx, int16_t *out_dy, uint8_t *out_lod) {
    if (out_dx != nullptr) {
        *out_dx = (int16_t)((ptr >> 16u) & 0xFFFFu);
    }
    if (out_dy != nullptr) {
        *out_dy = (int16_t)(ptr & 0xFFFFu);
    }
    if (out_lod != nullptr) {
        *out_lod = (uint8_t)((ptr >> 52u) & 0x0Fu);
    }
}

static uint16_t part1by1_u8(uint8_t n) {
    uint16_t v = (uint16_t)n;
    v = (uint16_t)((v | (v << 4u)) & 0x0F0Fu);
    v = (uint16_t)((v | (v << 2u)) & 0x3333u);
    v = (uint16_t)((v | (v << 1u)) & 0x5555u);
    return v;
}

static uint8_t compact1by1_u16(uint16_t v) {
    v &= 0x5555u;
    v = (uint16_t)((v | (v >> 1u)) & 0x3333u);
    v = (uint16_t)((v | (v >> 2u)) & 0x0F0Fu);
    v = (uint16_t)((v | (v >> 4u)) & 0x00FFu);
    return (uint8_t)v;
}

uint16_t relptr2d_morton2d_encode(uint8_t x, uint8_t y) {
    return (uint16_t)(part1by1_u8(x) | (uint16_t)(part1by1_u8(y) << 1u));
}

void relptr2d_morton2d_decode(uint16_t morton, uint8_t *out_x, uint8_t *out_y) {
    if (out_x != nullptr) {
        *out_x = compact1by1_u16(morton);
    }
    if (out_y != nullptr) {
        *out_y = compact1by1_u16((uint16_t)(morton >> 1u));
    }
}

GpuMathTileHeader gpumath_tile_header_pack(uint32_t block_idx_4k,
                                           uint16_t morton_z,
                                           uint8_t  lod,
                                           uint8_t  flags,
                                           float    min_elev_m,
                                           float    max_elev_m) {
    float qmin_f = fminf(fmaxf(min_elev_m * 0.5f, 0.0f), 1023.0f);
    float qmax_f = fminf(fmaxf(max_elev_m * 0.5f, 0.0f), 1023.0f);
    uint64_t qmin = (uint64_t)((uint32_t)lrintf(qmin_f) & 0x3FFu);
    uint64_t qmax = (uint64_t)((uint32_t)lrintf(qmax_f) & 0x3FFu);
    uint64_t blk  = (uint64_t)(block_idx_4k & 0xFFFFFu);
    uint64_t mrt  = (uint64_t)morton_z;
    uint64_t l    = (uint64_t)(lod & 0x0Fu);
    uint64_t f    = (uint64_t)(flags & 0x0Fu);

    return (blk << 44u) | (mrt << 28u) | (l << 24u) | (f << 20u) | (qmax << 10u) | qmin;
}

void gpumath_tile_header_unpack(GpuMathTileHeader hdr,
                                uint32_t *out_block_idx_4k,
                                uint16_t *out_morton_z,
                                uint8_t  *out_lod,
                                uint8_t  *out_flags,
                                float    *out_min_elev_m,
                                float    *out_max_elev_m) {
    if (out_block_idx_4k != nullptr) {
        *out_block_idx_4k = (uint32_t)((hdr >> 44u) & 0xFFFFFu);
    }
    if (out_morton_z != nullptr) {
        *out_morton_z = (uint16_t)((hdr >> 28u) & 0xFFFFu);
    }
    if (out_lod != nullptr) {
        *out_lod = (uint8_t)((hdr >> 24u) & 0x0Fu);
    }
    if (out_flags != nullptr) {
        *out_flags = (uint8_t)((hdr >> 20u) & 0x0Fu);
    }
    if (out_max_elev_m != nullptr) {
        *out_max_elev_m = (float)((hdr >> 10u) & 0x3FFu) * 2.0f;
    }
    if (out_min_elev_m != nullptr) {
        *out_min_elev_m = (float)(hdr & 0x3FFu) * 2.0f;
    }
}
