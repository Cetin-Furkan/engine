#version 450
#extension GL_EXT_scalar_block_layout : require

// One quad covers a chrome layer. The fragment shader decides the pixels.
// Framebuffer y = 0 is the top of the window, matching the cube's negated Y.

struct Control {
    int x;
    int y;
    int w;
    int h;
    uint color;
    uint glyph;
};

layout(push_constant, scalar) uniform Push {
    int rect_x;
    int rect_y;
    int rect_w;
    int rect_h;
    uint fb_w;
    uint fb_h;
    uint layer_color;
    uint ncontrols;
    uint srgb;
    uint pad;
    Control ctl[8];
} pc;

const ivec2 k_corner[6] = ivec2[](
    ivec2(0, 0), ivec2(1, 0), ivec2(1, 1),
    ivec2(0, 0), ivec2(1, 1), ivec2(0, 1)
);

void main() {
    ivec2 t = k_corner[gl_VertexIndex];
    float px = float(pc.rect_x) + float(t.x) * float(pc.rect_w);
    float py = float(pc.rect_y) + float(t.y) * float(pc.rect_h);
    float w = max(float(pc.fb_w), 1.0);
    float h = max(float(pc.fb_h), 1.0);
    gl_Position = vec4(px / w * 2.0 - 1.0, py / h * 2.0 - 1.0, 0.0, 1.0);
}
