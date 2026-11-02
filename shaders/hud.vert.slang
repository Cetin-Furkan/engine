struct HudInstance {
    uint x, y, w, h;
    uint rows0, rows1, rgb, solid;
};

struct Push {
    uint64_t instances;
    uint fb_w;
    uint fb_h;
};
[[vk::push_constant]] Push pc;

static const int2 k_corner[6] = {
    int2(0, 0), int2(1, 0), int2(1, 1),
    int2(0, 0), int2(1, 1), int2(0, 1)
};

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] nointerpolation uint inst_id : TEXCOORD0;
};

[shader("vertex")]
VSOutput main(uint inst_id : SV_VulkanInstanceID, uint vert_id : SV_VulkanVertexID) {
    HudInstance inst = ((HudInstance*)pc.instances)[inst_id];
    int2 t = k_corner[vert_id];
    float px = float(inst.x) + float(t.x) * float(inst.w);
    float py = float(inst.y) + float(t.y) * float(inst.h);
    float fw = max(float(pc.fb_w), 1.0);
    float fh = max(float(pc.fb_h), 1.0);
    VSOutput out;
    out.sv_pos = float4(px / fw * 2.0 - 1.0, py / fh * 2.0 - 1.0, 0.0, 1.0);
    out.inst_id = inst_id;
    return out;
}
