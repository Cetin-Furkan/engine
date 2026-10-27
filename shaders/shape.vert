#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_GOOGLE_include_directive : require

#include "camera.inc"
#include "material.inc"

// One polygon lives in a device buffer. The vertex shader turns it into
// fill triangles, edge quads, and corner marks. The turn about the center
// happens here. mode 0 fills, mode 1 strokes, mode 2 marks.

layout(buffer_reference, scalar, buffer_reference_align = 8) buffer Points {
    vec2 p[];
};

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Idxs {
    uint i[];
};

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) uint64_t points;
    layout(offset = 8) uint64_t indices;
    layout(offset = 16) uint count;
    layout(offset = 20) uint mode;
    layout(offset = 24) uint closed;
    layout(offset = 28) float px;
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

const int k_quad[6] = int[](0, 1, 2, 0, 2, 3);

vec2 turned(vec2 p) {
    vec2 d = p - vec2(pc.center_x, pc.center_y);
    float c = cos(pc.angle_rad);
    float s = sin(pc.angle_rad);
    return vec2(pc.center_x + c * d.x - s * d.y, pc.center_y + s * d.x + c * d.y);
}

vec2 stroke_corner(vec2 a, vec2 b, int which) {
    vec2 d = b - a;
    float len = length(d);
    vec2 n = vec2(0.0);
    if (len > 0.001) {
        n = vec2(-d.y, d.x) / len * pc.px;
    }
    if (which == 0) return a - n;
    if (which == 1) return b - n;
    if (which == 2) return b + n;
    return a + n;
}

vec2 mark_corner(vec2 p, int which) {
    float h = pc.px;
    if (which == 0) return p + vec2(-h, -h);
    if (which == 1) return p + vec2(h, -h);
    if (which == 2) return p + vec2(h, h);
    return p + vec2(-h, h);
}

void main() {
    Points pts = Points(pc.points);
    vec2 world = vec2(0.0);
    bool drop = false;
    if (pc.mode == 0u) {
        Idxs ix = Idxs(pc.indices);
        uint id = ix.i[gl_InstanceIndex * 3u + uint(gl_VertexIndex)];
        if (id >= pc.count) {
            drop = true;
        } else {
            world = pts.p[id];
        }
    } else {
        int which = k_quad[gl_VertexIndex];
        if (pc.mode == 1u) {
            uint i0 = gl_InstanceIndex;
            uint i1 = pc.closed != 0u ? (i0 + 1u) % pc.count : i0 + 1u;
            vec2 a = pts.p[i0];
            vec2 b = pts.p[i1];
            vec2 d = b - a;
            if (pc.closed == 0u && dot(d, d) < 0.25) {
                drop = true;
            }
            world = stroke_corner(a, b, which);
        } else {
            world = mark_corner(pts.p[gl_InstanceIndex], which);
        }
    }
    world = turned(world);
    if (drop) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    } else {
        vec3 view = view_of(vec3(world, 0.0), vec4(pc.qx, pc.qy, pc.qz, pc.qw),
                            vec3(pc.cam_x, pc.cam_y, pc.cam_z));
        gl_Position = view_ndc(view, pc.zoom, pc.width, pc.height, pc.depth);
    }
    v_emit = material_emit(pc.materials, pc.mat_index);
}
