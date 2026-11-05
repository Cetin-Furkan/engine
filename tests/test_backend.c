#include "backend/backend.h"
#include "backend/math/cell64.h"
#include "backend/math/relptr2d.h"
#include "backend/terrain/pack.h"
#include "backend/terrain/stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_cell_hits = 0u;

static int test_cell_fn(GpuMathStreamCell *self, void *ctx) {
    if (self == nullptr || ctx == nullptr) {
        return -1;
    }
    uint32_t *accum = (uint32_t *)ctx;
    *accum += self->slot_index + (uint32_t)self->lod + (uint32_t)self->inline_id;
    s_cell_hits++;
    return 0;
}

int main(void) {
    // 1. Verify RelPtr2D 2D spatial block pointer encoding, seal verification, and resolution
    alignas(4096) uint8_t arena[4096u * 32u];
    memset(arena, 0, sizeof arena);
    uint64_t base_addr = (uint64_t)(uintptr_t)arena;

    RelPtr2D ptr = relptr2d_pack(12, 7, -3, 2u, 1u);
    if (!relptr2d_verify(ptr, 1u) || relptr2d_verify(ptr, 2u)) {
        fprintf(stderr, "FAIL: relptr2d_verify failed\n");
        return 1;
    }
    int16_t dx = 0;
    int16_t dy = 0;
    uint8_t lod = 0u;
    relptr2d_grid_xy(ptr, &dx, &dy, &lod);
    if (lod != 2u || dx != 7 || dy != -3 || relptr2d_block_delta(ptr) != 12) {
        fprintf(stderr, "FAIL: relptr2d bitfield extraction mismatch\n");
        return 1;
    }
    uint64_t resolved = relptr2d_resolve_addr(base_addr, ptr);
    if (resolved != base_addr + 12ull * 4096ull) {
        fprintf(stderr, "FAIL: relptr2d_resolve_addr mismatch\n");
        return 1;
    }

    // 2. Verify Lilliput 64-bit tile header & Morton Z-order packing
    uint16_t morton = relptr2d_morton2d_encode(5u, 6u);
    uint8_t mx = 0u;
    uint8_t my = 0u;
    relptr2d_morton2d_decode(morton, &mx, &my);
    if (mx != 5u || my != 6u) {
        fprintf(stderr, "FAIL: Morton 2D encode/decode mismatch\n");
        return 1;
    }
    GpuMathTileHeader hdr = gpumath_tile_header_pack(
        18u, morton, 3u,
        (uint8_t)(k_tile_flag_resident | k_tile_flag_filtered),
        823.7f, 1690.8f);
    uint32_t blk_idx = 0u;
    uint16_t unpacked_morton = 0u;
    uint8_t unpacked_lod = 0u;
    uint8_t unpacked_flags = 0u;
    float min_m = 0.0f;
    float max_m = 0.0f;
    gpumath_tile_header_unpack(hdr, &blk_idx, &unpacked_morton, &unpacked_lod,
                               &unpacked_flags, &min_m, &max_m);
    if (blk_idx != 18u || unpacked_morton != morton || unpacked_lod != 3u ||
        (unpacked_flags & k_tile_flag_resident) == 0u) {
        fprintf(stderr, "FAIL: Lilliput 64-bit tile header pack/extract mismatch\n");
        return 1;
    }
    if (min_m < 820.0f || min_m > 828.0f || max_m < 1686.0f || max_m > 1695.0f) {
        fprintf(stderr, "FAIL: Lilliput elevation range mismatch (%.1f .. %.1f)\n",
                (double)min_m, (double)max_m);
        return 1;
    }

    // 3. Verify 64-byte Stateful Function Cell (Cell64) + CHERI-D 1-byte inline_id
    GpuMathStreamCell cell;
    cell64_init(&cell, 0x5Au);
    RelPtr2D cell_ptr = relptr2d_pack(12, 7, -3, 2u, cell.epoch);
    if (cell64_arm(&cell, test_cell_fn, cell_ptr, base_addr, 4096ull * 2ull, 21u, 11u, 3u) != 0) {
        fprintf(stderr, "FAIL: cell64_arm failed\n");
        return 1;
    }
    uint32_t accum = 0u;
    if (cell64_invoke(&cell, &accum) != 0 || s_cell_hits != 1u ||
        accum != (11u + 3u + 0x5Au)) {
        fprintf(stderr, "FAIL: Cell64 invocation mismatch\n");
        return 1;
    }
    // Poison & reincarnate cell: old epoch must be invalidated, new epoch valid
    cell64_poison_and_reincarnate(&cell);
    if (cell.epoch == 0x5Au || cell.inline_id != cell.epoch) {
        fprintf(stderr, "FAIL: cell64_poison_and_reincarnate failed\n");
        return 1;
    }
    // Stale RelPtr2D with old epoch (0x5A) must be rejected by reincarnated cell!
    if (cell64_arm(&cell, test_cell_fn, cell_ptr, base_addr, 4096ull * 2ull, 21u, 11u, 3u) != 0) {
        fprintf(stderr, "FAIL: cell64_arm after reincarnate failed\n");
        return 1;
    }
    if (cell64_invoke(&cell, &accum) == 0) {
        fprintf(stderr, "FAIL: reincarnated Cell64 failed to reject stale RelPtr2D epoch\n");
        return 1;
    }
    // Re-init and verify inline_id tampering rejection
    cell64_init(&cell, 0x33u);
    RelPtr2D fresh_ptr = relptr2d_pack(12, 7, -3, 2u, cell.epoch);
    (void)cell64_arm(&cell, test_cell_fn, fresh_ptr, base_addr, 4096ull * 2ull, 21u, 11u, 3u);
    cell.inline_id ^= 0xFFu;
    if (cell64_invoke(&cell, &accum) == 0) {
        fprintf(stderr, "FAIL: Cell64 CHERI-D capability check failed to reject tampered cell\n");
        return 1;
    }

    // 4. Verify Baklan & Beşparmak Dağı SSD .gmpack archive integrity & dynamic zoom LOD selection
    const char *pack_path = "examples/demo/maps/baklan_besparmak.gmpack";
    if (backend_terrain_ensure_pack(pack_path) != 0) {
        fprintf(stderr, "FAIL: backend_terrain_ensure_pack failed\n");
        return 1;
    }
    static GpuMathArchiveHeader archive_hdr;
    if (backend_terrain_read_header(pack_path, &archive_hdr) != 0) {
        fprintf(stderr, "FAIL: backend_terrain_read_header failed\n");
        return 1;
    }
    if (archive_hdr.dir.magic != k_gmpack_magic ||
        archive_hdr.dir.node_count != k_gmpack_node_count) {
        fprintf(stderr, "FAIL: .gmpack directory header mismatch\n");
        return 1;
    }

    // Read root tile 0 from SSD
    static GpuMathTileBlock root_tile;
    if (backend_terrain_read_tile(pack_path, 0u, &root_tile) != 0) {
        fprintf(stderr, "FAIL: backend_terrain_read_tile(0) failed\n");
        return 1;
    }
    uint16_t max_u16 = 0u;
    uint16_t min_u16 = 0xFFFFu;
    for (uint32_t i = 0u; i < k_gmpack_tile_samples; ++i) {
        if (root_tile.height_u16[i] > max_u16) {
            max_u16 = root_tile.height_u16[i];
        }
        if (root_tile.height_u16[i] < min_u16) {
            min_u16 = root_tile.height_u16[i];
        }
    }
    float tile0_min_m = (float)min_u16 * 0.0625f;
    float tile0_max_m = (float)max_u16 * 0.0625f;
    if (tile0_max_m < 1500.0f || tile0_min_m < 680.0f) {
        fprintf(stderr, "FAIL: root_tile elevation bounds unexpected (%.1f .. %.1f)\n",
                (double)tile0_min_m, (double)tile0_max_m);
        return 1;
    }

    // Verify dynamic zoom-dependent quadtree LOD selection
    uint32_t needed_far[k_gmpack_gpu_slot_cap];
    uint32_t needed_far_n = 0u;
    (void)backend_stream_update_lod(&archive_hdr, 1595.0f, -103.0f, 0.01f,
                                    needed_far, &needed_far_n);
    uint32_t leaves_far = archive_hdr.dir.active_leaf_count;

    uint32_t needed_close[k_gmpack_gpu_slot_cap];
    uint32_t needed_close_n = 0u;
    (void)backend_stream_update_lod(&archive_hdr, 1595.0f, -103.0f, 1.20f,
                                    needed_close, &needed_close_n);
    uint32_t leaves_close = archive_hdr.dir.active_leaf_count;

    if (leaves_far == 0u || leaves_close <= leaves_far) {
        fprintf(stderr, "FAIL: expected zoom-in to subdivide quadtree leaves (far=%u, close=%u)\n",
                leaves_far, leaves_close);
        return 1;
    }

    printf("test_backend: ok (RelPtr2D + Lilliput64 + Cell64 + Baklan/Beşparmak SSD .gmpack %.1f..%.1f m, leaves %u->%u)\n",
           (double)tile0_min_m, (double)tile0_max_m, leaves_far, leaves_close);
    return 0;
}
