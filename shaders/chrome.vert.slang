struct Control {
    int x;
    int y;
    int w;
    int h;
    uint color;
    uint glyph;
};

struct Push {
    int rect_x;
    int rect_y;
    int rect_w;
    int rect_h;
    uint fb_w;
    uint fb_h;
    uint layer_color;
    uint ncontrols;
    uint srgb;
    uint pad;
    Control ctl[8];
};
[[vk::push_constant]] Push pc;

static const int2 k_corner[6] = {
    int2(0, 0), int2(1, 0), int2(1, 1),
    int2(0, 0), int2(1, 1), int2(0, 1)
};

[shader("vertex")]
float4 main(uint vert_id : SV_VulkanVertexID) : SV_Position {
    int2 t = k_corner[vert_id];
    float px = float(pc.rect_x) + float(t.x) * float(pc.rect_w);
    float py = float(pc.rect_y) + float(t.y) * float(pc.rect_h);
    float w = max(float(pc.fb_w), 1.0);
    float h = max(float(pc.fb_h), 1.0);
    return float4(px / w * 2.0 - 1.0, py / h * 2.0 - 1.0, 0.0, 1.0);
}
