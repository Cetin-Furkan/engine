#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Screen text. Pixel y = 0 is the top of the framebuffer, matching hud.vert.
// NDC y = -1 is that top. The box is a pixel clip the fragment applies.

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) uint64_t atlas;
    layout(offset = 8) uint fb_w;
    layout(offset = 12) uint fb_h;
    layout(offset = 16) uint dim;
    layout(offset = 20) uint spread;
    layout(offset = 24) uint pad0;
    layout(offset = 28) uint pad1;
} pc;

layout(location = 0) in vec2 in_px;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_rgba;
layout(location = 3) in vec4 in_box;
layout(location = 4) in float in_mode;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_rgba;
layout(location = 2) out vec4 v_box;
layout(location = 3) out float v_mode;

void main() {
    float fw = max(float(pc.fb_w), 1.0);
    float fh = max(float(pc.fb_h), 1.0);
    gl_Position = vec4(in_px.x / fw * 2.0 - 1.0, in_px.y / fh * 2.0 - 1.0, 0.0, 1.0);
    v_uv = in_uv;
    v_rgba = in_rgba;
    v_box = in_box;
    v_mode = in_mode;
}
