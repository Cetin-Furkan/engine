#include "camera.slang"

struct ViewPush {
    uint64_t materials;
    uint mat_index;
    uint mat_pad;
    float4 q;
    float3 cam;
    float zoom;
};

struct ScreenPush {
    uint width;
    uint height;
    float cell_med;
    float level_frac;
    float origin_x;
    float origin_y;
    float span;
    float span_mod;
    float span_steps;
    float pad2;
    uint  pad3;
    uint  pad4;
    ViewPush view;
};
[[vk::push_constant]] ScreenPush pc;

float2 pixel_footprint(float4 q, float zoom) {
    float3 rx = view_rotate(q, float3(1.0, 0.0, 0.0));
    float3 ry = view_rotate(q, float3(0.0, 1.0, 0.0));
    float2 dp_dwx = float2(rx.x, -rx.y) * zoom;
    float2 dp_dwy = float2(ry.x, -ry.y) * zoom;
    float det = dp_dwx.x * dp_dwy.y - dp_dwy.x * dp_dwx.y;
    float sdet = det >= 0.0 ? 1.0 : -1.0;
    float inv_det = sdet / max(abs(det), 1.0e-8);
    float dwx_dpx = dp_dwy.y * inv_det;
    float dwx_dpy = -dp_dwy.x * inv_det;
    float dwy_dpx = -dp_dwx.y * inv_det;
    float dwy_dpy = dp_dwx.x * inv_det;
    return float2(abs(dwx_dpx) + abs(dwx_dpy), abs(dwy_dpx) + abs(dwy_dpy));
}

static const float2 k_quad[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)
};

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float2 v_c : TEXCOORD0;
    [[vk::location(1)]] nointerpolation float v_cell_med : TEXCOORD1;
    [[vk::location(2)]] nointerpolation float v_frac : TEXCOORD2;
    [[vk::location(3)]] nointerpolation float2 v_cam_mod : TEXCOORD3;
    [[vk::location(4)]] nointerpolation float v_span_steps : TEXCOORD4;
    [[vk::location(5)]] nointerpolation float v_span_mod : TEXCOORD5;
    [[vk::location(6)]] nointerpolation float2 v_fw : TEXCOORD6;
};

[shader("vertex")]
VSOutput main(uint vert_id : SV_VertexID) {
    float2 corner = k_quad[vert_id];
    float3 world_from_cam = float3(corner * pc.span, -pc.view.cam.z);
    float3 view = view_rotate(pc.view.q, world_from_cam);

    VSOutput out;
    out.sv_pos = view_ndc(view, pc.view.zoom, pc.width, pc.height, 0.0);
    out.v_c = corner;
    out.v_cell_med = pc.cell_med;
    out.v_frac = pc.level_frac;
    out.v_cam_mod = float2(pc.origin_x, pc.origin_y);
    out.v_span_steps = pc.span_steps;
    out.v_span_mod = pc.span_mod;
    out.v_fw = pixel_footprint(pc.view.q, pc.view.zoom);
    return out;
}
