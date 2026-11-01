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

static const int2 k_corner[6] = {
    int2(0, 0), int2(1, 0), int2(1, 1),
    int2(0, 0), int2(1, 1), int2(0, 1)
};

[shader("vertex")]
float4 main(uint vert_id : SV_VulkanVertexID) : SV_Position {
    int2 t = k_corner[vert_id];
    float px = float(pc.x) + float(t.x) * float(pc.w);
    float py = float(pc.y) + float(t.y) * float(pc.h);
    float fw = max(float(pc.fb_w), 1.0);
    float fh = max(float(pc.fb_h), 1.0);
    return float4(px / fw * 2.0 - 1.0, py / fh * 2.0 - 1.0, 0.0, 1.0);
}
