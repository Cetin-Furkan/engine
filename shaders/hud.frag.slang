struct Push {
    uint fb_w;
    uint fb_h;
    uint x;
    uint y;
    uint w;
    uint h;
    uint rows0;
    uint rows1;
    uint rgb;
    uint solid;
};
[[vk::push_constant]] Push pc;

[[vk::constant_id(0)]] const uint k_srgb = 0u;

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

[shader("fragment")]
float4 main(float4 sv_pos : SV_Position) : SV_Target0 {
    float3 plate = float3(channel(0x1cu), channel(0x1eu), channel(0x22u));
    if (pc.solid != 0u) {
        return float4(plate, 1.0);
    }
    int gy = int(sv_pos.y) - int(pc.y);
    if (gy < 0 || gy > 7) {
        return float4(plate, 1.0);
    }
    uint packed = gy < 4 ? pc.rows0 : pc.rows1;
    uint bits = (packed >> ((uint(gy) & 3u) * 8u)) & 0xffu;
    int gx = int(sv_pos.x) - int(pc.x);
    if (gx < 0 || gx > 7 || (bits & (1u << uint(gx))) == 0u) {
        return float4(plate, 1.0);
    }
    return float4(channel((pc.rgb >> 16u) & 0xffu),
                  channel((pc.rgb >> 8u) & 0xffu),
                  channel(pc.rgb & 0xffu),
                  1.0);
}
