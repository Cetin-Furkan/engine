#include "camera.slang"
#include "material.slang"
#include "face_grid.slang"

struct DrawPush {
    uint64_t verts;
    uint width;
    uint height;
    float yaw_deg;
    float origin_x;
    float origin_y;
    float depth_bias;
    uint64_t heat;
    float phase;
    float selected;
    uint64_t grid;
    uint grid_n;
    uint grid_pad;
    ViewPush view;
};
[[vk::push_constant]] DrawPush screen;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit : TEXCOORD2;
};

static const float k_edge_px = 240.0;

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    MeshVertex mv = ((MeshVertex*)screen.verts)[vert_id];
    SoftVertexState* soft = (SoftVertexState*)screen.heat;
    SoftVertexState sv = soft[vert_id];

    float3 in_pos = mv.pos + sv.disp;
    float3 in_nrm = mv.nrm;
    float3 in_albedo = mv.albedo;

    // Reconstruct deformed surface normal from neighbor displacements on face grids
    if (vert_id >= k_fillet_verts) {
        uint face, i, j;
        grid_decode(vert_id, face, i, j);
        float3 n, t, b;
        face_basis(face, n, t, b);
        uint i0 = i > 0u ? i - 1u : i;
        uint i1 = i < k_grid ? i + 1u : i;
        uint j0 = j > 0u ? j - 1u : j;
        uint j1 = j < k_grid ? j + 1u : j;
        float du = dot(soft[grid_vertex(face, i1, j)].disp - soft[grid_vertex(face, i0, j)].disp, n);
        float dv = dot(soft[grid_vertex(face, i, j1)].disp - soft[grid_vertex(face, i, j0)].disp, n);
        float step_u = float(i1 - i0) * (2.0 * (1.0 - k_radius) / float(k_grid));
        float step_v = float(j1 - j0) * (2.0 * (1.0 - k_radius) / float(k_grid));
        in_nrm = normalize(n - t * (du / max(step_u, 1e-4)) - b * (dv / max(step_v, 1e-4)));
    }

    const float ax = radians(28.0);
    float ay = radians(screen.yaw_deg);
    float3 posed_pos = rotate_pose(in_pos, ax, ay);
    float3 posed_nrm = rotate_pose(in_nrm, ax, ay);

    float3 p = posed_pos * (k_edge_px * 0.5);
    p.x += screen.origin_x;
    p.y += screen.origin_y;

    float3 view = view_of(p, screen.view.q, screen.view.cam);
    VSOutput out;
    out.sv_pos = view_ndc(view, screen.view.zoom, screen.width, screen.height, screen.depth_bias);
    out.v_nrm = view_rotate(screen.view.q, posed_nrm);

    float3 axis = float3(0.57735026919);
    float ang = 6.28318530718 * (screen.phase + dot(in_pos, float3(0.10, 0.14, 0.08)));
    float c = cos(ang);
    float s = sin(ang);
    float3 spun = in_albedo * c + cross(axis, in_albedo) * s + axis * dot(axis, in_albedo) * (1.0 - c);
    float3 graded = clamp(spun, 0.0, 1.0);
    float3 cool = graded;

    if (screen.grid != 0ul && screen.grid_n > 1u) {
        float edge = float(screen.grid_n - 1u);
        uint ix = uint(clamp(in_pos.x * 0.5 + 0.5, 0.0, 1.0) * edge);
        uint iy = uint(clamp(in_pos.y * 0.5 + 0.5, 0.0, 1.0) * edge);
        uint iz = uint(clamp(in_pos.z * 0.5 + 0.5, 0.0, 1.0) * edge);
        uint id = ix + iy * screen.grid_n + iz * screen.grid_n * screen.grid_n;
        half* field_v = (half*)screen.grid;
        float cell = clamp(float(field_v[id]), 0.0, 1.0);
        cool = lerp(graded, float3(0.45, 0.72, 1.0), cell);
    }

    float temp = clamp(sv.temp, 0.0, 1.0);
    out.v_albedo = lerp(cool, float3(1.0, 0.42, 0.12), temp);
    out.v_albedo = lerp(out.v_albedo, float3(1.0), clamp(screen.selected, 0.0, 1.0) * 0.42);
    out.v_emit = material_emit(screen.view.materials, screen.view.mat_index);

    return out;
}
