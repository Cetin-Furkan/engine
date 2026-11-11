// Slang 2026 GPU-Driven Frustum + Elevation AABB Culling Compute Shader:
// Evaluates active RelPtr2D quadtree leaves against the 3D camera frustum,
// compacts visible leaf node indices into BDA memory, and emits VkDrawIndirectCommand
// records + draw_count for vkCmdDrawIndirectCount.

#include "camera.slang"
import terrain_math;

struct CullPush {
    uint64_t dir_bda;
    uint64_t indirect_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    float    elev_base_m;
    float    vertical_scale;
    uint     width;
    uint     height;
    uint     leaf_count;
    uint     tile_verts;
};

[[vk::push_constant]] CullPush pc;

groupshared uint s_vis[64];
groupshared uint s_node[64];
groupshared uint s_verts[64];

bool tile_aabb_in_frustum(GpuMathQuadNodeGpu node, out uint out_tile_verts) {
    out_tile_verts = pc.tile_verts;
    // Unpack Lilliput 64-bit unified tile header quantized min/max elevation (bits [9:0] and [19:10], 2m step)
    float min_elev_m = float(node.header & 0x3FFu) * 2.0;
    float max_elev_m = float((node.header >> 10u) & 0x3FFu) * 2.0;
    if (max_elev_m <= min_elev_m) {
        min_elev_m = 800.0;
        max_elev_m = 1720.0;
    } else {
        min_elev_m = max(min_elev_m - 32.0, 0.0);
        max_elev_m = max_elev_m + 32.0;
    }

    float z0 = (min_elev_m - pc.elev_base_m) * pc.vertical_scale;
    float z1 = (max_elev_m - pc.elev_base_m) * pc.vertical_scale;

    float4 q   = float4(pc.qx, pc.qy, pc.qz, pc.qw);
    float3 cam = float3(pc.cam_x, pc.cam_y, pc.cam_z);

    float3 ndc_min = float3( 1.0e9,  1.0e9,  1.0e9);
    float3 ndc_max = float3(-1.0e9, -1.0e9, -1.0e9);

    [unroll]
    for (uint corner = 0u; corner < 8u; ++corner) {
        float wx = (corner & 1u) != 0u ? node.world_max_x : node.world_min_x;
        float wy = (corner & 2u) != 0u ? node.world_max_y : node.world_min_y;
        float wz = (corner & 4u) != 0u ? z1 : z0;

        float3 vpos = view_of(float3(wx, wy, wz), q, cam);
        float4 ndc  = view_ndc(vpos, pc.zoom, pc.width, pc.height, 0.0);
        float  ndc_z = 0.5 - vpos.z / 65536.0;
        float3 p = float3(ndc.x, ndc.y, ndc_z);
        ndc_min = min(ndc_min, p);
        ndc_max = max(ndc_max, p);
    }

    const float k_guard = 1.08;
    if (ndc_max.x < -k_guard || ndc_min.x > k_guard ||
        ndc_max.y < -k_guard || ndc_min.y > k_guard ||
        ndc_max.z < 0.0      || ndc_min.z > 1.0) {
        return false;
    }

    // Distance-Adaptive CDLOD Screen-Space Vertex Decimation (+ 64 watertight skirt quads):
    // Choose 64x64+64 (24,960v), 32x32+64 (6,528v), or 16x16+64 (1,920v) based on projected NDC span
    float2 ndc_span = ndc_max.xy - ndc_min.xy;
    float  px_span  = max(ndc_span.x * float(pc.width), ndc_span.y * float(pc.height)) * 0.5;
    if (px_span < 160.0) {
        out_tile_verts = min(pc.tile_verts, (16u * 16u + k_skirt_quads) * 6u);
    } else if (px_span < 340.0) {
        out_tile_verts = min(pc.tile_verts, (32u * 32u + k_skirt_quads) * 6u);
    } else {
        out_tile_verts = pc.tile_verts;
    }
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (pc.dir_bda == 0ull || pc.indirect_bda == 0ull) {
        return;
    }

    uint count  = min(pc.leaf_count, 64u);
    uint vis    = 0u;
    uint nidx   = 0u;
    uint tverts = pc.tile_verts;

    if (i < count) {
        nidx = load_active_leaf_index(pc.dir_bda, i);
        GpuMathQuadNodeGpu node = load_quad_node(pc.dir_bda, nidx);
        vis = tile_aabb_in_frustum(node, tverts) ? 1u : 0u;
    }

    s_vis[i]   = vis;
    s_node[i]  = nidx;
    s_verts[i] = tverts;
    GroupMemoryBarrierWithGroupSync();

    uint *words = (uint *)pc.indirect_bda;

    if (i < count && vis != 0u) {
        uint slot = 0u;
        for (uint j = 0u; j < i; ++j) {
            slot += s_vis[j];
        }
        // visible_nodes[] starts at byte offset 32 (word 8)
        words[8u + slot] = nidx;
        // cmds[] starts at byte offset 288 (word 72), 4 words (16B) per VkDrawIndirectCommand
        uint cmd_word = 72u + slot * 4u;
        words[cmd_word + 0u] = s_verts[i];
        words[cmd_word + 1u] = 1u;
        words[cmd_word + 2u] = 0u;
        words[cmd_word + 3u] = slot;
    }

    if (i == 0u) {
        uint total_vis = 0u;
        for (uint j = 0u; j < count; ++j) {
            total_vis += s_vis[j];
        }
        // Header words 0..3: draw_count, culled_count, active_leaf_total, pad0
        words[0u] = total_vis;
        words[1u] = count - total_vis;
        words[2u] = count;
        words[3u] = 0u;
        // Fallback single_cmd at byte offset 16 (words 4..7)
        words[4u] = pc.tile_verts;
        words[5u] = total_vis;
        words[6u] = 0u;
        words[7u] = 0u;
    }
}
