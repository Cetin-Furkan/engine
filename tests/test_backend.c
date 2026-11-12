#include "backend/backend.h"
#include "backend/math/cell64.h"
#include "backend/math/relptr2d.h"
#include "backend/terrain/pack.h"
#include "backend/terrain/stream.h"
#include "backend/terrain/survey.h"
#include "ring/ring.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    // 3. Verify 64-byte Host Closure (GpuMathStreamCell) + 1-byte inline_id epoch tag
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
        fprintf(stderr, "FAIL: Cell64 inline epoch check failed to reject tampered cell\n");
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
    if (tile0_max_m < 1580.0f || tile0_min_m < 680.0f || tile0_max_m > 1720.0f) {
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

    // Verify seamless LOD 4 coverage and bit-identical shared boundary posts/texels across the 37.9095N line
    // (between LOD 4 grid (7,11) and LOD 4 grid (7,12), which previously had a LOD 3/4 seam)
    uint32_t node_7_11 = backend_quadtree_node_index(4u, 7u, 11u);
    uint32_t node_7_12 = backend_quadtree_node_index(4u, 7u, 12u);
    static GpuMathTileBlock tile_7_11;
    static GpuMathTileBlock tile_7_12;
    if (node_7_11 == 0u || node_7_12 == 0u ||
        backend_terrain_read_tile(pack_path, node_7_11, &tile_7_11) != 0 ||
        backend_terrain_read_tile(pack_path, node_7_12, &tile_7_12) != 0) {
        fprintf(stderr, "FAIL: could not read adjacent LOD 4 tiles (7,11)=#%u and (7,12)=#%u\n",
                node_7_11, node_7_12);
        return 1;
    }
    // South border of (7,11) is sy=0; North border of (7,12) is sy=255
    for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
        uint16_t h_north = tile_7_11.height_u16[0u * k_gmpack_tile_dim + sx];
        uint16_t h_south = tile_7_12.height_u16[(k_gmpack_tile_dim - 1u) * k_gmpack_tile_dim + sx];
        uint32_t c_north = tile_7_11.imagery_rgba8[0u * k_gmpack_tile_dim + sx] & 0x00FFFFFFu;
        uint32_t c_south = tile_7_12.imagery_rgba8[(k_gmpack_tile_dim - 1u) * k_gmpack_tile_dim + sx] & 0x00FFFFFFu;
        if (h_north != h_south || c_north != c_south) {
            fprintf(stderr, "FAIL: boundary mismatch at sx=%u between LOD 4 (7,11) and (7,12): h=%u vs %u, rgb=0x%06x vs 0x%06x\n",
                    sx, (unsigned)h_north, (unsigned)h_south, c_north, c_south);
            return 1;
        }
    }

    // 5. Verify GPU/CPU indirect frustum culling & VkDrawIndirectCommand stream compaction
    static GpuMathIndirectBuffer indirect_all;
    TerrainCullParams cull_all = {
        .width          = 1280u,
        .height         = 720u,
        .elev_base_m    = k_besparmak_elev_min,
        .vertical_scale = 1.15f,
        .verts_per_tile = 64u * 64u * 6u,
        .qx             = 0.0f,
        .qy             = 0.0f,
        .qz             = 0.0f,
        .qw             = 1.0f,
        .cam_x          = 1595.0f,
        .cam_y          = -103.0f,
        .cam_z          = 0.0f,
        .zoom           = 0.01f,
    };
    (void)backend_stream_cull_frustum(&archive_hdr, &cull_all, &indirect_all);
    if (indirect_all.draw_count != leaves_close || indirect_all.culled_count != 0u) {
        fprintf(stderr, "FAIL: wide frustum should keep all %u leaves visible (got %u, culled %u)\n",
                leaves_close, indirect_all.draw_count, indirect_all.culled_count);
        return 1;
    }
    for (uint32_t i = 0u; i < indirect_all.draw_count; ++i) {
        if (indirect_all.cmds[i].vertex_count == 0u ||
            indirect_all.cmds[i].instance_count != 1u ||
            indirect_all.cmds[i].first_instance != i ||
            indirect_all.visible_nodes[i] >= k_gmpack_node_count) {
            fprintf(stderr, "FAIL: indirect command[%u] malformed\n", i);
            return 1;
        }
    }

    // Tight frustum zoomed into summit should cull peripheral tiles while keeping summit tiles
    static GpuMathIndirectBuffer indirect_tight;
    TerrainCullParams cull_tight = cull_all;
    cull_tight.zoom = 1.20f;
    (void)backend_stream_cull_frustum(&archive_hdr, &cull_tight, &indirect_tight);
    if (indirect_tight.draw_count == 0u ||
        indirect_tight.draw_count >= leaves_close ||
        indirect_tight.draw_count + indirect_tight.culled_count != leaves_close) {
        fprintf(stderr, "FAIL: tight frustum expected partial culling (visible=%u, culled=%u, total=%u)\n",
                indirect_tight.draw_count, indirect_tight.culled_count, leaves_close);
        return 1;
    }

    // Off-map frustum far outside the 24 km extent should cull all tiles
    static GpuMathIndirectBuffer indirect_offmap;
    TerrainCullParams cull_offmap = cull_all;
    cull_offmap.cam_x = 250000.0f;
    cull_offmap.cam_y = 250000.0f;
    cull_offmap.zoom  = 2.0f;
    (void)backend_stream_cull_frustum(&archive_hdr, &cull_offmap, &indirect_offmap);
    if (indirect_offmap.draw_count != 0u || indirect_offmap.culled_count != leaves_close) {
        fprintf(stderr, "FAIL: off-map frustum expected 0 visible leaves (got %u)\n",
                indirect_offmap.draw_count);
        return 1;
    }

    // 6. Verify Photogrammetric Sensor Presets, UTM/WGS84 <-> World Conversion & Brown-Conrady Camera Model
    TerrainSensorPreset iphone = {0};
    TerrainSensorPreset mavic  = {0};
    TerrainSensorPreset canon  = {0};
    TerrainSensorPreset bogus  = {0};
    if (!backend_survey_find_sensor_preset("iPhone 15 Pro Main", &iphone) ||
        !backend_survey_find_sensor_preset("dji mavic 3 enterprise", &mavic) ||
        !backend_survey_find_sensor_preset("Canon PowerShot S110", &canon) ||
        backend_survey_find_sensor_preset("NonExistentCamera999", &bogus) ||
        fabsf(iphone.focal_35mm_equiv - 24.0f) > 0.5f ||
        fabsf(mavic.sensor_width_mm - 17.3f) > 0.5f ||
        fabsf(canon.sensor_width_mm - 7.44f) > 0.05f ||
        fabsf(canon.default_focal_mm - 5.20f) > 0.05f) {
        fprintf(stderr, "FAIL: sensor preset lookup mismatch\n");
        return 1;
    }

    // Verify WGS84 UTM Zone 17N -> WGS84 (lat, lon) conversion
    double gcp2_lat = 0.0;
    double gcp2_lon = 0.0;
    if (!backend_survey_utm_to_wgs84(17, true, 441004.670, 4564119.587, &gcp2_lat, &gcp2_lon) ||
        fabs(gcp2_lat - 41.226316) > 1.0e-5 || fabs(gcp2_lon - (-81.703888)) > 1.0e-5) {
        fprintf(stderr, "FAIL: backend_survey_utm_to_wgs84 failed (got %.6f, %.6f)\n",
                gcp2_lat, gcp2_lon);
        return 1;
    }

    float wx_test = 0.0f;
    float wy_test = 0.0f;
    double lat_rt = 0.0;
    double lon_rt = 0.0;
    backend_survey_wgs84_to_world(&archive_hdr.dir, 37.981800, 29.707300, &wx_test, &wy_test);
    backend_survey_world_to_wgs84(&archive_hdr.dir, wx_test, wy_test, &lat_rt, &lon_rt);
    if (fabs(lat_rt - 37.981800) > 1.0e-5 || fabs(lon_rt - 29.707300) > 1.0e-5) {
        fprintf(stderr, "FAIL: WGS84 <-> world roundtrip error (%.6f, %.6f)\n", lat_rt, lon_rt);
        return 1;
    }

    TerrainCameraIntrinsics intr = {
        .width_px         = 4032u,
        .height_px        = 3024u,
        .sensor_width_mm  = iphone.sensor_width_mm,
        .sensor_height_mm = iphone.sensor_height_mm,
        .focal_mm         = iphone.default_focal_mm,
        .k1               = -0.065f,
        .k2               = 0.018f,
        .k3               = -0.003f,
        .p1               = 0.0008f,
        .p2               = -0.0005f,
    };
    if (!backend_survey_init_intrinsics(&intr)) {
        fprintf(stderr, "FAIL: backend_survey_init_intrinsics failed\n");
        return 1;
    }
    TerrainCameraPose pose = {
        .world_x        = 1380.0f,
        .world_y        = -310.0f,
        .altitude_msl_m = 2120.0f,
        .yaw_deg        = 42.0f,
        .pitch_deg      = -58.0f,
        .roll_deg       = 3.5f,
        .use_wgs84      = false,
    };
    backend_survey_init_pose(&archive_hdr.dir, &pose);

    // Check orthonormal camera basis
    float r_dot_u = pose.right[0] * pose.up[0] + pose.right[1] * pose.up[1] + pose.right[2] * pose.up[2];
    float r_dot_f = pose.right[0] * pose.forward[0] + pose.right[1] * pose.forward[1] + pose.right[2] * pose.forward[2];
    float u_dot_f = pose.up[0] * pose.forward[0] + pose.up[1] * pose.forward[1] + pose.up[2] * pose.forward[2];
    if (fabsf(r_dot_u) > 1.0e-4f || fabsf(r_dot_f) > 1.0e-4f || fabsf(u_dot_f) > 1.0e-4f) {
        fprintf(stderr, "FAIL: camera rotation basis not orthogonal (%.5f, %.5f, %.5f)\n",
                (double)r_dot_u, (double)r_dot_f, (double)u_dot_f);
        return 1;
    }

    // Project 3D target point with Brown-Conrady distortion and unproject ray back to target elevation
    float target_wx = 1595.0f;
    float target_wy = -103.0f;
    float target_wz = 1691.0f;
    float u_px = 0.0f;
    float v_px = 0.0f;
    float depth_m = 0.0f;
    if (!backend_survey_project_world_to_pixel(&intr, &pose,
                                               target_wx, target_wy, target_wz,
                                               &u_px, &v_px, &depth_m) ||
        depth_m <= 100.0f) {
        fprintf(stderr, "FAIL: backend_survey_project_world_to_pixel failed\n");
        return 1;
    }
    // Point behind camera must be rejected
    if (backend_survey_project_world_to_pixel(&intr, &pose,
                                              pose.world_x - pose.forward[0] * 50.0f,
                                              pose.world_y - pose.forward[1] * 50.0f,
                                              pose.altitude_msl_m - pose.forward[2] * 50.0f,
                                              nullptr, nullptr, nullptr)) {
        fprintf(stderr, "FAIL: point behind camera was not rejected\n");
        return 1;
    }

    float ray_orig[3] = {0};
    float ray_dir[3]  = {0};
    if (!backend_survey_pixel_to_world_ray(&intr, &pose, u_px, v_px, ray_orig, ray_dir)) {
        fprintf(stderr, "FAIL: backend_survey_pixel_to_world_ray failed\n");
        return 1;
    }
    float t_hit = (target_wz - ray_orig[2]) / ray_dir[2];
    float rec_wx = ray_orig[0] + ray_dir[0] * t_hit;
    float rec_wy = ray_orig[1] + ray_dir[1] * t_hit;
    if (fabsf(rec_wx - target_wx) > 0.02f || fabsf(rec_wy - target_wy) > 0.02f) {
        fprintf(stderr, "FAIL: Brown-Conrady project/undistort roundtrip error (got %.3f, %.3f vs %.3f, %.3f)\n",
                (double)rec_wx, (double)rec_wy, (double)target_wx, (double)target_wy);
        return 1;
    }

    // 7. Verify Multi-View 3D Ray Triangulation (3 overlapping views + parallel-ray rejection)
    TerrainCameraPose poses_3v[3] = {
        { .world_x = 1595.0f, .world_y = -103.0f, .altitude_msl_m = 2240.0f, .yaw_deg = 0.0f,   .pitch_deg = -90.0f, .roll_deg = 0.0f  },
        { .world_x = 1350.0f, .world_y = -320.0f, .altitude_msl_m = 2110.0f, .yaw_deg = 48.0f,  .pitch_deg = -58.0f, .roll_deg = 2.0f  },
        { .world_x = 1820.0f, .world_y = -340.0f, .altitude_msl_m = 2060.0f, .yaw_deg = -43.0f, .pitch_deg = -55.0f, .roll_deg = -1.5f },
    };
    for (uint32_t v = 0u; v < 3u; ++v) {
        backend_survey_init_pose(&archive_hdr.dir, &poses_3v[v]);
    }

    float true_pt[3] = { 1612.4f, -88.7f, 1704.6f };
    float origins_3v[3][3] = {0};
    float dirs_3v[3][3]    = {0};
    for (uint32_t v = 0u; v < 3u; ++v) {
        float pu = 0.0f;
        float pv = 0.0f;
        if (!backend_survey_project_world_to_pixel(&intr, &poses_3v[v],
                                                   true_pt[0], true_pt[1], true_pt[2],
                                                   &pu, &pv, nullptr)) {
            fprintf(stderr, "FAIL: 3-view projection failed for view %u\n", v);
            return 1;
        }
        (void)backend_survey_pixel_to_world_ray(&intr, &poses_3v[v], pu, pv,
                                                origins_3v[v], dirs_3v[v]);
    }
    float tri_pt[3] = {0};
    float tri_rmse  = 999.0f;
    float tri_par   = 0.0f;
    if (!backend_survey_triangulate_rays(3u, origins_3v, dirs_3v,
                                         tri_pt, &tri_rmse, &tri_par) ||
        fabsf(tri_pt[0] - true_pt[0]) > 0.02f ||
        fabsf(tri_pt[1] - true_pt[1]) > 0.02f ||
        fabsf(tri_pt[2] - true_pt[2]) > 0.02f ||
        tri_rmse > 0.02f || tri_par < 15.0f) {
        fprintf(stderr, "FAIL: 3-view triangulation mismatch (got %.3f, %.3f, %.3f rmse=%.4f par=%.1f)\n",
                (double)tri_pt[0], (double)tri_pt[1], (double)tri_pt[2],
                (double)tri_rmse, (double)tri_par);
        return 1;
    }
    // Edge case: parallel rays must be rejected
    float par_dirs[2][3] = {
        { 0.0f, 0.0f, -1.0f },
        { 0.0f, 0.0f, -1.0f },
    };
    if (backend_survey_triangulate_rays(2u, origins_3v, par_dirs,
                                        tri_pt, nullptr, nullptr)) {
        fprintf(stderr, "FAIL: parallel rays were not rejected by triangulator\n");
        return 1;
    }
    // Edge case: opposing 180-degree collinear rays and zero-baseline rays must be rejected
    float opp_origins[2][3] = {
        { 1595.0f, -103.0f, 2200.0f },
        { 1595.0f, -103.0f, 1200.0f },
    };
    float opp_dirs[2][3] = {
        { 0.0f, 0.0f, -1.0f },
        { 0.0f, 0.0f,  1.0f },
    };
    if (backend_survey_triangulate_rays(2u, opp_origins, opp_dirs,
                                        tri_pt, nullptr, nullptr)) {
        fprintf(stderr, "FAIL: opposing 180-deg collinear rays were not rejected\n");
        return 1;
    }
    float zero_base_origins[2][3] = {
        { 1595.0f, -103.0f, 2200.0f },
        { 1595.0f, -103.0f, 2200.0f },
    };
    if (backend_survey_triangulate_rays(2u, zero_base_origins, dirs_3v,
                                        tri_pt, nullptr, nullptr)) {
        fprintf(stderr, "FAIL: zero-baseline rays from identical origin were not rejected\n");
        return 1;
    }
    // Edge case: preset disambiguation ("iPhone 16 Pro UltraWide" -> 13mm, single-char "a" -> false)
    TerrainSensorPreset uw_preset = {0};
    if (!backend_survey_find_sensor_preset("iPhone 16 Pro UltraWide", &uw_preset) ||
        fabsf(uw_preset.focal_35mm_equiv - 13.0f) > 0.1f) {
        fprintf(stderr, "FAIL: 'iPhone 16 Pro UltraWide' did not resolve to 13mm UltraWide preset\n");
        return 1;
    }
    if (backend_survey_find_sensor_preset("a", &uw_preset)) {
        fprintf(stderr, "FAIL: single-character preset query 'a' should not fuzzy-match\n");
        return 1;
    }

    // 8. Verify PNG I/O, CSV Point Cloud Loading, Default Manifest Self-Healing & Stereo MVS Tile Fusion
    const char *tmp_png = "build/test_survey_pattern.png";
    uint32_t pat_w = 64u;
    uint32_t pat_h = 64u;
    uint32_t *pat_rgba = (uint32_t *)malloc((size_t)pat_w * pat_h * sizeof(uint32_t));
    if (pat_rgba == nullptr) {
        return 1;
    }
    for (uint32_t i = 0u; i < pat_w * pat_h; ++i) {
        uint8_t r = (uint8_t)((i * 17u) & 0xFFu);
        uint8_t g = (uint8_t)((i * 43u + 19u) & 0xFFu);
        uint8_t b = (uint8_t)((i * 97u + 73u) & 0xFFu);
        pat_rgba[i] = (uint32_t)r | ((uint32_t)g << 8u) | ((uint32_t)b << 16u) | 0xFF000000u;
    }
    if (backend_survey_save_png_file(tmp_png, pat_rgba, pat_w, pat_h) != 0) {
        fprintf(stderr, "FAIL: backend_survey_save_png_file failed\n");
        free(pat_rgba);
        return 1;
    }
    uint32_t *loaded_rgba = nullptr;
    uint32_t loaded_w = 0u;
    uint32_t loaded_h = 0u;
    if (backend_survey_load_image_file(tmp_png, &loaded_rgba, &loaded_w, &loaded_h) != 0 ||
        loaded_w != pat_w || loaded_h != pat_h ||
        memcmp(pat_rgba, loaded_rgba, (size_t)pat_w * pat_h * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "FAIL: PNG save/load roundtrip mismatch\n");
        free(pat_rgba);
        free(loaded_rgba);
        return 1;
    }
    free(pat_rgba);
    free(loaded_rgba);
    unlink(tmp_png);

    // Verify default survey manifest self-healing (clean satellite-preserving default without synthetic image overlays)
    const char *manifest_path = "examples/demo/maps/besparmak_survey.manifest";
    if (backend_survey_ensure_default_manifest(manifest_path, &archive_hdr.dir) != 0) {
        fprintf(stderr, "FAIL: backend_survey_ensure_default_manifest failed\n");
        return 1;
    }
    TerrainSurveyDataset ds;
    backend_survey_dataset_init(&ds);
    if (backend_survey_load_manifest(&ds, &archive_hdr.dir, manifest_path) != 0) {
        fprintf(stderr, "FAIL: backend_survey_load_manifest failed\n");
        backend_survey_dataset_free(&ds);
        return 1;
    }
    if (ds.photo_count != 0u) {
        fprintf(stderr, "FAIL: default manifest loaded unexpected photo count (%u)\n", ds.photo_count);
        backend_survey_dataset_free(&ds);
        return 1;
    }

    // Generate temporary synthetic stereo PNGs in build/ to test multi-view stereo + tiepoint + MVS fusion
    const char *tmp_nadir_png   = "build/test_calib_nadir.png";
    const char *tmp_oblique_png = "build/test_calib_oblique.png";
    float tie_u0 = 128.0f, tie_v0 = 128.0f, tie_u1 = 128.0f, tie_v1 = 128.0f;
    {
        uint32_t cw = 256u;
        uint32_t ch = 256u;
        uint32_t *nadir_rgba   = (uint32_t *)malloc((size_t)cw * ch * sizeof(uint32_t));
        uint32_t *oblique_rgba = (uint32_t *)malloc((size_t)cw * ch * sizeof(uint32_t));
        if (nadir_rgba == nullptr || oblique_rgba == nullptr) {
            free(nadir_rgba);
            free(oblique_rgba);
            backend_survey_dataset_free(&ds);
            return 1;
        }
        TerrainCameraIntrinsics intr0 = {
            .width_px         = cw,
            .height_px        = ch,
            .sensor_width_mm  = mavic.sensor_width_mm,
            .sensor_height_mm = mavic.sensor_height_mm,
            .focal_mm         = mavic.default_focal_mm,
            .k1               = mavic.k1,
            .k2               = mavic.k2,
        };
        (void)backend_survey_init_intrinsics(&intr0);
        TerrainCameraPose pose0 = {
            .world_x = 1595.0f, .world_y = -103.0f, .altitude_msl_m = 2240.0f,
            .yaw_deg = 0.0f, .pitch_deg = -90.0f, .roll_deg = 0.0f, .use_wgs84 = false,
        };
        backend_survey_init_pose(&archive_hdr.dir, &pose0);

        TerrainCameraIntrinsics intr1 = {
            .width_px         = cw,
            .height_px        = ch,
            .sensor_width_mm  = iphone.sensor_width_mm,
            .sensor_height_mm = iphone.sensor_height_mm,
            .focal_mm         = iphone.default_focal_mm,
            .k1               = iphone.k1,
            .k2               = iphone.k2,
        };
        (void)backend_survey_init_intrinsics(&intr1);
        TerrainCameraPose pose1 = {
            .world_x = 1380.0f, .world_y = -318.0f, .altitude_msl_m = 2115.0f,
            .yaw_deg = 45.0f, .pitch_deg = -60.6f, .roll_deg = 0.0f, .use_wgs84 = false,
        };
        backend_survey_init_pose(&archive_hdr.dir, &pose1);

        (void)backend_survey_project_world_to_pixel(&intr0, &pose0, 1595.0f, -103.0f, 1696.5f, &tie_u0, &tie_v0, nullptr);
        (void)backend_survey_project_world_to_pixel(&intr1, &pose1, 1595.0f, -103.0f, 1696.5f, &tie_u1, &tie_v1, nullptr);

        for (uint32_t py = 0u; py < ch; ++py) {
            for (uint32_t px = 0u; px < cw; ++px) {
                for (int view = 0; view < 2; ++view) {
                    const TerrainCameraIntrinsics *intr_v = (view == 0) ? &intr0 : &intr1;
                    const TerrainCameraPose       *pose_v = (view == 0) ? &pose0 : &pose1;
                    float orig[3] = {0};
                    float rdir[3] = {0};
                    float wx = 1595.0f;
                    float wy = -103.0f;
                    if (backend_survey_pixel_to_world_ray(intr_v, pose_v, (float)px, (float)py, orig, rdir) &&
                        fabsf(rdir[2]) > 1.0e-5f) {
                        float t = (1696.5f - orig[2]) / rdir[2];
                        wx = orig[0] + t * rdir[0];
                        wy = orig[1] + t * rdir[1];
                    }
                    float p1 = sinf(wx * 0.11f + cosf(wy * 0.09f));
                    float p2 = cosf(wy * 0.13f - sinf(wx * 0.07f));
                    float p3 = sinf((wx + wy) * 0.23f) * cosf((wx - wy) * 0.19f);
                    float lum = 0.52f + 0.22f * p1 + 0.16f * p2 + 0.10f * p3;
                    int r = (int)lrintf((110.0f + 115.0f * lum));
                    int g = (int)lrintf((104.0f + 110.0f * lum));
                    int b = (int)lrintf(( 94.0f + 100.0f * lum));
                    r = (r < 0) ? 0 : ((r > 255) ? 255 : r);
                    g = (g < 0) ? 0 : ((g > 255) ? 255 : g);
                    b = (b < 0) ? 0 : ((b > 255) ? 255 : b);
                    uint32_t rgba = (uint32_t)r | ((uint32_t)g << 8u) | ((uint32_t)b << 16u) | 0xFF000000u;
                    if (view == 0) {
                        nadir_rgba[py * cw + px] = rgba;
                    } else {
                        oblique_rgba[py * cw + px] = rgba;
                    }
                }
            }
        }
        (void)backend_survey_save_png_file(tmp_nadir_png, nadir_rgba, cw, ch);
        (void)backend_survey_save_png_file(tmp_oblique_png, oblique_rgba, cw, ch);
        free(nadir_rgba);
        free(oblique_rgba);
    }

    const char *tmp_stereo_manifest = "build/test_stereo_fusion.manifest";
    {
        FILE *smf = fopen(tmp_stereo_manifest, "w");
        if (smf == nullptr) {
            backend_survey_dataset_free(&ds);
            return 1;
        }
        fprintf(smf,
                "photo %s preset=\"DJI Mavic 3 Enterprise\" name=\"Mavic3E-Nadir\" "
                "wx=1595.0 wy=-103.0 alt=2240.0 yaw=0.0 pitch=-90.0 roll=0.0 weight=0.88 mvs=1\n"
                "photo %s preset=\"iPhone 15 Pro Main\" name=\"iPhone15Pro-Oblique\" "
                "wx=1380.0 wy=-318.0 alt=2115.0 yaw=45.0 pitch=-60.6 roll=0.0 weight=0.82 mvs=1\n"
                "tiepoint 90.0 0.96 0:%.5f:%.5f 1:%.5f:%.5f\n",
                tmp_nadir_png, tmp_oblique_png,
                (double)tie_u0, (double)tie_v0, (double)tie_u1, (double)tie_v1);
        fclose(smf);
    }
    if (backend_survey_load_manifest(&ds, &archive_hdr.dir, tmp_stereo_manifest) != 0) {
        fprintf(stderr, "FAIL: backend_survey_load_manifest failed on temporary stereo manifest\n");
        unlink(tmp_stereo_manifest);
        unlink(tmp_nadir_png);
        unlink(tmp_oblique_png);
        backend_survey_dataset_free(&ds);
        return 1;
    }
    unlink(tmp_stereo_manifest);
    if (ds.photo_count < 2u || ds.point_count < 1u ||
        ds.last_stats.tiepoints_triangulated < 1u) {
        fprintf(stderr, "FAIL: stereo manifest loaded unexpected counts (photos=%u, pts=%u, brk=%u, tie=%u)\n",
                ds.photo_count, ds.point_count, ds.breakline_count, ds.last_stats.tiepoints_triangulated);
        unlink(tmp_nadir_png);
        unlink(tmp_oblique_png);
        backend_survey_dataset_free(&ds);
        return 1;
    }
    // Verify idempotency of backend_survey_solve_tiepoints and rejection of duplicate-view tiepoints
    uint32_t pts_before_re_solve = ds.point_count;
    TerrainStereoTiePoint dup_tp = {
        .views = {
            { .photo_index = 0u, .u_px = 40.0f, .v_px = 40.0f },
            { .photo_index = 0u, .u_px = 80.0f, .v_px = 80.0f },
        },
        .view_count = 2u,
        .radius_m   = 60.0f,
        .weight     = 0.9f,
    };
    int dup_idx = backend_survey_add_tiepoint(&ds, &dup_tp);
    (void)backend_survey_solve_tiepoints(&ds, &archive_hdr.dir);
    if (ds.point_count != pts_before_re_solve ||
        (dup_idx >= 0 && ds.tiepoints[dup_idx].valid)) {
        fprintf(stderr, "FAIL: solve_tiepoints was not idempotent or accepted duplicate photo_index view\n");
        backend_survey_dataset_free(&ds);
        return 1;
    }
    if (!ds.tiepoints[0].valid ||
        fabsf(ds.tiepoints[0].triangulated_x - 1595.0f) > 0.05f ||
        fabsf(ds.tiepoints[0].triangulated_y - (-103.0f)) > 0.05f ||
        fabsf(ds.tiepoints[0].triangulated_elev_m - 1696.5f) > 0.05f) {
        fprintf(stderr, "FAIL: tiepoint[0] triangulation inaccurate (%.3f, %.3f, %.3f)\n",
                (double)ds.tiepoints[0].triangulated_x,
                (double)ds.tiepoints[0].triangulated_y,
                (double)ds.tiepoints[0].triangulated_elev_m);
        backend_survey_dataset_free(&ds);
        return 1;
    }

    // Verify CSV point cloud loading (both WGS84 lat/lon and metric world X/Y rows)
    const char *tmp_csv = "build/test_survey_points.csv";
    FILE *cf = fopen(tmp_csv, "w");
    if (cf == nullptr) {
        backend_survey_dataset_free(&ds);
        return 1;
    }
    fprintf(cf, "# lat_or_wx, lon_or_wy, elev_m, radius_m, weight\n");
    fprintf(cf, "37.981800, 29.707300, 1696.5, 110.0, 0.90\n");
    fprintf(cf, "1645.0, -78.0, 1664.0, 95.0, 0.85\n");
    fclose(cf);
    int csv_added = backend_survey_load_csv_points(&ds, &archive_hdr.dir, tmp_csv, 90.0f, 0.85f);
    unlink(tmp_csv);
    if (csv_added != 2) {
        fprintf(stderr, "FAIL: backend_survey_load_csv_points expected 2 points, got %d\n", csv_added);
        backend_survey_dataset_free(&ds);
        return 1;
    }

    // Fuse into root_tile (LOD 0) AND the finest LOD 4 summit leaf tile covering (1595.0, -103.0)
    static GpuMathTileBlock fused_tile;
    memcpy(&fused_tile, &root_tile, sizeof(GpuMathTileBlock));
    TerrainSurveyStats fuse_stats = {0};
    if (!backend_survey_fuse_tile(&ds, &archive_hdr, 0u, &fused_tile, &fuse_stats) ||
        fuse_stats.tiles_updated == 0u ||
        fuse_stats.dem_posts_refined == 0u ||
        fuse_stats.texels_orthorectified == 0u) {
        fprintf(stderr, "FAIL: backend_survey_fuse_tile failed to refine tile 0 (posts=%u, texels=%u)\n",
                fuse_stats.dem_posts_refined, fuse_stats.texels_orthorectified);
        backend_survey_dataset_free(&ds);
        return 1;
    }

    uint32_t summit_leaf = 0u;
    for (uint32_t idx = 0u; idx < k_gmpack_node_count; ++idx) {
        const GpuMathQuadNode *n = &archive_hdr.nodes[idx];
        if (n->lod == 4u &&
            1595.0f >= n->world_min_x && 1595.0f <= n->world_max_x &&
            -103.0f >= n->world_min_y && -103.0f <= n->world_max_y) {
            summit_leaf = idx;
            break;
        }
    }
    if (summit_leaf == 0u || backend_terrain_read_tile(pack_path, summit_leaf, &fused_tile) != 0) {
        fprintf(stderr, "FAIL: could not read LOD 4 summit leaf tile #%u\n", summit_leaf);
        backend_survey_dataset_free(&ds);
        return 1;
    }
    if (!backend_survey_fuse_tile(&ds, &archive_hdr, summit_leaf, &fused_tile, &fuse_stats) ||
        fuse_stats.mvs_elev_posts_refined == 0u ||
        fuse_stats.texels_orthorectified == 0u ||
        fuse_stats.mean_mvs_ncc < 0.55f) {
        fprintf(stderr, "FAIL: LOD 4 summit leaf dense MVS ZNCC failed (mvs_posts=%u, ncc=%.3f, texels=%u)\n",
                fuse_stats.mvs_elev_posts_refined, (double)fuse_stats.mean_mvs_ncc, fuse_stats.texels_orthorectified);
        unlink(tmp_nadir_png);
        unlink(tmp_oblique_png);
        backend_survey_dataset_free(&ds);
        return 1;
    }
    backend_survey_dataset_free(&ds);

    // 9. Verify AGL-only camera initialization, order-independent manifest tokens & 3-view MVS (pair 0-2)
    const char *tmp_manifest = "build/test_order_agl.manifest";
    FILE *mf = fopen(tmp_manifest, "w");
    if (mf == nullptr) {
        unlink(tmp_nadir_png);
        unlink(tmp_oblique_png);
        return 1;
    }
    fprintf(mf,
            "photo %s "
            "focal_mm=10.5 focal_35mm=24.0 preset=\"iPhone 15 Pro Main\" name=\"OrderTest#1\" "
            "wx=1595.0 wy=-103.0 agl=420.0 yaw=0.0 pitch=-90.0 roll=0.0 mvs=1\n"
            "photo %s "
            "preset=\"DJI Mavic 3 Enterprise\" focal_mm=12.29 "
            "wx=1595.0 wy=-103.0 alt=2240.0 yaw=0.0 pitch=-90.0 roll=0.0 mvs=1\n"
            "photo %s "
            "preset=\"iPhone 15 Pro Main\" "
            "wx=1380.0 wy=-318.0 alt=2115.0 yaw=45.0 pitch=-60.6 roll=0.0 mvs=1\n",
            tmp_nadir_png, tmp_nadir_png, tmp_oblique_png);
    fclose(mf);

    TerrainSurveyDataset ds_agl;
    backend_survey_dataset_init(&ds_agl);
    if (backend_survey_load_manifest(&ds_agl, &archive_hdr.dir, tmp_manifest) != 0 ||
        ds_agl.photo_count != 3u) {
        fprintf(stderr, "FAIL: could not load build/test_order_agl.manifest\n");
        backend_survey_dataset_free(&ds_agl);
        unlink(tmp_manifest);
        unlink(tmp_nadir_png);
        unlink(tmp_oblique_png);
        return 1;
    }
    unlink(tmp_manifest);
    unlink(tmp_nadir_png);
    unlink(tmp_oblique_png);

    // Photo 0 had focal_mm=10.5 before focal_35mm=24.0 and preset="iPhone 15 Pro Main", plus agl=420.0 without alt=
    if (fabsf(ds_agl.photos[0].intrinsics.focal_mm - 10.5f) > 0.01f ||
        strcmp(ds_agl.photos[0].name, "OrderTest#1") != 0 ||
        !ds_agl.photos[0].pose.use_agl ||
        ds_agl.photos[0].pose.altitude_msl_m < 1900.0f) {
        fprintf(stderr, "FAIL: order-independent photo token or AGL altitude init failed (focal=%.2f, name='%s', alt=%.1f)\n",
                (double)ds_agl.photos[0].intrinsics.focal_mm,
                ds_agl.photos[0].name,
                (double)ds_agl.photos[0].pose.altitude_msl_m);
        backend_survey_dataset_free(&ds_agl);
        return 1;
    }
    // Now set Photo 0 and Photo 1 to nearly identical poses (<0.2 deg parallax) so pair (0,1) has insufficient baseline,
    // while Photo 2 has a 28-deg baseline with both. Verify 3-view MVS still succeeds via pairs (0,2) and (1,2).
    ds_agl.photos[0].intrinsics = ds_agl.photos[1].intrinsics;
    ds_agl.photos[0].pose       = ds_agl.photos[1].pose;
    ds_agl.photos[0].pose.world_x += 0.05f;
    backend_survey_init_pose(&archive_hdr.dir, &ds_agl.photos[0].pose);

    if (backend_terrain_read_tile(pack_path, summit_leaf, &fused_tile) != 0) {
        backend_survey_dataset_free(&ds_agl);
        return 1;
    }
    TerrainSurveyStats stats_3v = {0};
    if (!backend_survey_fuse_tile(&ds_agl, &archive_hdr, summit_leaf, &fused_tile, &stats_3v) ||
        stats_3v.mvs_elev_posts_refined == 0u ||
        stats_3v.mean_mvs_ncc < 0.55f) {
        fprintf(stderr, "FAIL: 3-view MVS failed when views 0 & 1 had <0.5 deg parallax (mvs_posts=%u, ncc=%.3f)\n",
                stats_3v.mvs_elev_posts_refined, (double)stats_3v.mean_mvs_ncc);
        backend_survey_dataset_free(&ds_agl);
        return 1;
    }
    backend_survey_dataset_free(&ds_agl);

    // 10. Verify AABB-clipped DEM self-occlusion ray-marching for a distant oblique camera (1.8 km away)
    TerrainSurveyDataset ds_occ;
    backend_survey_dataset_init(&ds_occ);
    uint32_t solid_red[64 * 64];
    for (uint32_t i = 0u; i < 64u * 64u; ++i) {
        solid_red[i] = 0xFF0000FFu; // Pure red (R=255, G=0, B=0, A=255)
    }
    const GpuMathQuadNode *snode = &archive_hdr.nodes[summit_leaf];
    float tile_cx = 0.5f * (snode->world_min_x + snode->world_max_x);
    float tile_cy = 0.5f * (snode->world_min_y + snode->world_max_y);
    TerrainSurveyPhoto occ_photo = {
        .name = "DistantGrazingCam",
        .intrinsics = {
            .width_px         = 64u,
            .height_px        = 64u,
            .sensor_width_mm  = 36.0f,
            .sensor_height_mm = 24.0f,
            .focal_mm         = 50.0f,
        },
        .pose = {
            .world_x        = tile_cx - 1800.0f,
            .world_y        = tile_cy,
            .altitude_msl_m = 1350.0f,
            .yaw_deg        = 90.0f,  // Looking due East (+X)
            .pitch_deg      = -5.0f,
            .roll_deg       = 0.0f,
            .use_wgs84      = false,
        },
        .rgba_pixels  = solid_red,
        .blend_weight = 1.0f,
        .enable_mvs   = false,
    };
    if (backend_survey_add_photo(&ds_occ, &archive_hdr.dir, &occ_photo) < 0) {
        backend_survey_dataset_free(&ds_occ);
        return 1;
    }
    // Construct a synthetic tile with a flat 1000m floor and a narrow 1250m north-south ridge wall at sx=110..118
    for (uint32_t sy = 0u; sy < k_gmpack_tile_dim; ++sy) {
        for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
            float h = (sx >= 110u && sx <= 118u) ? 1250.0f : 1000.0f;
            fused_tile.height_u16[sy * k_gmpack_tile_dim + sx]    = (uint16_t)lrintf(h * 16.0f);
            fused_tile.imagery_rgba8[sy * k_gmpack_tile_dim + sx] = 0xFF204020u; // Dark green base
        }
    }
    archive_hdr.nodes[summit_leaf].header = gpumath_tile_header_pack(
        0u, 0u, 4u, 0u, 1000.0f, 1250.0f);
    TerrainSurveyStats occ_stats = {0};
    (void)backend_survey_fuse_tile(&ds_occ, &archive_hdr, summit_leaf, &fused_tile, &occ_stats);
    // Point in front of the ridge (sx=90, sy=128) is visible to the West camera -> red channel should increase;
    // Point immediately in the shadow behind the ridge (sx=130, sy=128) is occluded -> red channel must remain 0x20.
    uint32_t front_rgba  = fused_tile.imagery_rgba8[128u * k_gmpack_tile_dim + 90u];
    uint32_t shadow_rgba = fused_tile.imagery_rgba8[128u * k_gmpack_tile_dim + 130u];
    if ((front_rgba & 0xFFu) <= 0x40u || (shadow_rgba & 0xFFu) != 0x20u) {
        fprintf(stderr, "FAIL: distant ridge self-occlusion failed (front_R=%u, shadow_R=%u)\n",
                front_rgba & 0xFFu, shadow_rgba & 0xFFu);
        backend_survey_dataset_free(&ds_occ);
        return 1;
    }
    backend_survey_dataset_free(&ds_occ);

    // 11. Verify Bird's-Eye Ortho Overlay baking & manifest ortho_overlay directive
    if (sizeof(GpuMathOrthoOverlay) != 64u || sizeof(GpuMathArchiveHeader) != 24576u) {
        fprintf(stderr, "FAIL: GpuMathOrthoOverlay size=%zu or GpuMathArchiveHeader size=%zu mismatch\n",
                sizeof(GpuMathOrthoOverlay), sizeof(GpuMathArchiveHeader));
        return 1;
    }
    const char *tmp_ortho_png = "build/test ortho with spaces.png";
    const char *tmp_ortho_man = "build/test_ortho_overlay.manifest";
    uint32_t cyan_px[32 * 32];
    for (uint32_t i = 0u; i < 32u * 32u; ++i) {
        cyan_px[i] = 0xFFFFFF00u; // R=0, G=255, B=255, A=255
    }
    if (backend_survey_save_png_file(tmp_ortho_png, cyan_px, 32u, 32u) != 0) {
        fprintf(stderr, "FAIL: could not save test ortho overlay PNG\n");
        return 1;
    }
    FILE *omf = fopen(tmp_ortho_man, "w");
    if (omf == nullptr) {
        unlink(tmp_ortho_png);
        return 1;
    }
    fprintf(omf,
            "ortho_overlay \"%s\" wx=%.1f wy=%.1f hx=400.0 hy=400.0 weight=1.0\n",
            tmp_ortho_png, (double)tile_cx, (double)tile_cy);
    fclose(omf);

    TerrainSurveyDataset ds_ortho;
    backend_survey_dataset_init(&ds_ortho);
    if (backend_survey_load_manifest(&ds_ortho, &archive_hdr.dir, tmp_ortho_man) != 0 ||
        ds_ortho.ortho_overlay_count != 1u ||
        fabsf(ds_ortho.ortho_overlays[0].half_span_x - 400.0f) > 0.01f ||
        fabsf(ds_ortho.ortho_overlays[0].half_span_y - 400.0f) > 0.01f) {
        fprintf(stderr, "FAIL: ortho_overlay manifest directive failed to load (count=%u, hx=%.1f, hy=%.1f)\n",
                ds_ortho.ortho_overlay_count,
                (double)ds_ortho.ortho_overlays[0].half_span_x,
                (double)ds_ortho.ortho_overlays[0].half_span_y);
        backend_survey_dataset_free(&ds_ortho);
        unlink(tmp_ortho_man);
        unlink(tmp_ortho_png);
        return 1;
    }
    unlink(tmp_ortho_man);
    unlink(tmp_ortho_png);

    for (uint32_t i = 0u; i < k_gmpack_tile_dim * k_gmpack_tile_dim; ++i) {
        fused_tile.imagery_rgba8[i] = 0xFF101010u;
    }
    TerrainSurveyStats ortho_stats = {0};
    if (!backend_survey_fuse_tile(&ds_ortho, &archive_hdr, summit_leaf, &fused_tile, &ortho_stats) ||
        ortho_stats.texels_orthorectified == 0u) {
        fprintf(stderr, "FAIL: backend_survey_fuse_tile did not bake ortho_overlay\n");
        backend_survey_dataset_free(&ds_ortho);
        return 1;
    }
    uint32_t center_rgba = fused_tile.imagery_rgba8[128u * k_gmpack_tile_dim + 128u];
    if (((center_rgba >> 8u) & 0xFFu) < 200u || ((center_rgba >> 16u) & 0xFFu) < 200u) {
        fprintf(stderr, "FAIL: baked ortho_overlay center pixel mismatch (0x%08x)\n", center_rgba);
        backend_survey_dataset_free(&ds_ortho);
        return 1;
    }
    backend_survey_dataset_free(&ds_ortho);

    // 12. Milestone 2 (.epack v1 3.84x Compressed Codec + Lossless Perimeter Seam + Registered io_uring Fixed Read)
    if (sizeof(EpackTileBlock) != 102400u || sizeof(GpuMathTileBlock) != 393216u) {
        fprintf(stderr, "FAIL: EpackTileBlock size=%zu (expected 102400) or GpuMathTileBlock size=%zu\n",
                sizeof(EpackTileBlock), sizeof(GpuMathTileBlock));
        return 1;
    }

    static GpuMathTileBlock raw_summit;
    static GpuMathTileBlock dec_summit;
    static EpackTileBlock   enc_summit;
    if (backend_terrain_read_tile(pack_path, summit_leaf, &raw_summit) != 0) {
        fprintf(stderr, "FAIL: could not read summit_leaf from .gmpack for .epack test\n");
        return 1;
    }
    backend_epack_encode_tile(&raw_summit, &enc_summit);
    backend_epack_decode_tile(&enc_summit, &dec_summit);

    // 12a. Verify 0.0000 m EXACT lossless perimeter invariance (all 1020 border vertices)
    // and interior elevation RMSE + BC1 imagery PSNR
    double sum_sq_elev = 0.0;
    uint32_t interior_cnt = 0u;
    double sum_sq_rgb = 0.0;
    for (uint32_t y = 0u; y < k_gmpack_tile_dim; ++y) {
        for (uint32_t x = 0u; x < k_gmpack_tile_dim; ++x) {
            uint32_t idx = y * k_gmpack_tile_dim + x;
            bool is_border = (x == 0u || x == 255u || y == 0u || y == 255u);
            float orig_h = (float)raw_summit.height_u16[idx] * 0.0625f;
            float dec_h  = backend_epack_load_height_m(&enc_summit, x, y);
            float dec_h2 = (float)dec_summit.height_u16[idx] * 0.0625f;
            if (is_border) {
                if (raw_summit.height_u16[idx] != dec_summit.height_u16[idx] ||
                    fabsf(orig_h - dec_h) != 0.0f || fabsf(dec_h - dec_h2) != 0.0f) {
                    fprintf(stderr, "FAIL: border vertex (%u,%u) is not bit-exact lossless (orig=%u dec=%u)\n",
                            x, y, raw_summit.height_u16[idx], dec_summit.height_u16[idx]);
                    return 1;
                }
            } else {
                double dh = (double)orig_h - (double)dec_h;
                sum_sq_elev += dh * dh;
                interior_cnt++;
                if (fabsf(dec_h - dec_h2) > 0.07f) {
                    fprintf(stderr, "FAIL: backend_epack_load_height_m vs decode_tile mismatch at (%u,%u)\n", x, y);
                    return 1;
                }
            }
            uint32_t c0 = raw_summit.imagery_rgba8[idx];
            uint32_t c1 = backend_epack_load_rgba8(&enc_summit, x, y);
            if (c1 != dec_summit.imagery_rgba8[idx]) {
                fprintf(stderr, "FAIL: backend_epack_load_rgba8 vs decode_tile mismatch at (%u,%u)\n", x, y);
                return 1;
            }
            double dr = (double)(c0 & 0xFFu) - (double)(c1 & 0xFFu);
            double dg = (double)((c0 >> 8u) & 0xFFu) - (double)((c1 >> 8u) & 0xFFu);
            double db = (double)((c0 >> 16u) & 0xFFu) - (double)((c1 >> 16u) & 0xFFu);
            sum_sq_rgb += (dr * dr + dg * dg + db * db) / 3.0;
        }
    }
    double elev_rmse_m = sqrt(sum_sq_elev / (double)interior_cnt);
    double rgb_mse     = sum_sq_rgb / (double)(k_gmpack_tile_dim * k_gmpack_tile_dim);
    double rgb_psnr_db = (rgb_mse > 1.0e-9) ? (10.0 * log10((255.0 * 255.0) / rgb_mse)) : 99.0;
    if (elev_rmse_m > 0.12 || rgb_psnr_db < 32.0) {
        fprintf(stderr, "FAIL: .epack quality out of bounds (elev_rmse=%.4f m, psnr=%.2f dB)\n",
                elev_rmse_m, rgb_psnr_db);
        return 1;
    }

    // 12b. Synthesize baklan_besparmak.epack and verify RelPtr2D 25-block stride + non-blocking io_uring fixed read
    const char *epack_path = "examples/demo/maps/baklan_besparmak.epack";
    if (backend_terrain_ensure_epack(epack_path, pack_path) != 0) {
        fprintf(stderr, "FAIL: backend_terrain_ensure_epack failed\n");
        return 1;
    }
    static GpuMathArchiveHeader epack_hdr;
    if (backend_terrain_read_epack_header(epack_path, pack_path, &epack_hdr) != 0 ||
        epack_hdr.dir.magic != k_epack_magic ||
        epack_hdr.dir.node_count != k_gmpack_node_count) {
        fprintf(stderr, "FAIL: backend_terrain_read_epack_header mismatch\n");
        return 1;
    }
    uint64_t hdr_base_addr = (uint64_t)(uintptr_t)&epack_hdr;
    uint64_t slot1_off = relptr2d_resolve_addr(
        hdr_base_addr, epack_hdr.nodes[1].rel_ssd_payload) - hdr_base_addr;
    if (slot1_off != (uint64_t)(k_gmpack_header_blocks + k_epack_tile_blocks_4k) * 4096ull) {
        fprintf(stderr, "FAIL: .epack RelPtr2D slot 1 byte offset=%llu (expected %llu)\n",
                (unsigned long long)slot1_off,
                (unsigned long long)(k_gmpack_header_blocks + k_epack_tile_blocks_4k) * 4096ull);
        return 1;
    }

    // Verify backend_survey_tile_may_overlap culling
    TerrainSurveyDataset empty_ds;
    backend_survey_dataset_init(&empty_ds);
    if (backend_survey_tile_may_overlap(&empty_ds, &epack_hdr.nodes[summit_leaf])) {
        fprintf(stderr, "FAIL: empty survey dataset reported tile overlap\n");
        return 1;
    }

    // Verify registered fixed-file + fixed-buffer non-blocking io_uring .epack tile read
    setenv("ENGINE_URING_FIXED_BUF", "1", 1);
    if (ring_init() != 0) {
        fprintf(stderr, "FAIL: ring_init failed in test_backend\n");
        return 1;
    }
    RingContext *ring = ring_get_context();
    int epack_fd = open(epack_path, O_RDONLY | O_CLOEXEC);
    if (epack_fd < 0) {
        ring_shutdown();
        fprintf(stderr, "FAIL: open(%s) failed\n", epack_path);
        return 1;
    }
    bool fixed_file_ok = (ring_update_file(ring, ENG_PACK_FD_SLOT, epack_fd) == 0);
    struct io_uring_sqe *sqe = ring_sqe(ring);
    void *uring_dst = ring->pager_fixed_on
        ? ring_fixed_ptr(ring, ENG_FBUF_PAGER_BASE)
        : (void *)&enc_summit;
    if (sqe == nullptr || uring_dst == nullptr) {
        close(epack_fd);
        ring_shutdown();
        fprintf(stderr, "FAIL: ring_sqe or ring_fixed_ptr failed\n");
        return 1;
    }
    memset(uring_dst, 0, sizeof(EpackTileBlock));
    uint64_t off_summit = relptr2d_resolve_addr(
        hdr_base_addr, epack_hdr.nodes[summit_leaf].rel_ssd_payload) - hdr_base_addr;
    sqe->opcode    = ring->pager_fixed_on ? IORING_OP_READ_FIXED : IORING_OP_READ;
    sqe->flags     = fixed_file_ok ? IOSQE_FIXED_FILE : 0u;
    sqe->fd        = fixed_file_ok ? (int)ENG_PACK_FD_SLOT : epack_fd;
    sqe->off       = off_summit;
    sqe->addr      = (uint64_t)(uintptr_t)uring_dst;
    sqe->len       = k_epack_tile_bytes;
    sqe->buf_index = ring->pager_fixed_on ? (uint16_t)ENG_FBUF_PAGER_BASE : 0u;
    sqe->user_data = ENG_UD_PAGER_BASE + 3u;
    unsigned n_sub = ring_submit(ring);
    if (ring_enter(ring, n_sub, 0u, -1) < 0) {
        close(epack_fd);
        ring_shutdown();
        fprintf(stderr, "FAIL: non-blocking ring_enter(to_submit, 0) failed\n");
        return 1;
    }
    struct io_uring_cqe cqe = {0};
    while (!ring_peek(ring, &cqe)) {
        (void)ring_enter(ring, 0u, 1u, 100);
    }
    static EpackTileBlock disk_epack_tile;
    (void)backend_terrain_read_epack_tile(epack_path, summit_leaf, &disk_epack_tile);
    bool uring_match = ring_ud_is_pager(cqe.user_data) &&
                       cqe.res == (int32_t)k_epack_tile_bytes &&
                       memcmp(uring_dst, &disk_epack_tile, sizeof(EpackTileBlock)) == 0;
    if (fixed_file_ok) {
        (void)ring_update_file(ring, ENG_PACK_FD_SLOT, -1);
    }
    close(epack_fd);
    ring_shutdown();
    if (!uring_match) {
        fprintf(stderr, "FAIL: io_uring fixed-file/fixed-buf .epack read mismatch (res=%d)\n", cqe.res);
        return 1;
    }

    // 13. Milestone 5: Oriented 2D Gaussian Splatting (2DGS) Geodetic Surfels (32B, 0 EWA warp)
    if (sizeof(TerrainSurfel2DGS) != 32u || !backend_surfel_2dgs_self_check()) {
        fprintf(stderr, "FAIL: 2DGS geodetic surfel self-check or 32B struct invariant failed\n");
        return 1;
    }
    static TerrainSurfel2DGS summit_surfels[512];
    uint32_t tile_surfel_cnt = backend_surfel_2dgs_extract_tile(&epack_hdr.nodes[summit_leaf],
                                                                summit_leaf,
                                                                &enc_summit,
                                                                1.15f,
                                                                6u,
                                                                summit_surfels,
                                                                512u);
    if (tile_surfel_cnt == 0u) {
        fprintf(stderr, "FAIL: 2DGS cliff/karst surfel extractor emitted 0 surfels on Beşparmak summit tile\n");
        return 1;
    }
    float sn[3], stu[3], stv[3], ssu = 0.0f, ssv = 0.0f;
    backend_surfel_2dgs_unpack_basis(&summit_surfels[0], sn, stu, stv, &ssu, &ssv);
    float ortho_err = fabsf(sn[0] * stu[0] + sn[1] * stu[1] + sn[2] * stu[2]) +
                      fabsf(stu[0] * stv[0] + stu[1] * stv[1] + stu[2] * stv[2]);
    if (ortho_err > 1.0e-3f || ssu <= 0.0f || ssv <= 0.0f) {
        fprintf(stderr, "FAIL: unpacked summit 2DGS surfel tangent basis non-orthonormal (err=%.6f)\n",
                (double)ortho_err);
        return 1;
    }

    TerrainSurveyDataset ds_gs;
    backend_survey_dataset_init(&ds_gs);
    TerrainSurveyPoint gcp_pt = {
        .world_x = 1520.0f, .world_y = -620.0f, .elevation_m = 1612.0f,
        .radius_m = 12.0f, .weight = 1.0f, .r = 250u, .g = 90u, .b = 40u, .has_color = true
    };
    TerrainSurveyBreakline gcp_bl = {
        .world_ax = 1480.0f, .world_ay = -640.0f, .elev_a_m = 1590.0f,
        .world_bx = 1560.0f, .world_by = -600.0f, .elev_b_m = 1610.0f,
        .radius_m = 8.0f, .weight = 0.9f
    };
    (void)backend_survey_add_point(&ds_gs, &archive_hdr.dir, &gcp_pt);
    (void)backend_survey_add_breakline(&ds_gs, &gcp_bl);
    TerrainSurfel2DGS survey_surfels[64];
    uint32_t survey_surfel_cnt = backend_surfel_2dgs_extract_survey(&ds_gs, survey_surfels, 64u);
    backend_survey_dataset_free(&ds_gs);
    if (survey_surfel_cnt < 4u) {
        fprintf(stderr, "FAIL: backend_surfel_2dgs_extract_survey emitted %u surfels (expected >= 4)\n",
                survey_surfel_cnt);
        return 1;
    }

    printf("test_backend: ok (RelPtr2D + Lilliput64 + Cell64 + .epack 3.84x [RMSE %.4f m, border 0.0000 m, %.1f dB, fixed_buf=%u] + Survey MVS/GCP fused %u posts [%u MVS @ ZNCC %.3f] & %u ortho texels + 2DGS %u+%u surfels)\n",
           elev_rmse_m, rgb_psnr_db, 1u,
           fuse_stats.dem_posts_refined, fuse_stats.mvs_elev_posts_refined,
           (double)fuse_stats.mean_mvs_ncc, fuse_stats.texels_orthorectified,
           tile_surfel_cnt, survey_surfel_cnt);
    return 0;
}


