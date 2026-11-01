#include "camera.slang"
#include "material.slang"

struct ShapePush {
    uint64_t points;
    uint64_t indices;
    uint count;
    uint mode;
    uint closed;
    float px;
    float center_x;
    float center_y;
    float angle_rad;
    float depth;
    uint width;
    uint height;
    float r;
    float g;
    float b;
    float a;
    ViewPush view;
};
[[vk::push_constant]] ShapePush pc;

static const int k_quad[6] = { 0, 1, 2, 0, 2, 3 };

float2 turned(float2 p) {
    float2 d = p - float2(pc.center_x, pc.center_y);
    float c = cos(pc.angle_rad);
    float s = sin(pc.angle_rad);
    return float2(pc.center_x + c * d.x - s * d.y, pc.center_y + s * d.x + c * d.y);
}

float screen_px() {
    return pc.px / max(pc.view.zoom, 0.0001);
}

float2 stroke_corner(float2 a, float2 b, int which) {
    float2 d = b - a;
    float len = length(d);
    float2 n = float2(0.0);
    if (len > 0.001) {
        n = float2(-d.y, d.x) / len * screen_px();
    }
    if (which == 0) return a - n;
    if (which == 1) return b - n;
    if (which == 2) return b + n;
    return a + n;
}

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] nointerpolation float3 v_emit : TEXCOORD0;
};

[shader("vertex")]
VSOutput main(uint inst_id : SV_VulkanInstanceID, uint vert_id : SV_VulkanVertexID) {
    float2* pts = (float2*)pc.points;
    float2 world = float2(0.0);
    bool drop = false;

    if (pc.mode == 0u) {
        uint* ix = (uint*)pc.indices;
        uint id = ix[inst_id * 3u + vert_id];
        if (id >= pc.count) {
            drop = true;
        } else {
            world = pts[id];
        }
    } else if (pc.mode == 1u) {
        int which = k_quad[vert_id];
        uint i0 = inst_id;
        uint i1 = pc.closed != 0u ? (i0 + 1u) % pc.count : i0 + 1u;
        float2 a = pts[i0];
        float2 b = pts[i1];
        float2 d = b - a;
        if (pc.closed == 0u && dot(d, d) < 0.25) {
            drop = true;
        }
        world = stroke_corner(a, b, which);
    } else {
        uint seg = vert_id / 3u;
        uint corner = vert_id % 3u;
        float t0 = 6.28318530718 * float(seg) / 12.0;
        float t1 = 6.28318530718 * float(seg + 1u) / 12.0;
        float rad = screen_px();
        float2 c = pts[inst_id];
        world = c;
        if (corner == 1u) {
            world = c + float2(cos(t0), sin(t0)) * rad;
        } else if (corner == 2u) {
            world = c + float2(cos(t1), sin(t1)) * rad;
        }
        if (pc.px <= 0.0) {
            drop = true;
        }
    }

    world = turned(world);
    VSOutput out;
    if (drop) {
        out.sv_pos = float4(0.0, 0.0, -2.0, 1.0);
    } else {
        float3 view = view_of(float3(world, 0.0), pc.view.q, pc.view.cam);
        out.sv_pos = view_ndc(view, pc.view.zoom, pc.width, pc.height, pc.depth);
    }
    out.v_emit = material_emit(pc.view.materials, pc.view.mat_index);
    return out;
}
