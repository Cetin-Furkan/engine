struct Push {
    uint64_t quads;
    uint64_t atlas;
    uint fb_w;
    uint fb_h;
    uint dim;
    uint spread;
};
[[vk::push_constant]] Push pc;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float2 v_uv : TEXCOORD0;
    [[vk::location(1)]] float4 v_rgba : TEXCOORD1;
    [[vk::location(2)]] float4 v_box : TEXCOORD2;
    [[vk::location(3)]] float v_mode : TEXCOORD3;
};

[shader("vertex")]
VSOutput main(uint inst_id : SV_VulkanInstanceID, uint vert_id : SV_VulkanVertexID) {
    float* quads = (float*)pc.quads;
    uint base = inst_id * 20u;
    float x0 = quads[base + 0u];
    float y0 = quads[base + 1u];
    float x1 = quads[base + 2u];
    float y1 = quads[base + 3u];
    float u0 = quads[base + 4u];
    float v0 = quads[base + 5u];
    float u1 = quads[base + 6u];
    float v1 = quads[base + 7u];

    VSOutput out;
    out.v_rgba = float4(quads[base + 8u], quads[base + 9u],
                        quads[base + 10u], quads[base + 11u]);
    out.v_box = float4(quads[base + 12u], quads[base + 13u],
                       quads[base + 14u], quads[base + 15u]);
    out.v_mode = quads[base + 16u];

    float x = x0;
    float y = y0;
    float u = u0;
    float v = v0;
    if (vert_id == 1u || vert_id == 2u || vert_id == 4u) {
        x = x1;
        u = u1;
    }
    if (vert_id == 2u || vert_id == 4u || vert_id == 5u) {
        y = y1;
        v = v1;
    }
    float fw = max(float(pc.fb_w), 1.0);
    float fh = max(float(pc.fb_h), 1.0);
    out.sv_pos = float4(x / fw * 2.0 - 1.0, y / fh * 2.0 - 1.0, 0.0, 1.0);
    out.v_uv = float2(u, v);
    return out;
}
