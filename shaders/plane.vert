#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

// One clipmap level. 32 by 32 cells, six vertices each, no vertex buffer.
// The snap origin is the camera, rounded to this level's cell. A quad
// that sits inside the finer level is degenerate, so the coarser level
// is only the ring. Far cells are the ones this index no longer names.

#include "camera.inc"

layout(push_constant, scalar) uniform Screen {
    layout(offset = 0) uint width;
    layout(offset = 4) uint height;
    layout(offset = 8) float cell;
    layout(offset = 12) float z;
    layout(offset = 16) float origin_x;
    layout(offset = 20) float origin_y;
    layout(offset = 24) float hole_min_x;
    layout(offset = 28) float hole_min_y;
    layout(offset = 32) float hole_max_x;
    layout(offset = 36) float hole_max_y;
    layout(offset = 40) uint hole;
    layout(offset = 44) uint cells;
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
layout(location = 1) flat out float v_cell;

void main() {
    uint cells = max(pc.cells, 1u);
    uint quad = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    uint qx = quad % cells;
    uint qy = quad / cells;
    uint cx = (corner == 1u || corner == 2u || corner == 4u) ? 1u : 0u;
    uint cy = (corner == 2u || corner == 4u || corner == 5u) ? 1u : 0u;
    float span = float(cells) * 0.5;
    float x0 = pc.origin_x + (float(qx) - span) * pc.cell;
    float y0 = pc.origin_y + (float(qy) - span) * pc.cell;
    float x1 = x0 + pc.cell;
    float y1 = y0 + pc.cell;
    bool interior = pc.hole != 0u &&
                    x0 > pc.hole_min_x && x1 < pc.hole_max_x &&
                    y0 > pc.hole_min_y && y1 < pc.hole_max_y;
    float x = x0 + float(cx) * pc.cell;
    float y = y0 + float(cy) * pc.cell;
    v_cell = pc.cell;
    v_world = vec2(x, y);
    if (interior) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 view = view_of(vec3(x, y, pc.z), vec4(pc.qx, pc.qy, pc.qz, pc.qw),
                        vec3(pc.cam_x, pc.cam_y, pc.cam_z));
    gl_Position = view_ndc(view, pc.zoom, pc.width, pc.height, 0.0);
}
