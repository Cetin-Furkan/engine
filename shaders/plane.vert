#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

// Universal 6-vertex infinite ground plane on z = 0. No vertex buffer.
// The span is computed on the CPU from the viewport diagonal and zoom,
// so the single quad covers the visible region of the plane at any
// yaw, pitch, pan, or zoom. Grid lines are analytic in the fragment
// shader; this stage only places the quad corners on z = 0.

#include "camera.inc"

layout(push_constant, scalar) uniform Screen {
    layout(offset = 0)  uint width;
    layout(offset = 4)  uint height;
    layout(offset = 8)  float cell_med;
    layout(offset = 12) float level_frac;
    layout(offset = 16) float origin_x;
    layout(offset = 20) float origin_y;
    layout(offset = 24) float span;
    layout(offset = 28) float pad0;
    layout(offset = 32) float pad1;
    layout(offset = 36) float pad2;
    layout(offset = 40) uint  pad3;
    layout(offset = 44) uint  pad4;
    layout(offset = 48) uint64_t materials;
    layout(offset = 56) uint mat_index;
    layout(offset = 60) uint mat_pad;
    layout(offset = 64) float qx;
    layout(offset = 68) float qy;
    layout(offset = 72) float qz;
    layout(offset = 76) float qw;
    layout(offset = 80) float cam_x;
    layout(offset = 84) float cam_y;
    layout(offset = 88) float cam_z;
    layout(offset = 92) float zoom;
} pc;

layout(location = 0) out vec2 v_world;
layout(location = 1) flat out float v_cell_med;
layout(location = 2) flat out float v_frac;

const vec2 k_corner[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

void main() {
    vec2 c = k_corner[gl_VertexIndex];
    vec2 world_xy = vec2(pc.origin_x, pc.origin_y) + c * pc.span;

    v_world    = world_xy;
    v_cell_med = pc.cell_med;
    v_frac     = pc.level_frac;

    vec3 view = view_of(vec3(world_xy, 0.0),
                        vec4(pc.qx, pc.qy, pc.qz, pc.qw),
                        vec3(pc.cam_x, pc.cam_y, pc.cam_z));
    vec4 ndc = view_ndc(view, pc.zoom, pc.width, pc.height, 0.0);
    ndc.z = clamp(ndc.z, 0.0001, 0.9999);
    gl_Position = ndc;
}
