#version 450
#extension GL_EXT_scalar_block_layout : require

// Flat color. The cube pass resolves multisample coverage, so the edge
// of this polygon is an average of the samples that cover it.

layout(push_constant, scalar) uniform Push {
    layout(offset = 56) float r;
    layout(offset = 60) float g;
    layout(offset = 64) float b;
    layout(offset = 68) float a;
} pc;

layout(location = 0) flat in vec3 v_emit;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(min(vec3(pc.r, pc.g, pc.b) + v_emit, vec3(1.0)), pc.a);
}
