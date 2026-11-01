struct Push {
    uint64_t quads;
    uint64_t atlas;
    uint fb_w;
    uint fb_h;
    uint dim;
    uint spread;
};
[[vk::push_constant]] Push pc;

[[vk::constant_id(0)]] const uint k_srgb = 0u;

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
    uint* atlas = (uint*)pc.atlas;
    uint word = atlas[linear >> 2u];
    uint shift = (linear & 3u) * 8u;
    return float((word >> shift) & 255u);
}

float sample_sdf(float2 uv) {
    float2 p = uv - 0.5;
    int2 i = int2(floor(p));
    float2 f = frac(p);
    float a = sdf_at(i.x, i.y);
    float b = sdf_at(i.x + 1, i.y);
    float c = sdf_at(i.x, i.y + 1);
    float d = sdf_at(i.x + 1, i.y + 1);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

[shader("fragment")]
float4 main(float4 sv_pos : SV_Position,
            [[vk::location(0)]] float2 v_uv : TEXCOORD0,
            [[vk::location(1)]] float4 v_rgba : TEXCOORD1,
            [[vk::location(2)]] float4 v_box : TEXCOORD2,
            [[vk::location(3)]] float v_mode : TEXCOORD3) : SV_Target0 {
    if (sv_pos.x < v_box.x || sv_pos.y < v_box.y ||
        sv_pos.x >= v_box.z || sv_pos.y >= v_box.w) {
        discard;
    }
    float3 rgb = float3(channel(v_rgba.r), channel(v_rgba.g), channel(v_rgba.b));
    if (v_mode < 0.5) {
        return float4(rgb, v_rgba.a);
    }
    float byte_val = sample_sdf(v_uv);
    float dist = (byte_val - 128.0) * (float(pc.spread) / 128.0);
    float edge = max(fwidth(dist), 0.5);
    float alpha = smoothstep(-edge, edge, dist) * v_rgba.a;
    return float4(rgb, alpha);
}
