// Slang 2026 Fused Single-Pass SPD Bloom Downsample (Mips 0..4) + Center-Weighted 256-Bin Histogram
// - Mode 0: Reads 1x HDR target ONCE, accumulates center-weighted log2-luminance histogram in SLM,
//           applies Karis Average + soft-knee prefilter into Mip 0, then reduces Mips 1..4 inside
//           WaveReadLaneAt + groupshared SLM in a single compute dispatch.
// - Mode 1: Single-workgroup coarse tail downsampling Mip 4 -> Mip 5 for wide photographic bloom.

#include "post_common.slang"

[[vk::binding(0, 0)]] Sampler2D           g_hdr_src;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_mip0;
[[vk::binding(2, 0)]] RWTexture2D<float4> g_mip1;
[[vk::binding(3, 0)]] RWTexture2D<float4> g_mip2;
[[vk::binding(4, 0)]] RWTexture2D<float4> g_mip3;
[[vk::binding(5, 0)]] RWTexture2D<float4> g_mip4;
[[vk::binding(6, 0)]] Sampler2D           g_mip4_src;
[[vk::binding(7, 0)]] RWTexture2D<float4> g_mip5;

struct PostDownPush {
    uint64_t hist_bda;
    uint64_t exp_bda;
    uint     src_w;
    uint     src_h;
    uint     m0_w;
    uint     m0_h;
    uint     m1_w;
    uint     m1_h;
    uint     m2_w;
    uint     m2_h;
    uint     m3_w;
    uint     m3_h;
    uint     m4_w;
    uint     m4_h;
    uint     m5_w;
    uint     m5_h;
    float    threshold;
    float    knee;
    uint     mode;   // 0 = Fused Mip 0..4 + Histogram, 1 = Mip 4 -> Mip 5 tail
    uint     flags;  // bit 0 = accumulate histogram, bit 1 = write bloom mips, bit 2 = use exposure pre-scale
};

[[vk::push_constant]] PostDownPush pc;

groupshared uint   s_hist[256];
groupshared float3 s_tile[16][16];
groupshared float3 s_tile_b[8][8];

float3 sample_karis_5tap(Sampler2D tex, float2 uv, float2 texel_uv, out float3 out_center) {
    float3 c  = max(tex.SampleLevel(uv, 0.0).rgb, float3(0.0));
    float3 s0 = max(tex.SampleLevel(uv + float2(-texel_uv.x, -texel_uv.y), 0.0).rgb, float3(0.0));
    float3 s1 = max(tex.SampleLevel(uv + float2( texel_uv.x, -texel_uv.y), 0.0).rgb, float3(0.0));
    float3 s2 = max(tex.SampleLevel(uv + float2(-texel_uv.x,  texel_uv.y), 0.0).rgb, float3(0.0));
    float3 s3 = max(tex.SampleLevel(uv + float2( texel_uv.x,  texel_uv.y), 0.0).rgb, float3(0.0));
    out_center = c;

    // Karis inverse-luminance weights kill single-pixel specular/emissive fireflies
    float wc = post_karis_weight(c)  * 0.50;
    float w0 = post_karis_weight(s0) * 0.125;
    float w1 = post_karis_weight(s1) * 0.125;
    float w2 = post_karis_weight(s2) * 0.125;
    float w3 = post_karis_weight(s3) * 0.125;
    float ws = max(wc + w0 + w1 + w2 + w3, 1.0e-5);
    return (c * wc + s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3) / ws;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 wg : SV_GroupID) {
    uint tid = gtid.x;
    uint lx  = tid & 15u;
    uint ly  = tid >> 4u;

    // Mode 1: Single-workgroup coarse Mip 4 -> Mip 5 downsample
    if (pc.mode != 0u) {
        if ((pc.flags & 2u) == 0u || pc.m5_w == 0u || pc.m5_h == 0u) {
            return;
        }
        float2 inv_m4 = float2(1.0 / max(float(pc.m4_w), 1.0), 1.0 / max(float(pc.m4_h), 1.0));
        float2 inv_m5 = float2(1.0 / max(float(pc.m5_w), 1.0), 1.0 / max(float(pc.m5_h), 1.0));
        for (uint y = ly; y < pc.m5_h; y += 16u) {
            for (uint x = lx; x < pc.m5_w; x += 16u) {
                float2 uv = (float2(float(x), float(y)) + 0.5) * inv_m5;
                float3 c  = max(g_mip4_src.SampleLevel(uv, 0.0).rgb, float3(0.0));
                float3 a  = max(g_mip4_src.SampleLevel(uv + float2(-0.5, -0.5) * inv_m4, 0.0).rgb, float3(0.0));
                float3 b  = max(g_mip4_src.SampleLevel(uv + float2( 0.5, -0.5) * inv_m4, 0.0).rgb, float3(0.0));
                float3 d  = max(g_mip4_src.SampleLevel(uv + float2(-0.5,  0.5) * inv_m4, 0.0).rgb, float3(0.0));
                float3 e  = max(g_mip4_src.SampleLevel(uv + float2( 0.5,  0.5) * inv_m4, 0.0).rgb, float3(0.0));
                float3 avg = c * 0.5 + (a + b + d + e) * 0.125;
                g_mip5[uint2(x, y)] = float4(avg, 1.0);
            }
        }
        return;
    }

    const bool do_hist  = (pc.flags & 1u) != 0u && pc.hist_bda != 0ull;
    const bool do_bloom = (pc.flags & 2u) != 0u;
    const bool use_exp  = (pc.flags & 4u) != 0u && pc.exp_bda != 0ull;

    if (do_hist) {
        s_hist[tid] = 0u;
        GroupMemoryBarrierWithGroupSync();
    }

    float exp_scale = 1.0;
    if (use_exp) {
        PostExposureState st = ((PostExposureState *)pc.exp_bda)[0];
        if (st.valid != 0u) {
            exp_scale = clamp(st.exposure_scale, 0.125, 8.0);
        }
    }

    uint gx0 = wg.x * 16u + lx;
    uint gy0 = wg.y * 16u + ly;
    float3 m0_col = float3(0.0);

    if (gx0 < pc.m0_w && gy0 < pc.m0_h) {
        float2 uv       = (float2(float(gx0), float(gy0)) + 0.5) / float2(float(pc.m0_w), float(pc.m0_h));
        float2 texel_uv = float2(1.0 / max(float(pc.src_w), 1.0), 1.0 / max(float(pc.src_h), 1.0));
        float3 raw_center;
        float3 karis = sample_karis_5tap(g_hdr_src, uv, texel_uv, raw_center);

        if (do_hist) {
            float luma = post_luma(raw_center);
            uint bin = 0u;
            if (luma > 1.0e-4) {
                float norm = saturate((log2(luma) - k_post_log_luma_min) / k_post_log_luma_span);
                bin = uint(clamp(norm * 254.0 + 1.0, 1.0, 255.0));
            }
            // Smooth center-weighted metering (4x center, 1x corners)
            float2 d_c = uv * 2.0 - 1.0;
            uint   w_c = 1u + uint(saturate(1.0 - dot(d_c, d_c) * 0.45) * 3.0);
            InterlockedAdd(s_hist[bin], w_c);
        }

        if (do_bloom) {
            float3 pre = min(karis * exp_scale, float3(24.0));
            m0_col = post_soft_threshold(pre, pc.threshold, pc.knee);
            g_mip0[uint2(gx0, gy0)] = float4(m0_col, 1.0);
        }
    }

    if (do_hist) {
        GroupMemoryBarrierWithGroupSync();
        uint bin_cnt = s_hist[tid];
        if (bin_cnt != 0u) {
            uint *global_hist = (uint *)pc.hist_bda;
            InterlockedAdd(global_hist[tid], bin_cnt);
        }
    }

    if (!do_bloom) {
        return;
    }

    // --- Fused SPD Subgroup + SLM Reduction: Mip 0 -> Mip 1 -> Mip 2 -> Mip 3 -> Mip 4 ---
    uint lane = WaveGetLaneIndex();
    float3 pair_x = m0_col;
    if (WaveGetLaneCount() >= 8u) {
        float3 partner = WaveReadLaneAt(m0_col, lane ^ 1u);
        if ((lx & 1u) == 0u) {
            pair_x = (gx0 + 1u < pc.m0_w) ? (0.5 * (m0_col + partner)) : m0_col;
        }
    }
    s_tile[ly][lx] = (WaveGetLaneCount() >= 8u) ? pair_x : m0_col;
    GroupMemoryBarrierWithGroupSync();

    // Mip 1 (8x8 per workgroup): read s_tile[16][16] -> write s_tile_b[8][8] (zero aliasing)
    if (lx < 8u && ly < 8u) {
        uint gx1 = wg.x * 8u + lx;
        uint gy1 = wg.y * 8u + ly;
        float3 c1 = float3(0.0);
        if (gx1 < pc.m1_w && gy1 < pc.m1_h) {
            uint sx0 = lx * 2u;
            uint sx1 = (gx1 * 2u + 1u < pc.m0_w) ? (sx0 + 1u) : sx0;
            uint sy0 = ly * 2u;
            uint sy1 = (gy1 * 2u + 1u < pc.m0_h) ? (sy0 + 1u) : sy0;
            if (WaveGetLaneCount() >= 8u) {
                c1 = 0.5 * (s_tile[sy0][sx0] + s_tile[sy1][sx0]);
            } else {
                c1 = 0.25 * (s_tile[sy0][sx0] + s_tile[sy0][sx1] + s_tile[sy1][sx0] + s_tile[sy1][sx1]);
            }
            g_mip1[uint2(gx1, gy1)] = float4(c1, 1.0);
        }
        s_tile_b[ly][lx] = c1;
    }
    GroupMemoryBarrierWithGroupSync();

    // Mip 2 (4x4 per workgroup): read s_tile_b[8][8] -> write s_tile[4][4]
    if (lx < 4u && ly < 4u) {
        uint gx2 = wg.x * 4u + lx;
        uint gy2 = wg.y * 4u + ly;
        float3 c2 = float3(0.0);
        if (gx2 < pc.m2_w && gy2 < pc.m2_h) {
            uint sx0 = lx * 2u;
            uint sx1 = (gx2 * 2u + 1u < pc.m1_w) ? (sx0 + 1u) : sx0;
            uint sy0 = ly * 2u;
            uint sy1 = (gy2 * 2u + 1u < pc.m1_h) ? (sy0 + 1u) : sy0;
            c2 = 0.25 * (s_tile_b[sy0][sx0] + s_tile_b[sy0][sx1] + s_tile_b[sy1][sx0] + s_tile_b[sy1][sx1]);
            g_mip2[uint2(gx2, gy2)] = float4(c2, 1.0);
        }
        s_tile[ly][lx] = c2;
    }
    GroupMemoryBarrierWithGroupSync();

    // Mip 3 (2x2 per workgroup): read s_tile[4][4] -> write s_tile_b[2][2]
    if (lx < 2u && ly < 2u) {
        uint gx3 = wg.x * 2u + lx;
        uint gy3 = wg.y * 2u + ly;
        float3 c3 = float3(0.0);
        if (gx3 < pc.m3_w && gy3 < pc.m3_h) {
            uint sx0 = lx * 2u;
            uint sx1 = (gx3 * 2u + 1u < pc.m2_w) ? (sx0 + 1u) : sx0;
            uint sy0 = ly * 2u;
            uint sy1 = (gy3 * 2u + 1u < pc.m2_h) ? (sy0 + 1u) : sy0;
            c3 = 0.25 * (s_tile[sy0][sx0] + s_tile[sy0][sx1] + s_tile[sy1][sx0] + s_tile[sy1][sx1]);
            g_mip3[uint2(gx3, gy3)] = float4(c3, 1.0);
        }
        s_tile_b[ly][lx] = c3;
    }
    GroupMemoryBarrierWithGroupSync();

    // Mip 4 (1x1 per workgroup): read s_tile_b[2][2]
    if (lx == 0u && ly == 0u) {
        uint gx4 = wg.x;
        uint gy4 = wg.y;
        if (gx4 < pc.m4_w && gy4 < pc.m4_h) {
            uint sx1 = (gx4 * 2u + 1u < pc.m3_w) ? 1u : 0u;
            uint sy1 = (gy4 * 2u + 1u < pc.m3_h) ? 1u : 0u;
            float3 c4 = 0.25 * (s_tile_b[0][0] + s_tile_b[0][sx1] + s_tile_b[sy1][0] + s_tile_b[sy1][sx1]);
            g_mip4[uint2(gx4, gy4)] = float4(c4, 1.0);
        }
    }
}
