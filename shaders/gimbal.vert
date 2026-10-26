#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

#include "camera.inc"

// Three axes in the corner. The quaternion is the camera, so the widget
// turns with the view and stays in one screen place. Depth is not written.
// Identity looks down -Z: X right, Y up, Z toward the camera as a dot.

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) float qx;
    layout(offset = 4) float qy;
    layout(offset = 8) float qz;
    layout(offset = 12) float qw;
} pc;

layout(location = 0) flat out vec3 v_color;

const int k_corner[6] = int[](0, 1, 2, 0, 2, 3);

void main() {
    uint axis = uint(gl_VertexIndex) / 6u;
    int corner = k_corner[gl_VertexIndex % 6];
    vec3 dir = vec3(1.0, 0.0, 0.0);
    v_color = vec3(0.93, 0.28, 0.24);
    if (axis == 1u) {
        dir = vec3(0.0, 1.0, 0.0);
        v_color = vec3(0.28, 0.82, 0.36);
    } else if (axis == 2u) {
        dir = vec3(0.0, 0.0, 1.0);
        v_color = vec3(0.30, 0.55, 1.0);
    }
    vec3 turned = view_rotate(vec4(pc.qx, pc.qy, pc.qz, pc.qw), dir);
    vec2 d = vec2(turned.x, -turned.y);
    float len = length(d);
    vec2 n = vec2(0.0, 1.0);
    if (len > 1.0e-4) {
        d /= len;
        n = vec2(-d.y, d.x);
    }
    vec2 origin = vec2(0.78, 0.68);
    float reach = 0.075;
    float hw = 0.0045;
    vec2 a = origin;
    vec2 b = origin + d * reach;
    vec2 quad[4];
    if (len <= 1.0e-4) {
        hw = 0.008;
        quad[0] = a + vec2(-hw, -hw);
        quad[1] = a + vec2(hw, -hw);
        quad[2] = a + vec2(hw, hw);
        quad[3] = a + vec2(-hw, hw);
    } else {
        quad[0] = a - n * hw;
        quad[1] = b - n * hw;
        quad[2] = b + n * hw;
        quad[3] = a + n * hw;
    }
    gl_Position = vec4(quad[corner], 0.0, 1.0);
}
