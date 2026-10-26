#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

#include "camera.inc"
#include "material.inc"

// Four corners. Repeating corner 2 in corner 3 draws one triangle.
// Corners are world pixels on the plane, +Y up, before the in-plane turn.
// Framebuffer y = 0 is the top, so Y is negated, matching the cube.
// Six vertices, no vertex buffer.

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) float x0;
    layout(offset = 4) float y0;
    layout(offset = 8) float x1;
    layout(offset = 12) float y1;
    layout(offset = 16) float x2;
    layout(offset = 20) float y2;
    layout(offset = 24) float x3;
    layout(offset = 28) float y3;
    layout(offset = 32) float center_x;
    layout(offset = 36) float center_y;
    layout(offset = 40) float angle_rad;
    layout(offset = 44) float depth;
    layout(offset = 48) uint width;
    layout(offset = 52) uint height;
    layout(offset = 56) float r;
    layout(offset = 60) float g;
    layout(offset = 64) float b;
    layout(offset = 68) float a;
    layout(offset = 72) uint64_t materials;
    layout(offset = 80) uint mat_index;
    layout(offset = 84) uint mat_pad;
    layout(offset = 88) float qx;
    layout(offset = 92) float qy;
    layout(offset = 96) float qz;
    layout(offset = 100) float qw;
    layout(offset = 104) float cam_x;
    layout(offset = 108) float cam_y;
    layout(offset = 112) float cam_z;
    layout(offset = 116) float zoom;
} pc;

layout(location = 0) flat out vec3 v_emit;

const int k_id[6] = int[](0, 1, 2, 0, 2, 3);

void main() {
    float xs[4] = float[4](pc.x0, pc.x1, pc.x2, pc.x3);
    float ys[4] = float[4](pc.y0, pc.y1, pc.y2, pc.y3);
    int i = k_id[gl_VertexIndex];
    float dx = xs[i] - pc.center_x;
    float dy = ys[i] - pc.center_y;
    float c = cos(pc.angle_rad);
    float s = sin(pc.angle_rad);
    float px = pc.center_x + c * dx - s * dy;
    float py = pc.center_y + s * dx + c * dy;
    vec3 view = view_of(vec3(px, py, 0.0), vec4(pc.qx, pc.qy, pc.qz, pc.qw),
                        vec3(pc.cam_x, pc.cam_y, pc.cam_z));
    gl_Position = view_ndc(view, pc.zoom, pc.width, pc.height, pc.depth);
    v_emit = material_emit(pc.materials, pc.mat_index);
}
