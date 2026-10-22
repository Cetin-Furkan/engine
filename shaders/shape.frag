#version 450
#extension GL_EXT_scalar_block_layout : require

// Flat color. The cube pass resolves multisample coverage, so the edge
// of this quad is an average of the samples that cover it.

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
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(pc.r, pc.g, pc.b, pc.a);
}
