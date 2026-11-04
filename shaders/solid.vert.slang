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
    float origin_z;
    float squash;
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
    MeshVertex v = ((MeshVertex*)screen.verts)[vert_id];
    float3 in_pos = v.pos;
    float3 in_nrm = v.nrm;
    float3 in_albedo = v.albedo;

    const float ax = radians(28.0);
    float ay = radians(screen.yaw_deg);
    float3 posed_pos = rotate_pose(in_pos, ax, ay);
    float3 posed_nrm = rotate_pose(in_nrm, ax, ay);

    float sz = clamp(1.0 + screen.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);
    float3 p = posed_pos * (k_edge_px * 0.5);
    p.x *= sxy;
    p.y *= sxy;
    p.z *= sz;
    p.x += screen.origin_x;
    p.y += screen.origin_y;
    p.z += screen.origin_z;

    float3 scaled_nrm = normalize(float3(posed_nrm.x / sxy, posed_nrm.y / sxy, posed_nrm.z / sz));
    float3 view = view_of(p, screen.view.q, screen.view.cam);
    VSOutput out;
    out.sv_pos = view_ndc(view, screen.view.zoom, screen.width, screen.height, screen.depth_bias);
    out.v_nrm = view_rotate(screen.view.q, scaled_nrm);
    out.v_albedo = lerp(in_albedo, float3(1.0), clamp(screen.selected, 0.0, 1.0) * 0.42);
    out.v_emit = material_emit(screen.view.materials, screen.view.mat_index);
    return out;
}
