#version 450
#extension GL_EXT_scalar_block_layout : require

// One convex quad. Corners are pixels from the window center, +Y up,
// before the in-plane turn. Framebuffer y = 0 is the top, so Y is negated,
// matching the cube. Six vertices, no vertex buffer.

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
    float w = max(float(pc.width), 1.0);
    float h = max(float(pc.height), 1.0);
    gl_Position = vec4(px / (w * 0.5), -py / (h * 0.5), pc.depth, 1.0);
}
