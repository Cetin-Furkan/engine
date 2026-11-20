// Slang 2026 Indirect Hardware MSAA & Translucent Meshlet Vertex Shader

#include "scene_common.slang"

[[vk::push_constant]] SceneDrawPush pc;

struct MeshletVSOut {
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

[shader("vertex")]
MeshletVSOut main(uint vert_id : SV_VulkanVertexID, uint vulkan_inst_id : SV_VulkanInstanceID) {
    MeshletVSOut out;
    out.sv_pos       = float4(2.0, 2.0, 2.0, 1.0);
    out.v_nrm        = float3(0.0, 1.0, 0.0);
    out.v_tan        = float4(1.0, 0.0, 0.0, 0.0);
    out.v_uv         = float2(0.0, 0.0);
    out.v_rgba       = float4(1.0);
    out.v_emit       = float3(0.0);
    out.v_view_pos   = float3(0.0);
    out.v_roughness  = 0.35;
    out.v_pbr_mat_id = 0u;
    out.v_inst_flags = 0u;
    out.v_cluster_id = 1u;

    uint pair = vulkan_inst_id;
    if (pc.phase_cmd_mode == 3u) {
        pair = ((uint *)(pc.cull_slot_bda + k_slot_off_blend))[vulkan_inst_id];
    } else if (pc.phase_cmd_mode != 0u) {
        SceneDrawIndirectCmd *cmds = (pc.phase_cmd_mode == 1u)
            ? (SceneDrawIndirectCmd *)(pc.cull_slot_bda + k_slot_off_cmds1)
            : (SceneDrawIndirectCmd *)(pc.cull_slot_bda + k_slot_off_cmds2);
        SceneDrawIndirectCmd cmd = cmds[vulkan_inst_id];
        if (vert_id >= cmd.vertexCount) {
            return out;
        }
        pair = cmd.firstInstance;
    }

    uint inst_id    = pair >> 16u;
    uint meshlet_id = pair & 0xFFFFu;
    uint prim_id    = vert_id / 3u;
    uint corner     = vert_id % 3u;

    SceneMeshlet ml = ((SceneMeshlet *)pc.meshlets_bda)[meshlet_id];
    if (prim_id >= uint(ml.tri_count)) {
        return out;
    }

    uint packed_tri = ((uint *)pc.triangles_bda)[ml.tri_offset + prim_id];
    uint local_v    = (packed_tri >> (corner * 8u)) & 0xFFu;
    ScenePackedVertex pv   = ((ScenePackedVertex *)pc.vertices_bda)[ml.vert_offset + local_v];
    SceneInstance     inst = ((SceneInstance *)(pc.cull_slot_bda + k_slot_off_instances))[inst_id];

    SceneVertXform xf = scene_transform_vertex_pbr(pv, ml, inst, pc.view, pc.width, pc.height);
    out.sv_pos       = xf.clip;
    out.v_nrm        = xf.view_nrm;
    out.v_tan        = float4(xf.view_tan, xf.has_tan ? xf.bitan_sign : 0.0);
    out.v_uv         = xf.uv;
    out.v_rgba       = xf.rgba;
    out.v_emit       = xf.emit;
    out.v_view_pos   = xf.view_pos;
    out.v_roughness  = xf.roughness;
    out.v_pbr_mat_id = inst.pbr_mat_id;
    out.v_inst_flags = inst.flags;
    out.v_cluster_id = (inst_id << 11u) ^ (meshlet_id + 1u);
    return out;
}
