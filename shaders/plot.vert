#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

#include "camera.inc"

// A ribbon between two samples, or a camera-facing dot. Points live in a
// device-address buffer. A break flag on the second sample drops the join
// so a new polyline does not draw a line back to the previous one.

struct PlotPoint {
    float x;
    float y;
    float z;
    float brk;
};

layout(buffer_reference, scalar, buffer_reference_align = 16) buffer PlotBuf {
    PlotPoint p[];
};

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) uint64_t addr;
    layout(offset = 8) uint width;
    layout(offset = 12) uint height;
    layout(offset = 16) uint base;
    layout(offset = 20) uint count;
    layout(offset = 24) uint kind;
    layout(offset = 28) float r;
    layout(offset = 32) float g;
    layout(offset = 36) float b;
    layout(offset = 40) float qx;
    layout(offset = 44) float qy;
    layout(offset = 48) float qz;
    layout(offset = 52) float qw;
    layout(offset = 56) float cam_x;
    layout(offset = 60) float cam_y;
    layout(offset = 64) float cam_z;
    layout(offset = 68) float zoom;
} pc;

layout(location = 0) flat out vec3 v_color;

const int k_corner[6] = int[](0, 1, 2, 0, 2, 3);

void main() {
    v_color = vec3(pc.r, pc.g, pc.b);
    uint slot = uint(gl_VertexIndex) / 6u;
    int corner = k_corner[gl_VertexIndex % 6];
    if (pc.addr == 0ul || slot >= pc.count) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    PlotBuf buf = PlotBuf(pc.addr);
    vec4 q = vec4(pc.qx, pc.qy, pc.qz, pc.qw);
    vec3 cam = vec3(pc.cam_x, pc.cam_y, pc.cam_z);
    // Same floor as k_camera_zoom_min. Below it the ribbon stops tracking
    // the screen thickness and a zoomed-out plot goes hairline.
    float zoom = max(pc.zoom, 1.0e-4);
    PlotPoint a = buf.p[pc.base + slot];
    vec3 view_a = view_of(vec3(a.x, a.y, a.z), q, cam);
    vec3 view_b = view_a;
    float half_px = 2.0;
    bool drop = false;
    if (pc.kind == 0u) {
        if (slot + 1u >= pc.count) {
            drop = true;
        } else {
            PlotPoint b = buf.p[pc.base + slot + 1u];
            if (b.brk > 0.5) {
                drop = true;
            } else {
                view_b = view_of(vec3(b.x, b.y, b.z), q, cam);
            }
        }
        half_px = 1.0;
    }
    if (drop) {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
        return;
    }
    float hw = half_px / zoom;
    vec2 pa = view_a.xy;
    vec2 pb = view_b.xy;
    vec2 dir = pb - pa;
    float len = length(dir);
    vec2 n = vec2(0.0, 1.0);
    if (len > 1.0e-5) {
        n = vec2(-dir.y, dir.x) / len;
    }
    vec2 quad[4];
    if (pc.kind == 1u || len <= 1.0e-5) {
        quad[0] = pa + vec2(-hw, -hw);
        quad[1] = pa + vec2(hw, -hw);
        quad[2] = pa + vec2(hw, hw);
        quad[3] = pa + vec2(-hw, hw);
    } else {
        quad[0] = pa - n * hw;
        quad[1] = pb - n * hw;
        quad[2] = pb + n * hw;
        quad[3] = pa + n * hw;
    }
    float pz = (corner == 0 || corner == 3) ? view_a.z : view_b.z;
    gl_Position = view_ndc(vec3(quad[corner], pz), zoom, pc.width, pc.height, 0.0);
}
