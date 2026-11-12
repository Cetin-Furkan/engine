// Slang 2026 Subgroup-Accelerated Single-Pass Hi-Z Depth Pyramid Builder
// Mode 0 (Tile Pass): Reads full-res D32_SFLOAT depth image via Vulkan 1.4 push descriptor,
//                     reduces 32x32 pixel tiles across Mip 0..4 (div 2..div 32) in ONE pass
//                     using wave lane shuffles (WaveReadLaneAt) + groupshared memory.
// Mode 1 (Tail Pass): Single workgroup reduces coarse Mip 4 -> Mip 5..(mip_count-1) down to 1x1
//                     and stamps camera state + valid flag into SceneHizHeader for Frame N+1.

#include "scene_common.slang"

[[vk::binding(0, 0)]]
Texture2D<float> g_depth_tex;

struct SceneHizPush {
    uint64_t hiz_bda;
    uint64_t cull_slot_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    uint     mode;        // 0 = Mip 0..4 tile pass, 1 = Mip 5..N coarse tail pass
    uint     commit_prev; // 1 = GPU-predicated Phase-2 rebuild + publish camera pose into SceneHizHeader
    uint     pad0;
    uint     pad1;
};

[[vk::push_constant]] SceneHizPush pc;

groupshared float s_tile[16][16];

float load_full_depth_2x2(uint gx0, uint gy0, uint fw, uint fh) {
    int x0 = int(min(gx0 * 2u,      fw - 1u));
    int x1 = int(min(gx0 * 2u + 1u, fw - 1u));
    int y0 = int(min(gy0 * 2u,      fh - 1u));
    int y1 = int(min(gy0 * 2u + 1u, fh - 1u));

    float d00 = g_depth_tex.Load(int3(x0, y0, 0));
    float d10 = g_depth_tex.Load(int3(x1, y0, 0));
    float d01 = g_depth_tex.Load(int3(x0, y1, 0));
    float d11 = g_depth_tex.Load(int3(x1, y1, 0));
    return max(max(d00, d10), max(d01, d11));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 wg : SV_GroupID) {
    if (pc.hiz_bda == 0ull) {
        return;
    }
    SceneHizHeader *hdr_ptr = (SceneHizHeader *)pc.hiz_bda;
    SceneHizHeader hdr      = hdr_ptr[0];
    float *hiz              = (float *)(pc.hiz_bda + 256ull);
    uint mip_count          = hdr.mip_count;
    if (mip_count == 0u) {
        return;
    }

    // GPU-predicated Phase-2 steady-state skip: if Phase 2 disoccluded 0 meshlets
    // (phase2_draw_count == 0), s_vis_depth did not change after Phase 1, so skip
    // re-reading the full-resolution depth buffer and only commit prev_cam in Mode 1.
    bool need_rebuild = true;
    if (pc.commit_prev != 0u && pc.cull_slot_bda != 0ull) {
        uint p2_draws = ((uint *)pc.cull_slot_bda)[1];
        need_rebuild = (p2_draws > 0u);
    }

    // 1D linear thread index guarantees subgroup lane (lane ^ 1u) == (lx ^ 1u, ly)
    // across SIMD8, SIMD16, SIMD32, and Wave64 without 2D swizzle ambiguity.
    uint tid = gtid.x;
    uint lx  = tid & 15u;
    uint ly  = tid >> 4u;

    if (pc.mode == 0u) {
        if (!need_rebuild) {
            return;
        }
        // --- Mip 0 (ceil(fw/2) x ceil(fh/2)) ---
        SceneHizMipInfo m0 = hdr.mips[0];
        uint gx0 = wg.x * 16u + lx;
        uint gy0 = wg.y * 16u + ly;
        float d0 = 0.0;
        if (gx0 < m0.width && gy0 < m0.height) {
            d0 = load_full_depth_2x2(gx0, gy0, hdr.full_width, hdr.full_height);
            hiz[m0.offset_floats + gy0 * m0.width + gx0] = d0;
        }
        uint lane = WaveGetLaneIndex();
        float d0_pair_x = d0;
        if (WaveGetLaneCount() >= 8u) {
            float partner = WaveReadLaneAt(d0, lane ^ 1u);
            if ((lx & 1u) == 0u) {
                d0_pair_x = (gx0 + 1u < m0.width) ? max(d0, partner) : d0;
            }
        }
        s_tile[ly][lx] = (WaveGetLaneCount() >= 8u) ? d0_pair_x : d0;
        GroupMemoryBarrierWithGroupSync();

        // --- Mip 1 (8x8 per workgroup) ---
        if (mip_count > 1u && lx < 8u && ly < 8u) {
            SceneHizMipInfo m1 = hdr.mips[1];
            uint gx1 = wg.x * 8u + lx;
            uint gy1 = wg.y * 8u + ly;
            if (gx1 < m1.width && gy1 < m1.height) {
                uint sx0 = lx * 2u;
                uint sx1 = (gx1 * 2u + 1u < m0.width) ? (sx0 + 1u) : sx0;
                uint sy0 = ly * 2u;
                uint sy1 = (gy1 * 2u + 1u < m0.height) ? (sy0 + 1u) : sy0;
                float d1;
                if (WaveGetLaneCount() >= 8u) {
                    d1 = max(s_tile[sy0][sx0], s_tile[sy1][sx0]);
                } else {
                    d1 = max(max(s_tile[sy0][sx0], s_tile[sy0][sx1]),
                             max(s_tile[sy1][sx0], s_tile[sy1][sx1]));
                }
                hiz[m1.offset_floats + gy1 * m1.width + gx1] = d1;
                s_tile[ly][lx] = d1;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        // --- Mip 2 (4x4 per workgroup) ---
        if (mip_count > 2u && lx < 4u && ly < 4u) {
            SceneHizMipInfo m1 = hdr.mips[1];
            SceneHizMipInfo m2 = hdr.mips[2];
            uint gx2 = wg.x * 4u + lx;
            uint gy2 = wg.y * 4u + ly;
            if (gx2 < m2.width && gy2 < m2.height) {
                uint sx0 = lx * 2u;
                uint sx1 = (gx2 * 2u + 1u < m1.width) ? (sx0 + 1u) : sx0;
                uint sy0 = ly * 2u;
                uint sy1 = (gy2 * 2u + 1u < m1.height) ? (sy0 + 1u) : sy0;
                float d2 = max(max(s_tile[sy0][sx0], s_tile[sy0][sx1]),
                               max(s_tile[sy1][sx0], s_tile[sy1][sx1]));
                hiz[m2.offset_floats + gy2 * m2.width + gx2] = d2;
                s_tile[ly][lx] = d2;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        // --- Mip 3 (2x2 per workgroup) ---
        if (mip_count > 3u && lx < 2u && ly < 2u) {
            SceneHizMipInfo m2 = hdr.mips[2];
            SceneHizMipInfo m3 = hdr.mips[3];
            uint gx3 = wg.x * 2u + lx;
            uint gy3 = wg.y * 2u + ly;
            if (gx3 < m3.width && gy3 < m3.height) {
                uint sx0 = lx * 2u;
                uint sx1 = (gx3 * 2u + 1u < m2.width) ? (sx0 + 1u) : sx0;
                uint sy0 = ly * 2u;
                uint sy1 = (gy3 * 2u + 1u < m2.height) ? (sy0 + 1u) : sy0;
                float d3 = max(max(s_tile[sy0][sx0], s_tile[sy0][sx1]),
                               max(s_tile[sy1][sx0], s_tile[sy1][sx1]));
                hiz[m3.offset_floats + gy3 * m3.width + gx3] = d3;
                s_tile[ly][lx] = d3;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        // --- Mip 4 (1x1 per workgroup) ---
        if (mip_count > 4u && lx == 0u && ly == 0u) {
            SceneHizMipInfo m3 = hdr.mips[3];
            SceneHizMipInfo m4 = hdr.mips[4];
            uint gx4 = wg.x;
            uint gy4 = wg.y;
            if (gx4 < m4.width && gy4 < m4.height) {
                uint sx1 = (gx4 * 2u + 1u < m3.width)  ? 1u : 0u;
                uint sy1 = (gy4 * 2u + 1u < m3.height) ? 1u : 0u;
                float d4 = max(max(s_tile[0][0],   s_tile[0][sx1]),
                               max(s_tile[sy1][0], s_tile[sy1][sx1]));
                hiz[m4.offset_floats + gy4 * m4.width + gx4] = d4;
            }
        }
        return;
    }

    // --- Mode 1: Single-workgroup coarse tail reduction for Mip 5 .. (mip_count - 1) ---
    if (need_rebuild) {
        for (uint mip = 5u; mip < mip_count; ++mip) {
            SceneHizMipInfo src = hdr.mips[mip - 1u];
            SceneHizMipInfo dst = hdr.mips[mip];
            for (uint y = ly; y < dst.height; y += 16u) {
                for (uint x = lx; x < dst.width; x += 16u) {
                    uint sx0 = min(x * 2u,      src.width - 1u);
                    uint sx1 = min(x * 2u + 1u, src.width - 1u);
                    uint sy0 = min(y * 2u,      src.height - 1u);
                    uint sy1 = min(y * 2u + 1u, src.height - 1u);
                    float v00 = hiz[src.offset_floats + sy0 * src.width + sx0];
                    float v10 = hiz[src.offset_floats + sy0 * src.width + sx1];
                    float v01 = hiz[src.offset_floats + sy1 * src.width + sx0];
                    float v11 = hiz[src.offset_floats + sy1 * src.width + sx1];
                    hiz[dst.offset_floats + y * dst.width + x] = max(max(v00, v10), max(v01, v11));
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    if (pc.commit_prev != 0u && lx == 0u && ly == 0u) {
        hdr_ptr[0].prev_qx    = pc.qx;
        hdr_ptr[0].prev_qy    = pc.qy;
        hdr_ptr[0].prev_qz    = pc.qz;
        hdr_ptr[0].prev_qw    = pc.qw;
        hdr_ptr[0].prev_cam_x = pc.cam_x;
        hdr_ptr[0].prev_cam_y = pc.cam_y;
        hdr_ptr[0].prev_cam_z = pc.cam_z;
        hdr_ptr[0].prev_zoom  = pc.zoom;
        hdr_ptr[0].valid      = 1u;
    }
}
