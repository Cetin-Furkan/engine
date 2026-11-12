// Slang 2026 Indirect Meshlet Depth / R32_UINT Visibility Buffer Vertex Shader

#include "scene_common.slang"

struct SceneDrawPush {
    uint64_t meshlets_bda;
    uint64_t vertices_bda;
    uint64_t triangles_bda;
    uint64_t cull_slot_bda;
    uint     width;
    uint     height;
    uint     phase_cmd_mode; // 0 = firstInstance holds pair, 1 = phase1_cmds[inst], 2 = phase2_cmds[inst]
    uint     pad0;
    ViewPush view;
};

[[vk::push_constant]] SceneDrawPush pc;

struct VisVSOut {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] nointerpolation uint vis_id : TEXCOORD0;
};

[shader("vertex")]
VisVSOut main(uint vert_id : SV_VulkanVertexID, uint vulkan_inst_id : SV_VulkanInstanceID) {
    VisVSOut out;
    out.sv_pos = float4(2.0, 2.0, 2.0, 1.0);
    out.vis_id = 0u;

    uint pair = vulkan_inst_id;
    if (pc.phase_cmd_mode != 0u) {
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

    float4 clip;
    float3 vnrm, alb, emi;
    float  rgh;
    scene_transform_vertex(pv, ml, inst, pc.view, pc.width, pc.height,
                           clip, vnrm, alb, emi, rgh);

    out.sv_pos = clip;
    out.vis_id = scene_visbuf_pack_id(inst_id, meshlet_id, prim_id);
    return out;
}
