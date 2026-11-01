#include "camera.slang"
#include "material.slang"

struct ScreenPush {
    uint64_t verts;
    uint width;
    uint height;
    float yaw_deg;
    float origin_x;
    float origin_y;
    float depth_bias;
    float selected;
    uint pad;
    uint tail0;
    uint tail1;
    ViewPush view;
};
[[vk::push_constant]] ScreenPush screen;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit : TEXCOORD2;
};

static const float k_edge_px = 168.0;

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    float* mesh = (float*)screen.verts;
    uint o = vert_id * 9u;
    float3 in_pos = float3(mesh[o + 0u], mesh[o + 1u], mesh[o + 2u]);
    float3 in_nrm = float3(mesh[o + 3u], mesh[o + 4u], mesh[o + 5u]);
    float3 in_albedo = float3(mesh[o + 6u], mesh[o + 7u], mesh[o + 8u]);

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
    out.v_albedo = lerp(in_albedo, float3(1.0), clamp(screen.selected, 0.0, 1.0) * 0.42);
    out.v_emit = material_emit(screen.view.materials, screen.view.mat_index);
    return out;
}
