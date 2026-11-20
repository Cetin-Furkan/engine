// Slang 2026 Hardware Directional Shadow Map Vertex Shader for Unified Scene Meshlets
// Renders all active meshlet instances directly from the shared BDA geometry slab in 1 draw.

#include "scene_common.slang"

struct SceneShadowPush {
    uint64_t meshlets_bda;
    uint64_t vertices_bda;
    uint64_t triangles_bda;
    uint64_t cull_slot_bda;
    float4   vp_row0;
    float4   vp_row1;
    float4   vp_row2;
    float4   vp_row3;
};

[[vk::push_constant]] SceneShadowPush pc;

[shader("vertex")]
float4 main(uint vert_id : SV_VulkanVertexID, uint vulkan_inst_id : SV_VulkanInstanceID) : SV_Position {
    uint pair       = ((uint *)(pc.cull_slot_bda + k_slot_off_work))[vulkan_inst_id];
    uint inst_id    = pair >> 16u;
    uint meshlet_id = pair & 0xFFFFu;
    uint prim_id    = vert_id / 3u;
    uint corner     = vert_id % 3u;

    SceneMeshlet ml = ((SceneMeshlet *)pc.meshlets_bda)[meshlet_id];
    if (prim_id >= uint(ml.tri_count)) {
        return float4(2.0, 2.0, 2.0, 1.0);
    }

    uint packed_tri       = ((uint *)pc.triangles_bda)[ml.tri_offset + prim_id];
    uint local_v          = (packed_tri >> (corner * 8u)) & 0xFFu;
    ScenePackedVertex pv  = ((ScenePackedVertex *)pc.vertices_bda)[ml.vert_offset + local_v];
    SceneInstance    inst = ((SceneInstance *)(pc.cull_slot_bda + k_slot_off_instances))[inst_id];

    float3 local_p = scene_dequantize_pos(pv, ml);
    float3 world_p = scene_transform_point_squash(local_p, inst);

    float4 wp = float4(world_p, 1.0);
    return float4(dot(pc.vp_row0, wp),
                  dot(pc.vp_row1, wp),
                  dot(pc.vp_row2, wp),
                  dot(pc.vp_row3, wp));
}
