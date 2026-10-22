#version 450
#extension GL_EXT_scalar_block_layout : require

// One axis-aligned rectangle. Framebuffer y = 0 is the top, matching chrome.

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) uint fb_w;
    layout(offset = 4) uint fb_h;
    layout(offset = 8) uint x;
    layout(offset = 12) uint y;
    layout(offset = 16) uint w;
    layout(offset = 20) uint h;
    layout(offset = 24) uint rows0;
    layout(offset = 28) uint rows1;
    layout(offset = 32) uint rgb;
    layout(offset = 36) uint solid;
} pc;

const ivec2 k_corner[6] = ivec2[](
    ivec2(0, 0), ivec2(1, 0), ivec2(1, 1),
    ivec2(0, 0), ivec2(1, 1), ivec2(0, 1)
);

void main() {
    ivec2 t = k_corner[gl_VertexIndex];
    float px = float(pc.x) + float(t.x) * float(pc.w);
    float py = float(pc.y) + float(t.y) * float(pc.h);
    float fw = max(float(pc.fb_w), 1.0);
    float fh = max(float(pc.fb_h), 1.0);
    gl_Position = vec4(px / fw * 2.0 - 1.0, py / fh * 2.0 - 1.0, 0.0, 1.0);
}
