// Slang 2026 Indirect Hardware MSAA & Translucent Meshlet Fragment Shader
// Supports both legacy vertex-color solids and full Vulkan 1.4 Bindless glTF 2.0 PBR.

#include "scene_common.slang"
#include "lighting.slang"
#include "pbr_bindless.slang"

[[vk::binding(1, 0)]]
Sampler2DShadow g_shadow_map;

[[vk::push_constant]] SceneDrawPush pc;

struct MeshletPSIn {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm       : TEXCOORD0;
    [[vk::location(1)]] float4 v_tan       : TEXCOORD1;
    [[vk::location(2)]] float2 v_uv        : TEXCOORD2;
    [[vk::location(3)]] float4 v_rgba      : TEXCOORD3;
    [[vk::location(4)]] float3 v_emit      : TEXCOORD4;
    [[vk::location(5)]] float3 v_view_pos  : TEXCOORD5;
    [[vk::location(6)]] float  v_roughness : TEXCOORD6;
    [[vk::location(7)]] nointerpolation uint v_pbr_mat_id : TEXCOORD7;
    [[vk::location(8)]] nointerpolation uint v_inst_flags : TEXCOORD8;
    [[vk::location(9)]] nointerpolation uint v_cluster_id : TEXCOORD9;
};

[shader("fragment")]
float4 main(MeshletPSIn pin, bool is_front : SV_IsFrontFace) : SV_Target0 {
    bool eff_front = is_front;
    if ((pin.v_inst_flags & k_scene_flag_flip_winding) != 0u) {
        eff_front = !eff_front;
    }
    bool two_sided = (pin.v_inst_flags & k_scene_flag_two_sided) != 0u;
    if ((pc.draw_flags & 1u) != 0u && !two_sided && !eff_front) {
        discard;
    }

    float3 view_nrm = pin.v_nrm;
    if (!eff_front) {
        view_nrm = -view_nrm;
    }

    if (pin.v_pbr_mat_id == 0u || pc.pbr_mats_bda == 0ull) {
        SurfaceFragment surf = { view_nrm, pin.v_rgba.rgb, pin.v_emit, pin.v_roughness };
        return float4(shade_surface_gi_shadow_cluster(
            surf, pin.sv_pos.xy, pin.v_view_pos.z, pc.view.materials, g_shadow_map, pin.v_cluster_id), 1.0);
    }

    PbrMaterialGpu mat = ((PbrMaterialGpu *)pc.pbr_mats_bda)[pin.v_pbr_mat_id];
    float2 uv = pbr_transform_uv(pin.v_uv, mat);

    float4 base_col = mat.base_color_factor * pin.v_rgba;
    if ((mat.flags & k_pbr_flag_has_base_tex) != 0u) {
        base_col *= pbr_sample_tex(mat.tex_base, mat.samp_base, uv);
    }

    if ((mat.flags & k_pbr_flag_alpha_mask) != 0u && base_col.a < mat.alpha_cutoff) {
        discard;
    }

    float metallic  = mat.metallic_factor;
    float roughness = mat.roughness_factor;
    if ((mat.flags & k_pbr_flag_has_mr_tex) != 0u) {
        float4 mr = pbr_sample_tex(mat.tex_mr, mat.samp_mr, uv);
        roughness *= mr.g;
        metallic  *= mr.b;
    }

    float occlusion = 1.0;
    if ((mat.flags & k_pbr_flag_has_occ) != 0u) {
        float occ_s = pbr_sample_tex(mat.tex_occ, mat.samp_occ, uv).r;
        occlusion   = lerp(1.0, occ_s, clamp(mat.occlusion_strength, 0.0, 1.0));
    }

    float3 emissive = mat.emissive_factor;
    if ((mat.flags & k_pbr_flag_has_emi_tex) != 0u) {
        emissive *= pbr_sample_tex(mat.tex_emissive, mat.samp_emissive, uv).rgb;
    }

    float3 shaded_nrm = normalize(view_nrm);
    if ((mat.flags & k_pbr_flag_has_normal) != 0u) {
        float2 ts_rg   = pbr_sample_tex(mat.tex_normal, mat.samp_normal, uv).rg;
        bool   has_tan = abs(pin.v_tan.w) > 0.5;
        float  tsign   = pin.v_tan.w >= 0.0 ? 1.0 : -1.0;
        if (!eff_front) {
            tsign = -tsign;
        }
        shaded_nrm = pbr_perturb_normal(
            shaded_nrm, pin.v_tan.xyz, tsign, has_tan, ts_rg, mat.normal_scale,
            ddx(pin.v_view_pos), ddy(pin.v_view_pos), ddx(uv), ddy(uv));
    }

    PbrSurfaceFragment surf;
    surf.nrm       = shaded_nrm;
    surf.alb_lin   = base_col.rgb;
    surf.emi_lin   = emissive;
    surf.rgh       = roughness;
    surf.metallic  = metallic;
    surf.occlusion = occlusion;
    surf.ior       = mat.ior;
    surf.unlit     = (mat.flags & k_pbr_flag_unlit) != 0u;

    float3 col = shade_pbr_gi_shadow_cluster(
        surf, pin.sv_pos.xy, pin.v_view_pos.z, pc.view.materials, g_shadow_map, pin.v_cluster_id);
    float out_a = ((mat.flags & k_pbr_flag_alpha_blend) != 0u) ? clamp(base_col.a, 0.0, 1.0) : 1.0;
    return float4(col, out_a);
}
