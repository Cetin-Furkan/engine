// Slang 2026 Indirect Hardware MSAA Meshlet Vertex Shader

#include "scene_common.slang"

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

struct MeshletVSOut {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm       : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo    : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit      : TEXCOORD2;
    [[vk::location(3)]] float  v_roughness : TEXCOORD3;
};

[shader("vertex")]
MeshletVSOut main(uint vert_id : SV_VulkanVertexID, uint vulkan_inst_id : SV_VulkanInstanceID) {
    MeshletVSOut out;
    out.sv_pos      = float4(2.0, 2.0, 2.0, 1.0);
    out.v_nrm       = float3(0.0, 1.0, 0.0);
    out.v_albedo    = float3(0.0);
    out.v_emit      = float3(0.0);
    out.v_roughness = 0.35;

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

    scene_transform_vertex(pv, ml, inst, pc.view, pc.width, pc.height,
                           out.sv_pos, out.v_nrm, out.v_albedo, out.v_emit, out.v_roughness);
    return out;
}
