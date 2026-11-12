struct MinimalVertex {
    float2 pos_px;
    float2 pad;
    float4 rgba;
};

struct MinimalPush {
    uint64_t vertices_bda;
    uint     fb_w;
    uint     fb_h;
};
[[vk::push_constant]] MinimalPush pc;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    MinimalVertex v = ((MinimalVertex*)pc.vertices_bda)[vert_id];
    float half_w = max(float(pc.fb_w) * 0.5, 1.0);
    float half_h = max(float(pc.fb_h) * 0.5, 1.0);

    VSOutput out;
    out.sv_pos = float4(v.pos_px.x / half_w, -v.pos_px.y / half_h, 0.0, 1.0);
    out.color  = v.rgba;
    return out;
}
