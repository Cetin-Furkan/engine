// Slang 2026 Adaptive Quadtree Terrain Vertex Shader
// Reads the 256-byte PULSE Active GPU Window and 149 RelPtr2D Quadtree Nodes
// directly via Vulkan 1.4 Buffer Device Address (BDA) in 1:1 Metric Coordinates (1 unit = 1 meter).

#include "camera.slang"
import terrain_math;

struct TerrainPush {
    uint64_t dir_bda;
    uint64_t indirect_bda;
    uint     width;
    uint     height;
    float    elev_base_m;
    float    vertical_scale;
    float    marker_x;
    float    marker_y;
    float    marker_active;
    float    marker_b_x;
    float    marker_b_y;
    float    marker_b_active;
    float    cell_med;
    float    coarse;
    uint     flags;
    uint     pad0;
    ViewPush view;
};

[[vk::push_constant]] TerrainPush pc;

struct VSOutput {
    float4   sv_pos       : SV_Position;
    float3   view_nrm     : TEXCOORD0;
    float3   world_pos    : TEXCOORD1;
    float2   tile_uv      : TEXCOORD2;
    float3   elev_span    : TEXCOORD3; // x = elev_m, y = resolved_tile_world_span, z = view_pos.z
    nointerpolation uint64_t tile_rel_ptr : TEXCOORD4;
};

static const uint k_default_quads_per_side = 64u;
static const uint2 k_tri_corner[6] = {
    uint2(0u, 0u), uint2(1u, 0u), uint2(0u, 1u),
    uint2(1u, 0u), uint2(1u, 1u), uint2(0u, 1u)
};

// Walk up the 5-level quadtree (LOD 0..4) if the leaf tile's SSD block is not yet resident,
// remapping local tile UV into the resident ancestor tile.
void resolve_resident_tile(uint64_t dir_bda,
                           GpuMathQuadNodeGpu leaf,
                           inout float2 uv,
                           out uint64_t out_rel_ptr,
                           out uint64_t out_tile_bda,
                           out float    out_span_world) {
    GpuMathQuadNodeGpu cur = leaf;
    for (uint hop = 0u; hop < 5u; ++hop) {
        uint resident = (cur.packed_ids2 >> 8u) & 0xFFu;
        if (resident != 0u && cur.rel_gpu_payload != 0ull) {
            out_rel_ptr    = cur.rel_gpu_payload;
            out_tile_bda   = relptr2d_resolve_gpu(dir_bda, cur.rel_gpu_payload);
            out_span_world = max(cur.world_max_x - cur.world_min_x, 1.0);
            return;
        }
        uint cur_lod = (cur.packed_ids1 >> 16u) & 0xFFu;
        if (cur_lod == 0u) {
            break;
        }
        uint gx = (cur.packed_ids1 >> 24u) & 0xFFu;
        uint gy = cur.packed_ids2 & 0xFFu;
        // Remap UV into parent quadrant (gy=0 is North/+Y, uv.y=1 is North/+Y)
        uv.x = (uv.x + float(gx & 1u)) * 0.5;
        uv.y = (uv.y + float(1u - (gy & 1u))) * 0.5;
        uint parent_idx = (cur.packed_ids0 >> 16u) & 0xFFFFu;
        cur = load_quad_node(dir_bda, parent_idx);
    }
    GpuMathQuadNodeGpu root = load_quad_node(dir_bda, 0u);
    out_rel_ptr    = root.rel_gpu_payload;
    out_tile_bda   = relptr2d_resolve_gpu(dir_bda, root.rel_gpu_payload);
    out_span_world = max(root.world_max_x - root.world_min_x, 1.0);
}

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID, uint inst_id : SV_VulkanInstanceID) {
    VSOutput out;
    uint quads_per_side = load_visible_leaf_quads_side(pc.indirect_bda, inst_id, k_default_quads_per_side);
    uint surface_quads  = quads_per_side * quads_per_side;
    uint quad_id        = vert_id / 6u;
    uint corner_id      = vert_id % 6u;
    uint2 tri_c         = k_tri_corner[corner_id];

    uint node_idx = load_visible_leaf_index(pc.dir_bda, pc.indirect_bda, inst_id);
    GpuMathQuadNodeGpu node = load_quad_node(pc.dir_bda, node_idx);

    float2 local_uv     = float2(0.0);
    float  skirt_drop_z = 0.0;

    if (quad_id < surface_quads) {
        uint qx       = quad_id % quads_per_side;
        uint qy       = quad_id / quads_per_side;
        uint2 grid_pt = min(uint2(qx, qy) + tri_c, uint2(quads_per_side));

        // Stitch boundary vertices across both LOD transitions (0..4) and adaptive mesh resolutions (64, 32, 16)
        // by snapping tile edges to a universal 16-segment edge lattice matched 1:1 with the perimeter skirt
        uint2 sample_pt    = grid_pt;
        uint edge_segments = 16u;
        uint edge_step     = max(quads_per_side / edge_segments, 1u);
        bool on_x_edge     = (grid_pt.x == 0u || grid_pt.x == quads_per_side);
        bool on_y_edge     = (grid_pt.y == 0u || grid_pt.y == quads_per_side);
        if (on_x_edge && grid_pt.y < quads_per_side) {
            sample_pt.y = (grid_pt.y / edge_step) * edge_step;
        }
        if (on_y_edge && grid_pt.x < quads_per_side) {
            sample_pt.x = (grid_pt.x / edge_step) * edge_step;
        }
        local_uv = float2(sample_pt) / float(quads_per_side);
    } else {
        // 4-edge 16-segment watertight vertical skirt (64 quads = 384 vertices: 16 per S/N/W/E border)
        // Welded 1:1 to the 16-segment surface border on top (tri_c.y == 0) and extending 28m down on bottom (tri_c.y == 1)
        uint skirt_id = min(quad_id - surface_quads, 63u);
        uint edge_dir = skirt_id / 16u;
        uint seg_idx  = skirt_id % 16u;
        float t_along = float(seg_idx + tri_c.x) / 16.0;
        if (edge_dir == 0u) {
            local_uv = float2(t_along, 0.0);
        } else if (edge_dir == 1u) {
            local_uv = float2(t_along, 1.0);
        } else if (edge_dir == 2u) {
            local_uv = float2(0.0, t_along);
        } else {
            local_uv = float2(1.0, t_along);
        }
        if (tri_c.y != 0u) {
            skirt_drop_z = 28.0;
        }
    }

    float2 world_xy = float2(
        lerp(node.world_min_x, node.world_max_x, local_uv.x),
        lerp(node.world_min_y, node.world_max_y, local_uv.y)
    );

    float2 resolved_uv = local_uv;
    uint64_t tile_rel  = 0ull;
    uint64_t tile_bda  = 0ull;
    float resolved_span = max(node.world_max_x - node.world_min_x, 1.0);
    resolve_resident_tile(pc.dir_bda, node, resolved_uv, tile_rel, tile_bda, resolved_span);

    float elev_m = pc.elev_base_m;
    float3 world_nrm = float3(0.0, 0.0, 1.0);

    if (tile_bda != 0ull) {
        float2 dh_duv = float2(0.0);
        // Fast 4-tap bilinear vertex displacement (fragment shader evaluates full continuous normal per pixel)
        elev_m = tile_sample_bilinear_height_grad_m(tile_bda, resolved_uv, dh_duv);
        float dzdx = (dh_duv.x / resolved_span) * pc.vertical_scale;
        float dzdy = (dh_duv.y / resolved_span) * pc.vertical_scale;
        world_nrm = normalize(float3(-dzdx, -dzdy, 1.0));
    }

    float world_z = (elev_m - pc.elev_base_m) * pc.vertical_scale - skirt_drop_z;
    float3 world_pos = float3(world_xy.x, world_xy.y, world_z);
    float3 view_pos  = view_of(world_pos, pc.view.q, pc.view.cam);

    float4 ndc       = view_ndc(view_pos, pc.view.zoom, pc.width, pc.height, 0.0);
    ndc.z            = clamp(0.5 - view_pos.z / 65536.0, 0.001, 0.999);
    out.sv_pos       = ndc;
    out.view_nrm     = normalize(view_rotate(pc.view.q, world_nrm));
    out.world_pos    = world_pos;
    out.tile_uv      = resolved_uv;
    out.elev_span    = float3(elev_m, resolved_span, view_pos.z);
    out.tile_rel_ptr = tile_rel;
    return out;
}
