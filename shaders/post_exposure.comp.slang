// Slang 2026 Single-Workgroup 256-Bin Log-Luminance Histogram Auto-Exposure Solver
// - Computes percentile-trimmed mean [10%..95%] across the center-weighted luminance histogram
// - Rejects dark voids and bright emissive/specular outliers automatically
// - Applies asymmetric exponential temporal eye adaptation directly in BDA (zero CPU readback)
// - Self-clears the 256-bin histogram to 0 for the next frame

#include "post_common.slang"

struct PostExposurePush {
    uint64_t hist_bda;
    uint64_t exp_bda;
    float    dt_s;
    float    key_value;
    float    min_scale;
    float    max_scale;
    float    speed_up;
    float    speed_down;
    uint     snap;
    uint     frame_index;
};

[[vk::push_constant]] PostExposurePush pc;

groupshared uint  s_wave_tot[32];
groupshared float s_wave_log[32];
groupshared float s_wave_cnt[32];

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID) {
    uint tid      = gtid.x;
    uint lane     = WaveGetLaneIndex();
    uint wave_sz  = max(WaveGetLaneCount(), 8u);
    uint wave_cnt = min((256u + wave_sz - 1u) / wave_sz, 32u);
    uint wave_id  = tid / wave_sz;

    bool valid_ptrs = (pc.hist_bda != 0ull && pc.exp_bda != 0ull);
    uint cnt = 0u;
    if (valid_ptrs) {
        uint *hist = (uint *)pc.hist_bda;
        cnt        = hist[tid];
        // Self-clear histogram for next frame (zero extra clear pass!)
        hist[tid]  = 0u;
    }

    // Bin 0 holds near-black pixels; keep it separate so dark voids don't crush exposure
    uint nz_cnt = (tid == 0u) ? 0u : cnt;

    // Stage 1: Exact 256-bin inclusive CDF via Vulkan 1.4 WavePrefixSum (1 barrier, zero RMW loops)
    uint local_inc = WavePrefixSum(nz_cnt) + nz_cnt;
    if (lane == wave_sz - 1u && wave_id < 32u) {
        s_wave_tot[wave_id] = local_inc;
    }
    GroupMemoryBarrierWithGroupSync();

    uint wave_base = 0u;
    uint total     = 0u;
    for (uint w = 0u; w < wave_cnt; ++w) {
        uint wt = s_wave_tot[w];
        if (w < wave_id) {
            wave_base += wt;
        }
        total += wt;
    }

    uint  bin_hi = wave_base + local_inc;
    uint  bin_lo = bin_hi - nz_cnt;
    float w_in   = 0.0;
    float log_c  = k_post_log_luma_min;
    if (total > 0u && tid > 0u) {
        uint lo_cut = uint(float(total) * 0.10);
        uint hi_cut = uint(float(total) * 0.95);
        uint ov_lo  = max(bin_lo, lo_cut);
        uint ov_hi  = min(bin_hi, hi_cut);
        if (ov_hi > ov_lo) {
            w_in  = float(ov_hi - ov_lo);
            float t = (float(tid) - 0.5) / 254.0;
            log_c = k_post_log_luma_min + t * k_post_log_luma_span;
        }
    }

    // Stage 2: Percentile-trimmed mean via Vulkan 1.4 WaveActiveSum (1 barrier, 384B total SLM)
    float w_log_sum = WaveActiveSum(w_in * log_c);
    float w_cnt_sum = WaveActiveSum(w_in);
    if (WaveIsFirstLane() && wave_id < 32u) {
        s_wave_log[wave_id] = w_log_sum;
        s_wave_cnt[wave_id] = w_cnt_sum;
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid == 0u && valid_ptrs) {
        float total_log = 0.0;
        float wsum      = 0.0;
        for (uint w = 0u; w < wave_cnt; ++w) {
            total_log += s_wave_log[w];
            wsum      += s_wave_cnt[w];
        }
        PostExposureState *st = (PostExposureState *)pc.exp_bda;
        float avg_l  = (wsum > 1.0) ? exp2(total_log / wsum) : pc.key_value;
        float t_scl  = clamp(pc.key_value / max(avg_l, 1.0e-4), pc.min_scale, pc.max_scale);
        float t_ev   = log2(max(avg_l, 1.0e-4) * 8.0);

        float prev_scl = (st[0].valid != 0u && pc.snap == 0u)
                       ? clamp(st[0].exposure_scale, pc.min_scale, pc.max_scale)
                       : t_scl;
        float dt       = clamp(pc.dt_s, 0.0, 0.25);
        float rate     = (t_scl < prev_scl) ? pc.speed_up : pc.speed_down;
        float alpha    = (pc.snap != 0u || st[0].valid == 0u) ? 1.0 : (1.0 - exp(-dt * rate));
        float new_scl  = lerp(prev_scl, t_scl, alpha);
        float new_ev   = (pc.snap != 0u || st[0].valid == 0u) ? t_ev : lerp(st[0].ev100, t_ev, alpha);

        st[0].ev100          = new_ev;
        st[0].exposure_scale = new_scl;
        st[0].avg_luma       = avg_l;
        st[0].target_ev100   = t_ev;
        st[0].valid          = 1u;
        st[0].frame_index    = pc.frame_index;
    }
}
