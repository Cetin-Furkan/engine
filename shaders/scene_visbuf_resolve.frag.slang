// Slang 2026 Visibility Buffer Deferred Shading & Analytical Barycentric Resolve
// Shades each visible pixel exactly once (0% 2x2 quad helper-lane waste),
// reconstructs quantized BDA meshlet vertices, evaluates analytical screen-space
// barycentrics + partial derivatives for specular AA, and writes hardware SV_Depth.

#include "scene_common.slang"
#include "lighting.slang"

[[vk::binding(0, 0)]]
Texture2D<uint> g_vis_tex;

struct SceneDrawPush {
    uint64_t meshlets_bda;
    uint64_t vertices_bda;
    uint64_t triangles_bda;
    uint64_t cull_slot_bda;
    uint     width;
    uint     height;
    uint     phase_cmd_mode;
    uint     pad0;
    ViewPush view;
};

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

    float4 c0, c1, c2;
    float3 n0, n1, n2;
    float3 a0, a1, a2;
    float3 e0, e1, e2;
    float  r0, r1, r2;

    scene_transform_vertex(pv0, ml, inst, pc.view, pc.width, pc.height, c0, n0, a0, e0, r0);
    scene_transform_vertex(pv1, ml, inst, pc.view, pc.width, pc.height, c1, n1, a1, e1, r1);
    scene_transform_vertex(pv2, ml, inst, pc.view, pc.width, pc.height, c2, n2, a2, e2, r2);

    SceneBaryResult bary = scene_eval_barycentrics(
        c0, c1, c2, sv_pos.xy, float2(float(pc.width), float(pc.height)));
    if (!bary.valid) {
        discard;
    }

    float3 w   = bary.lambda;
    float3 nrm = n0 * w.x + n1 * w.y + n2 * w.z;
    float3 alb = a0 * w.x + a1 * w.y + a2 * w.z;
    float3 emi = e0 * w.x + e1 * w.y + e2 * w.z;
    float  rgh = r0 * w.x + r1 * w.y + r2 * w.z;

    // Analytical screen-space normal derivatives (Kaplanyan/Toksvig geometric specular AA
    // without helper-lane ddx/ddy finite differences).
    float3 dn_dx = n0 * bary.dlambda_dx.x + n1 * bary.dlambda_dx.y + n2 * bary.dlambda_dx.z;
    float3 dn_dy = n0 * bary.dlambda_dy.x + n1 * bary.dlambda_dy.y + n2 * bary.dlambda_dy.z;
    float  n_var = min(0.18, 0.35 * (dot(dn_dx, dn_dx) + dot(dn_dy, dn_dy)));
    float  aa_rgh = sqrt(clamp(rgh * rgh + n_var, 0.04, 1.0));

    SurfaceFragment surf   = { nrm, alb, emi, aa_rgh };
    float           view_z = (0.5 - bary.z_ndc - inst.depth_bias) * 4096.0;
    ResolvePSOut out;
    out.color = float4(shade_surface_gi(surf, sv_pos.xy, view_z, pc.view.materials), 1.0);
    out.depth = clamp(bary.z_ndc, 0.0, 1.0);
    return out;
}
