#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

// Universal 6-vertex infinite ground plane on z = 0. No vertex buffer.
// The span is computed on the CPU from the viewport diagonal and zoom,
// so the single quad covers the visible region of the plane at any
// yaw, pitch, pan, or zoom. Grid lines are analytic in the fragment
// shader.
//
// The view is camera-relative: world_xy - cam_xy is c * span, so the
// vertex never adds the pan to a corner. origin_x/y are that pan
// modulo the coarse decade. The fragment rebuilds the grid phase from
// the corner parameter, which stays in [-1, 1], instead of interpolating
// a world position of several thousand units.

#include "camera.inc"

layout(push_constant, scalar) uniform Screen {
    layout(offset = 0)  uint width;
    layout(offset = 4)  uint height;
    layout(offset = 8)  float cell_med;
    layout(offset = 12) float level_frac;
    layout(offset = 16) float origin_x;
    layout(offset = 20) float origin_y;
    layout(offset = 24) float span;
    layout(offset = 28) float span_mod;
    layout(offset = 32) float span_steps;
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

layout(location = 0) noperspective out vec2 v_c;
layout(location = 1) flat out float v_cell_med;
layout(location = 2) flat out float v_frac;
layout(location = 3) flat out vec2 v_cam_mod;
layout(location = 4) flat out float v_span_steps;
layout(location = 5) flat out float v_span_mod;
layout(location = 6) flat out vec2 v_fw;

// World units one screen pixel covers, in x and in y. Same sum of
// absolute partials that fwidth produced for an orthographic view,
// taken from the quaternion so it does not depend on a varying that
// has already lost its low bits.
vec2 pixel_footprint(vec4 q, float zoom) {
    vec3 rx = view_rotate(q, vec3(1.0, 0.0, 0.0));
    vec3 ry = view_rotate(q, vec3(0.0, 1.0, 0.0));
    vec2 dp_dwx = vec2(rx.x, -rx.y) * zoom;
    vec2 dp_dwy = vec2(ry.x, -ry.y) * zoom;
    float det = dp_dwx.x * dp_dwy.y - dp_dwy.x * dp_dwx.y;
    float sdet = det >= 0.0 ? 1.0 : -1.0;
    float inv_det = sdet / max(abs(det), 1.0e-8);
    float dwx_dpx = dp_dwy.y * inv_det;
    float dwx_dpy = -dp_dwy.x * inv_det;
    float dwy_dpx = -dp_dwx.y * inv_det;
    float dwy_dpy = dp_dwx.x * inv_det;
    return vec2(abs(dwx_dpx) + abs(dwx_dpy), abs(dwy_dpx) + abs(dwy_dpy));
}

const vec2 k_corner[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

void main() {
    vec2 c = k_corner[gl_VertexIndex];
    vec4 q = vec4(pc.qx, pc.qy, pc.qz, pc.qw);

    v_c          = c;
    v_cell_med   = pc.cell_med;
    v_frac       = pc.level_frac;
    v_cam_mod    = vec2(pc.origin_x, pc.origin_y);
    v_span_steps = pc.span_steps;
    v_span_mod   = pc.span_mod;
    v_fw         = pixel_footprint(q, pc.zoom);

    vec3 view = view_rotate(q, vec3(c * pc.span, -pc.cam_z));
    vec4 ndc = view_ndc(view, pc.zoom, pc.width, pc.height, 0.0);
    ndc.z = clamp(ndc.z, 0.0001, 0.9999);
    gl_Position = ndc;
}
