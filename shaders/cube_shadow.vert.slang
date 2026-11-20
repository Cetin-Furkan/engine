// Slang 2026 Hardware Directional Shadow Map Vertex Shader for Deformable Fillet Cubes
// Reads the live displaced soft-body vertex state via BDA so dented cubes cast exact shadows.

#include "camera.slang"
#include "face_grid.slang"

struct CubeShadowPush {
    uint64_t verts;
    uint64_t heat;
    float    yaw_deg;
    float    origin_x;
    float    origin_y;
    float    origin_z;
    float4   vp_row0;
    float4   vp_row1;
    float4   vp_row2;
    float4   vp_row3;
};

[[vk::push_constant]] CubeShadowPush pc;

static const float k_edge_px = 240.0;

[shader("vertex")]
float4 main(uint vert_id : SV_VulkanVertexID) : SV_Position {
    MeshVertex mv = ((MeshVertex *)pc.verts)[vert_id];
    SoftVertexState *soft = (SoftVertexState *)pc.heat;
    float3 disp = soft[vert_id].disp;

    if (vert_id >= k_grid_verts) {
        uint e, len, arc;
        seam_decode(vert_id, e, len, arc);
        float u = float(arc) / float(k_arc);
        disp = lerp(soft[face_boundary_vertex(e, len, 0u)].disp,
                    soft[face_boundary_vertex(e, len, 1u)].disp, u);
    } else if (vert_id >= k_face_corners && vert_id < k_corner_base) {
        uint local = vert_id - k_face_corners;
        uint span  = k_edge_cols * 2u;
        uint e     = local / span;
        uint rem   = local - e * span;
        uint row   = rem / k_edge_cols;
        uint col   = rem - row * k_edge_cols;
        uint len   = row == 0u ? 0u : k_grid;
        float u    = float(col) / float(k_arc);
        disp = lerp(soft[face_boundary_vertex(e, len, 0u)].disp,
                    soft[face_boundary_vertex(e, len, 1u)].disp, u);
    } else if (vert_id >= k_corner_base && vert_id < k_fillet_verts) {
        uint local = vert_id - k_corner_base;
        uint c     = local / k_corner_samples;
        uint rem   = local - c * k_corner_samples;
        uint iv    = rem / k_edge_cols;
        uint iu    = rem - iv * k_edge_cols;
        float3 n0, n1, n2;
        corner_basis(c, n0, n1, n2);
        float3 d0 = soft[face_corner_vertex(n0, n1, n2)].disp;
        float3 d1 = soft[face_corner_vertex(n1, n0, n2)].disp;
        float3 d2 = soft[face_corner_vertex(n2, n0, n1)].disp;
        float ua  = float(iu) / float(k_arc);
        float ub  = float(iv) / float(k_arc);
        disp = lerp(lerp(d0, d1, ua), d2, ub);
    } else if (vert_id < k_fillet_verts) {
        uint face, i, j;
        if (face_corner_at(mv.pos, face, i, j)) {
            uint canon = grid_vertex(face, i, j);
            if (canon != vert_id) {
                disp = soft[canon].disp;
            }
        }
    }

    float3 in_pos    = mv.pos + disp;
    const float ax   = radians(28.0);
    float ay         = radians(pc.yaw_deg);
    float3 posed_pos = rotate_pose(in_pos, ax, ay);
    float3 world_p   = posed_pos * (k_edge_px * 0.5) + float3(pc.origin_x, pc.origin_y, pc.origin_z);

    float4 wp = float4(world_p, 1.0);
    return float4(dot(pc.vp_row0, wp),
                  dot(pc.vp_row1, wp),
                  dot(pc.vp_row2, wp),
                  dot(pc.vp_row3, wp));
}
