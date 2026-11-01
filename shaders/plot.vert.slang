#include "camera.slang"

struct PlotPoint {
    float x;
    float y;
    float z;
    float brk;
};

struct Push {
    uint64_t addr;
    uint width;
    uint height;
    uint base;
    uint count;
    uint kind;
    float r;
    float g;
    float b;
    float qx;
    float qy;
    float qz;
    float qw;
    float cam_x;
    float cam_y;
    float cam_z;
    float zoom;

    property float4 q { get { return float4(qx, qy, qz, qw); } }
    property float3 cam { get { return float3(cam_x, cam_y, cam_z); } }
};
[[vk::push_constant]] Push pc;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] nointerpolation float3 v_color : TEXCOORD0;
};

static const int k_corner[6] = { 0, 1, 2, 0, 2, 3 };

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    VSOutput out;
    out.v_color = float3(pc.r, pc.g, pc.b);
    uint slot = vert_id / 6u;
    int corner = k_corner[vert_id % 6];
    if (pc.addr == 0ul || slot >= pc.count) {
        out.sv_pos = float4(2.0, 2.0, 0.0, 1.0);
        return out;
    }
    PlotPoint* buf = (PlotPoint*)pc.addr;
    float zoom = max(pc.zoom, 1.0e-4);
    PlotPoint a = buf[pc.base + slot];
    float3 view_a = view_of(float3(a.x, a.y, a.z), pc.q, pc.cam);
    float3 view_b = view_a;
    float half_px = 2.0;
    bool drop = false;

    if (pc.kind == 0u) {
        if (slot + 1u >= pc.count) {
            drop = true;
        } else {
            PlotPoint b = buf[pc.base + slot + 1u];
            if (b.brk > 0.5) {
                drop = true;
            } else {
                view_b = view_of(float3(b.x, b.y, b.z), pc.q, pc.cam);
            }
        }
        half_px = 1.0;
    }
    if (drop) {
        out.sv_pos = float4(2.0, 2.0, 0.0, 1.0);
        return out;
    }

    float hw = half_px / zoom;
    float2 pa = view_a.xy;
    float2 pb = view_b.xy;
    float2 dir = pb - pa;
    float len = length(dir);
    float2 n = float2(0.0, 1.0);
    if (len > 1.0e-5) {
        n = float2(-dir.y, dir.x) / len;
    }

    float2 quad[4];
    if (pc.kind == 1u || len <= 1.0e-5) {
        quad[0] = pa + float2(-hw, -hw);
        quad[1] = pa + float2(hw, -hw);
        quad[2] = pa + float2(hw, hw);
        quad[3] = pa + float2(-hw, hw);
    } else {
        quad[0] = pa - n * hw;
        quad[1] = pb - n * hw;
        quad[2] = pb + n * hw;
        quad[3] = pa + n * hw;
    }

    float pz = (corner == 0 || corner == 3) ? view_a.z : view_b.z;
    out.sv_pos = view_ndc(float3(quad[corner], pz), zoom, pc.width, pc.height, 0.0);
    return out;
}
