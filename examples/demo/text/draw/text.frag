#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// The atlas is R8 stored as little-endian uint32 words. FreeType's SDF
// puts the outline at 128 and the inside above it:
// distance = (byte - 128) / 128 * spread, in texels. Mode 0 is a solid
// plate, border, or caret and does not sample the atlas.

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Atlas {
    uint w[];
};

layout(push_constant, scalar) uniform Push {
    layout(offset = 0) uint64_t quads;
    layout(offset = 8) uint64_t atlas;
    layout(offset = 16) uint fb_w;
    layout(offset = 20) uint fb_h;
    layout(offset = 24) uint dim;
    layout(offset = 28) uint spread;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_rgba;
layout(location = 2) in vec4 v_box;
layout(location = 3) in float v_mode;

layout(location = 0) out vec4 out_color;

layout(constant_id = 0) const uint k_srgb = 0u;

float channel(float c) {
    if (k_srgb == 0u) {
        return c;
    }
    if (c <= 0.04045) {
        return c * (1.0 / 12.92);
    }
    float encoded = (c + 0.055) * (1.0 / 1.055);
    return exp2(2.4 * log2(encoded));
}

float sdf_at(int x, int y) {
    if (x < 0 || y < 0) {
        return 0.0;
    }
    uint ux = uint(x);
    uint uy = uint(y);
    if (ux >= pc.dim || uy >= pc.dim) {
        return 0.0;
    }
    uint linear = uy * pc.dim + ux;
    uint word = Atlas(pc.atlas).w[linear >> 2u];
    uint shift = (linear & 3u) * 8u;
    return float((word >> shift) & 255u);
}

float sample_sdf(vec2 uv) {
    vec2 p = uv - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = fract(p);
    float a = sdf_at(i.x, i.y);
    float b = sdf_at(i.x + 1, i.y);
    float c = sdf_at(i.x, i.y + 1);
    float d = sdf_at(i.x + 1, i.y + 1);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    if (gl_FragCoord.x < v_box.x || gl_FragCoord.y < v_box.y ||
        gl_FragCoord.x >= v_box.z || gl_FragCoord.y >= v_box.w) {
        discard;
    }
    vec3 rgb = vec3(channel(v_rgba.r), channel(v_rgba.g), channel(v_rgba.b));
    if (v_mode < 0.5) {
        out_color = vec4(rgb, v_rgba.a);
        return;
    }
    float byte = sample_sdf(v_uv);
    float dist = (byte - 128.0) * (float(pc.spread) / 128.0);
    float edge = max(fwidth(dist), 0.5);
    float alpha = smoothstep(-edge, edge, dist) * v_rgba.a;
    out_color = vec4(rgb, alpha);
}
