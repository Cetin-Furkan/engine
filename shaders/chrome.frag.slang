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

float channel(uint byte_value) {
    float c = float(byte_value) * (1.0 / 255.0);
    if (pc.srgb == 0u) {
        return c;
    }
    if (c <= 0.04045) {
        return c * (1.0 / 12.92);
    }
    float encoded = (c + 0.055) * (1.0 / 1.055);
    return exp2(2.4 * log2(encoded));
}

float3 rgb_of(uint packed) {
    return float3(channel(packed & 0xffu),
                  channel((packed >> 8) & 0xffu),
                  channel((packed >> 16) & 0xffu));
}

bool inside(int px, int py, int x, int y, int w, int h) {
    return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
}

bool on_x(int px, int py, int x0, int y0, int box) {
    if (box < 3) {
        return false;
    }
    int inset = box / 4;
    int stroke = box / 8;
    if (stroke < 1) {
        stroke = 1;
    }
    int x = px - x0;
    int y = py - y0;
    if (x < inset || y < inset || x + inset >= box || y + inset >= box) {
        return false;
    }
    int d1 = x - y;
    int d2 = x + y - (box - 1);
    if (d1 < 0) {
        d1 = -d1;
    }
    if (d2 < 0) {
        d2 = -d2;
    }
    return d1 <= stroke || d2 <= stroke;
}

[shader("fragment")]
float4 main(float4 sv_pos : SV_Position) : SV_Target0 {
    int2 p = int2(sv_pos.xy);
    float3 color = rgb_of(pc.layer_color);
    uint n = min(pc.ncontrols, 8u);
    for (uint i = 0u; i < n; ++i) {
        Control c = pc.ctl[i];
        if (!inside(p.x, p.y, c.x, c.y, c.w, c.h)) {
            continue;
        }
        color = rgb_of(c.color);
        if ((c.color >> 24) != 0u) {
            int box = c.w < c.h ? c.w : c.h;
            int gx = c.x + (c.w - box) / 2;
            int gy = c.y + (c.h - box) / 2;
            if (on_x(p.x, p.y, gx, gy, box)) {
                color = rgb_of(c.glyph);
            }
        }
    }
    return float4(color, 1.0);
}
