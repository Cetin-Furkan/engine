#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Screen text. One TextQuad is 20 floats. Pixel y = 0 is the top of the
// framebuffer, matching hud.vert. NDC y = -1 is that top. Six vertices
// expand one instance in the old two-triangle order.

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Quads {
    float f[];
};

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) uint64_t quads;
    layout(offset = 8) uint64_t atlas;
    layout(offset = 16) uint fb_w;
    layout(offset = 20) uint fb_h;
    layout(offset = 24) uint dim;
    layout(offset = 28) uint spread;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_rgba;
layout(location = 2) out vec4 v_box;
layout(location = 3) out float v_mode;

void main() {
    uint base = uint(gl_InstanceIndex) * 20u;
    float x0 = Quads(pc.quads).f[base + 0u];
    float y0 = Quads(pc.quads).f[base + 1u];
    float x1 = Quads(pc.quads).f[base + 2u];
    float y1 = Quads(pc.quads).f[base + 3u];
    float u0 = Quads(pc.quads).f[base + 4u];
    float v0 = Quads(pc.quads).f[base + 5u];
    float u1 = Quads(pc.quads).f[base + 6u];
    float v1 = Quads(pc.quads).f[base + 7u];
    v_rgba = vec4(Quads(pc.quads).f[base + 8u], Quads(pc.quads).f[base + 9u],
                  Quads(pc.quads).f[base + 10u], Quads(pc.quads).f[base + 11u]);
    v_box = vec4(Quads(pc.quads).f[base + 12u], Quads(pc.quads).f[base + 13u],
                 Quads(pc.quads).f[base + 14u], Quads(pc.quads).f[base + 15u]);
    v_mode = Quads(pc.quads).f[base + 16u];

    uint vi = uint(gl_VertexIndex);
    float x = x0;
    float y = y0;
    float u = u0;
    float v = v0;
    if (vi == 1u || vi == 2u || vi == 4u) {
        x = x1;
        u = u1;
    }
    if (vi == 2u || vi == 4u || vi == 5u) {
        y = y1;
        v = v1;
    }
    float fw = max(float(pc.fb_w), 1.0);
    float fh = max(float(pc.fb_h), 1.0);
    gl_Position = vec4(x / fw * 2.0 - 1.0, y / fh * 2.0 - 1.0, 0.0, 1.0);
    v_uv = vec2(u, v);
}
