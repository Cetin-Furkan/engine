#include "camera.slang"
#include "material.slang"

struct ViewPush {
    uint64_t materials;
    uint mat_index;
    uint mat_pad;
    float4 q;
    float3 cam;
    float zoom;
};

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
VSOutput main(uint vert_id : SV_VertexID) {
    float* mesh_f = (float*)screen.verts;
    uint o = vert_id * 9u;
    float3 in_pos = float3(mesh_f[o + 0u], mesh_f[o + 1u], mesh_f[o + 2u]);
    float3 in_nrm = float3(mesh_f[o + 3u], mesh_f[o + 4u], mesh_f[o + 5u]);
    float3 in_albedo = float3(mesh_f[o + 6u], mesh_f[o + 7u], mesh_f[o + 8u]);

    const float ax = radians(28.0);
    float ay = radians(screen.yaw_deg);
    float3x3 rx = float3x3(1.0, 0.0, 0.0,
                           0.0, cos(ax), sin(ax),
                           0.0, -sin(ax), cos(ax));
    float3x3 ry = float3x3(cos(ay), 0.0, -sin(ay),
                           0.0, 1.0, 0.0,
                           sin(ay), 0.0, cos(ay));
    float3x3 pose = mul(ry, rx);
    float3 p = mul(pose, in_pos) * (k_edge_px * 0.5);
    p.x += screen.origin_x;
    p.y += screen.origin_y;

    float3 view = view_of(p, screen.view.q, screen.view.cam);
    VSOutput out;
    out.sv_pos = view_ndc(view, screen.view.zoom, screen.width, screen.height, screen.depth_bias);
    out.v_nrm = view_rotate(screen.view.q, mul(pose, in_nrm));

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

    float* heat_t = (float*)screen.heat;
    float temp = clamp(heat_t[vert_id], 0.0, 1.0);
    out.v_albedo = lerp(cool, float3(1.0, 0.42, 0.12), temp);
    out.v_albedo = lerp(out.v_albedo, float3(1.0), clamp(screen.selected, 0.0, 1.0) * 0.42);
    out.v_emit = material_emit(screen.view.materials, screen.view.mat_index);

    return out;
}
