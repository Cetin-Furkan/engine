// Engine v0.2 Milestone 5: Oriented 2D Gaussian Splatting (2DGS) Geodetic Surfel Fragment Shader
// Evaluates exact tangent-plane 2D Gaussian falloff (zero OpKill / zero FragDepth -> 100% hardware Early-Z)
// and shades with Aegean sun, sky hemisphere fill, aerial haze, and live 2026 SSILVB + Split HRC GI.

#include "camera.slang"
import terrain_math;
import lighting;

struct SurfelDrawPush {
    uint64_t surfels_bda;
    uint64_t indirect_bda;
    uint     width;
    uint     height;
    float    elev_base_m;
    float    vertical_scale;
    ViewPush view;
};

[[vk::push_constant]] SurfelDrawPush pc;

struct FSInput {
    float4 sv_pos     : SV_Position;
    float3 world_nrm  : TEXCOORD0;
    float3 world_pos  : TEXCOORD1;
    float2 local_uv   : TEXCOORD2;
    float4 color_opa  : TEXCOORD3;
    float2 view_z_flg : TEXCOORD4;
};

[shader("fragment")]
float4 main(FSInput input) : SV_Target0 {
    float r2     = dot(input.local_uv, input.local_uv);
    float window = smoothstep(7.5625, 5.0, r2);
    float gauss  = exp(-0.5 * min(r2, 8.0)) * window;
    float alpha  = saturate(input.color_opa.a * gauss);

    float3 world_nrm = normalize(input.world_nrm);
    float3 alb_lin   = srgb_to_linear(input.color_opa.rgb);

    // Match World 8 Aegean sunlight, sky hemisphere fill, and 2026 SSILVB + Split HRC canyon GI
    const float3 sun_dir  = normalize(float3(-0.50, -0.52, 0.69));
    const float3 fill_dir = normalize(float3(0.55, 0.45, 0.70));
    float ndl  = max(dot(world_nrm, sun_dir), 0.0);
    float fill = max(dot(world_nrm, fill_dir), 0.0);

    float3 sun_light   = float3(1.06, 0.99, 0.92) * (0.72 * ndl);
    float3 sky_fill    = float3(0.42, 0.48, 0.56) * (0.44 + 0.22 * world_nrm.z + 0.18 * fill);
    float3 bounce_fill = float3(0.32, 0.28, 0.22) * (0.14 * saturate(1.0 - world_nrm.z));

    float  gi_ao      = 1.0;
    float3 vnrm       = normalize(view_rotate(pc.view.q, world_nrm));
    float3 gi_bent    = vnrm;
    float3 gi_ind_rgb = float3(0.0);
    if (lighting_sample_gi(pc.view.materials, input.sv_pos.xy, input.view_z_flg.x, vnrm,
                           gi_ao, gi_bent, gi_ind_rgb)) {
        bounce_fill = bounce_fill * 0.35 + gi_ind_rgb * 0.85;
    }

    uint   s_flags   = uint(input.view_z_flg.y + 0.5);
    bool   is_beacon = (s_flags & 0xEu) != 0u;
    float3 lit_color = alb_lin * (sky_fill * lerp(0.56, 1.0, gi_ao) +
                                  sun_light * lerp(0.75, 1.0, gi_ao) +
                                  bounce_fill);
    if (is_beacon) {
        // Survey GCP / Stereo Tie-Point / Breakline markers get subtle crisp survey highlight
        float core = exp(-1.2 * min(r2, 6.0));
        lit_color  = lerp(lit_color, alb_lin * 1.35 + float3(0.08), core * 0.45);
    }

    float cam_dist_m = length(input.world_pos.xy - pc.view.cam.xy);
    float haze       = smoothstep(9000.0, 26000.0, cam_dist_m) * 0.16;
    lit_color        = lerp(lit_color, float3(0.52, 0.62, 0.74), haze);

    return float4(clamp(lit_color, float3(0.0), float3(1.0)), alpha);
}
