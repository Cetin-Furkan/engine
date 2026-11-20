// Slang 2026 Visibility Buffer Deferred Shading & Analytical Barycentric Resolve
// Shades each visible pixel exactly once (0% 2x2 quad helper-lane waste),
// reconstructs 32B quantized BDA meshlet vertices, evaluates analytical screen-space
// barycentrics + analytical dUV/dx, dUV/dy for anisotropic SampleGrad bindless PBR,
// Kaplanyan/Toksvig geometric specular AA, hardware shadows, and writes SV_Depth.

#include "scene_common.slang"
#include "lighting.slang"
#include "pbr_bindless.slang"

[[vk::binding(0, 0)]]
Texture2D<uint> g_vis_tex;

[[vk::binding(1, 0)]]
Sampler2DShadow g_shadow_map;

[[vk::push_constant]] SceneDrawPush pc;

struct ResolvePSOut {
    float4 color : SV_Target0;
    float  depth : SV_Depth;
};

[shader("fragment")]
ResolvePSOut main(float4 sv_pos : SV_Position) {
    int2 pix = int2(sv_pos.xy);
    uint vis_id = g_vis_tex.Load(int3(pix, 0));

    uint inst_id, meshlet_id, prim_id;
    if (!scene_visbuf_unpack_id(vis_id, inst_id, meshlet_id, prim_id)) {
        discard;
    }

    SceneMeshlet  ml   = ((SceneMeshlet *)pc.meshlets_bda)[meshlet_id];
    SceneInstance inst = ((SceneInstance *)(pc.cull_slot_bda + k_slot_off_instances))[inst_id];
    uint packed_tri    = ((uint *)pc.triangles_bda)[ml.tri_offset + prim_id];

    uint i0 =  packed_tri         & 0xFFu;
    uint i1 = (packed_tri >>  8u) & 0xFFu;
    uint i2 = (packed_tri >> 16u) & 0xFFu;

    ScenePackedVertex *vbase = (ScenePackedVertex *)pc.vertices_bda;
    ScenePackedVertex pv0 = vbase[ml.vert_offset + i0];
    ScenePackedVertex pv1 = vbase[ml.vert_offset + i1];
    ScenePackedVertex pv2 = vbase[ml.vert_offset + i2];

    SceneVertXform x0 = scene_transform_vertex_pbr(pv0, ml, inst, pc.view, pc.width, pc.height);
    SceneVertXform x1 = scene_transform_vertex_pbr(pv1, ml, inst, pc.view, pc.width, pc.height);
    SceneVertXform x2 = scene_transform_vertex_pbr(pv2, ml, inst, pc.view, pc.width, pc.height);

    SceneBaryResult bary = scene_eval_barycentrics(
        x0.clip, x1.clip, x2.clip, sv_pos.xy, float2(float(pc.width), float(pc.height)));
    if (!bary.valid) {
        discard;
    }

    bool eff_front = bary.front_face;
    if ((inst.flags & k_scene_flag_flip_winding) != 0u) {
        eff_front = !eff_front;
    }

    float3 w   = bary.lambda;
    float3 nrm = x0.view_nrm * w.x + x1.view_nrm * w.y + x2.view_nrm * w.z;
    if (!eff_front) {
        nrm = -nrm;
    }
    float4 rgba = x0.rgba * w.x + x1.rgba * w.y + x2.rgba * w.z;
    float  vz   = x0.view_pos.z * w.x + x1.view_pos.z * w.y + x2.view_pos.z * w.z;

    // Analytical screen-space normal derivatives (Kaplanyan/Toksvig geometric specular AA)
    float3 dn_dx = x0.view_nrm * bary.dlambda_dx.x + x1.view_nrm * bary.dlambda_dx.y + x2.view_nrm * bary.dlambda_dx.z;
    float3 dn_dy = x0.view_nrm * bary.dlambda_dy.x + x1.view_nrm * bary.dlambda_dy.y + x2.view_nrm * bary.dlambda_dy.z;
    float  n_var = min(0.18, 0.35 * (dot(dn_dx, dn_dx) + dot(dn_dy, dn_dy)));

    uint cluster_id = (inst_id << 11u) ^ (meshlet_id + 1u);
    ResolvePSOut out;
    out.depth = clamp(bary.z_ndc, 0.0, 1.0);

    if (inst.pbr_mat_id == 0u || pc.pbr_mats_bda == 0ull) {
        float3 emi    = x0.emit * w.x + x1.emit * w.y + x2.emit * w.z;
        float  rgh    = x0.roughness * w.x + x1.roughness * w.y + x2.roughness * w.z;
        float  aa_rgh = sqrt(clamp(rgh * rgh + n_var, 0.04, 1.0));
        SurfaceFragment surf = { nrm, rgba.rgb, emi, aa_rgh };
        out.color = float4(shade_surface_gi_shadow_cluster(
            surf, sv_pos.xy, vz, pc.view.materials, g_shadow_map, cluster_id), 1.0);
        return out;
    }

    // Full Bindless PBR with Analytical UV Gradients (SampleGrad — zero quad helper-lane waste!)
    PbrMaterialGpu mat = ((PbrMaterialGpu *)pc.pbr_mats_bda)[inst.pbr_mat_id];
    float2 raw_uv  = x0.uv * w.x + x1.uv * w.y + x2.uv * w.z;
    float2 raw_dux = x0.uv * bary.dlambda_dx.x + x1.uv * bary.dlambda_dx.y + x2.uv * bary.dlambda_dx.z;
    float2 raw_duy = x0.uv * bary.dlambda_dy.x + x1.uv * bary.dlambda_dy.y + x2.uv * bary.dlambda_dy.z;

    float2 uv  = pbr_transform_uv(raw_uv, mat);
    float2 dux = pbr_transform_duv(raw_dux, mat);
    float2 duy = pbr_transform_duv(raw_duy, mat);

    float4 base_col = mat.base_color_factor * rgba;
    if ((mat.flags & k_pbr_flag_has_base_tex) != 0u) {
        base_col *= pbr_sample_tex_grad(mat.tex_base, mat.samp_base, uv, dux, duy);
    }

    float metallic  = mat.metallic_factor;
    float roughness = mat.roughness_factor;
    if ((mat.flags & k_pbr_flag_has_mr_tex) != 0u) {
        float4 mr = pbr_sample_tex_grad(mat.tex_mr, mat.samp_mr, uv, dux, duy);
        roughness *= mr.g;
        metallic  *= mr.b;
    }
    float aa_rgh = sqrt(clamp(roughness * roughness + n_var, 0.002, 1.0));

    float occlusion = 1.0;
    if ((mat.flags & k_pbr_flag_has_occ) != 0u) {
        float occ_s = pbr_sample_tex_grad(mat.tex_occ, mat.samp_occ, uv, dux, duy).r;
        occlusion   = lerp(1.0, occ_s, clamp(mat.occlusion_strength, 0.0, 1.0));
    }

    float3 emissive = mat.emissive_factor;
    if ((mat.flags & k_pbr_flag_has_emi_tex) != 0u) {
        emissive *= pbr_sample_tex_grad(mat.tex_emissive, mat.samp_emissive, uv, dux, duy).rgb;
    }

    float3 shaded_nrm = normalize(nrm);
    if ((mat.flags & k_pbr_flag_has_normal) != 0u) {
        float2 ts_rg   = pbr_sample_tex_grad(mat.tex_normal, mat.samp_normal, uv, dux, duy).rg;
        float3 vtan    = x0.view_tan * w.x + x1.view_tan * w.y + x2.view_tan * w.z;
        bool   has_tan = x0.has_tan && x1.has_tan && x2.has_tan;
        float  tsign   = x0.bitan_sign;
        if (!eff_front) {
            tsign = -tsign;
        }
        float3 dp_dx = x0.view_pos * bary.dlambda_dx.x + x1.view_pos * bary.dlambda_dx.y + x2.view_pos * bary.dlambda_dx.z;
        float3 dp_dy = x0.view_pos * bary.dlambda_dy.x + x1.view_pos * bary.dlambda_dy.y + x2.view_pos * bary.dlambda_dy.z;
        shaded_nrm = pbr_perturb_normal(
            shaded_nrm, vtan, tsign, has_tan, ts_rg, mat.normal_scale,
            dp_dx, dp_dy, dux, duy);
    }

    PbrSurfaceFragment surf;
    surf.nrm       = shaded_nrm;
    surf.alb_lin   = base_col.rgb;
    surf.emi_lin   = emissive;
    surf.rgh       = aa_rgh;
    surf.metallic  = metallic;
    surf.occlusion = occlusion;
    surf.ior       = mat.ior;
    surf.unlit     = (mat.flags & k_pbr_flag_unlit) != 0u;

    out.color = float4(shade_pbr_gi_shadow_cluster(
        surf, sv_pos.xy, vz, pc.view.materials, g_shadow_map, cluster_id), 1.0);
    return out;
}
