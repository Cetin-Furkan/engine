// Slang 2026 Unified Post-Processing Composite Shader
// - Single-point exposure, energy-conserving optical bloom blend, subtle vignette,
//   full Troy Sobotka AgX ("Punchy") filmic tone mapping, and Triangular-PDF IGN 8-bit dithering.
// - Clean passthrough mode (flags & 1 == 0) for 2D drafting & World 0 empty canvas.

#include "post_common.slang"

[[vk::binding(0, 0)]] Sampler2D g_hdr_tex;
[[vk::binding(1, 0)]] Sampler2D g_bloom_tex;

struct PostCompPush {
    uint64_t exp_bda;
    uint     width;
    uint     height;
    uint     flags;       // bit 0 = HDR AgX active, bit 1 = bloom enabled, bit 2 = auto-exposure enabled
    uint     frame_index;
    float    bloom_strength;
    float    manual_exposure;
    float    vignette_strength;
    float    pad0;
};

[[vk::push_constant]] PostCompPush pc;

[shader("fragment")]
float4 main(float4 frag : SV_Position) : SV_Target0 {
    float2 inv_wh = float2(1.0 / max(float(pc.width), 1.0), 1.0 / max(float(pc.height), 1.0));
    float2 uv     = frag.xy * inv_wh;
    float3 hdr    = max(g_hdr_tex.SampleLevel(uv, 0.0).rgb, float3(0.0));

    // Passthrough mode (2D drafting sheet / World 0): exact color fidelity without bloom or exposure pump
    if ((pc.flags & 1u) == 0u) {
        return float4(clamp(hdr, float3(0.0), float3(1.0)), 1.0);
    }

    // Read GPU-adapted exposure scale (or manual fallback)
    float exp_scale = pc.manual_exposure;
    if ((pc.flags & 4u) != 0u && pc.exp_bda != 0ull) {
        PostExposureState st = ((PostExposureState *)pc.exp_bda)[0];
        if (st.valid != 0u) {
            exp_scale *= clamp(st.exposure_scale, 0.18, 6.0);
        }
    }

    float3 exposed = hdr * exp_scale;

    // Physically-based energy-conserving bloom (soft-thresholded & pre-exposed in bloom_down)
    if ((pc.flags & 2u) != 0u && pc.bloom_strength > 0.0) {
        float3 bloom = max(g_bloom_tex.SampleLevel(uv, 0.0).rgb, float3(0.0));
        exposed += bloom * pc.bloom_strength;
    }

    // Whisper-subtle natural optical vignette
    if (pc.vignette_strength > 0.0) {
        float2 d = uv * 2.0 - 1.0;
        float  v = 1.0 - dot(d, d) * pc.vignette_strength;
        exposed *= clamp(v, 0.5, 1.0);
    }

    float3 agx_lin = post_agx_punchy_linear(exposed);
    float3 final_c = post_dither_for_srgb_8bit(agx_lin, frag.xy, pc.frame_index);
    return float4(final_c, 1.0);
}
