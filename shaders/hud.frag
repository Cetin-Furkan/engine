#version 450
#extension GL_EXT_scalar_block_layout : require

// solid draws the plate. Otherwise rows0/rows1 are eight 8-bit glyph rows,
// top row in the low byte. Bit 0 is the leftmost pixel.

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

layout(location = 0) out vec4 out_color;

// 1 on the sRGB pipeline, 0 on the UNORM pipeline. An sRGB view encodes
// again on store, so the shader must hand it the IEC 61966-2-1 linear value.
layout(constant_id = 0) const uint k_srgb = 0u;

float channel(uint byte_value) {
    float c = float(byte_value) * (1.0 / 255.0);
    if (k_srgb == 0u) {
        return c;
    }
    if (c <= 0.04045) {
        return c * (1.0 / 12.92);
    }
    float encoded = (c + 0.055) * (1.0 / 1.055);
    return exp2(2.4 * log2(encoded));
}

void main() {
    vec3 plate = vec3(channel(0x1cu), channel(0x1eu), channel(0x22u));
    if (pc.solid != 0u) {
        out_color = vec4(plate, 1.0);
        return;
    }
    int gy = int(gl_FragCoord.y) - int(pc.y);
    if (gy < 0 || gy > 7) {
        out_color = vec4(plate, 1.0);
        return;
    }
    uint packed = gy < 4 ? pc.rows0 : pc.rows1;
    uint bits = (packed >> ((uint(gy) & 3u) * 8u)) & 0xffu;
    int gx = int(gl_FragCoord.x) - int(pc.x);
    if (gx < 0 || gx > 7 || (bits & (1u << uint(gx))) == 0u) {
        out_color = vec4(plate, 1.0);
        return;
    }
    out_color = vec4(channel((pc.rgb >> 16u) & 0xffu),
                     channel((pc.rgb >> 8u) & 0xffu),
                     channel(pc.rgb & 0xffu),
                     1.0);
}
