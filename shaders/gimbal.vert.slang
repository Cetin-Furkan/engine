#include "camera.slang"

struct Push {
    float qx;
    float qy;
    float qz;
    float qw;
    float fb_w;
    float fb_h;
    uint ord;
    uint pad;
};
[[vk::push_constant]] Push pc;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] nointerpolation float3 v_color : TEXCOORD0;
};

static const uint SEG = 16u;
static const uint ARC_SEG = 16u;
static const uint AXIS_VERTS = 6u + SEG * 6u + SEG * 3u + 18u;
static const uint ARC_VERTS = 3u * ARC_SEG * 6u;
static const uint HUB_VERTS = SEG * 3u;

float3 axis_dir(uint a) {
    if (a == 0u) return float3(1.0, 0.0, 0.0);
    if (a == 1u) return float3(0.0, 1.0, 0.0);
    if (a == 2u) return float3(0.0, 0.0, 1.0);
    if (a == 3u) return float3(-1.0, 0.0, 0.0);
    if (a == 4u) return float3(0.0, -1.0, 0.0);
    return float3(0.0, 0.0, -1.0);
}

float3 axis_rgb(uint a) {
    uint p = a % 3u;
    float3 c = float3(0.93, 0.29, 0.24);
    if (p == 1u) {
        c = float3(0.22, 0.78, 0.38);
    } else if (p == 2u) {
        c = float3(0.28, 0.52, 1.0);
    }
    if (a >= 3u) {
        c = lerp(c, float3(0.62, 0.68, 0.74), 0.55);
    }
    return c;
}

float2 hub_px() {
    float margin = 96.0;
    return float2(pc.fb_w - margin, 36.0 + margin);
}

float2 to_ndc(float2 px) {
    float w = max(pc.fb_w, 1.0);
    float h = max(pc.fb_h, 1.0);
    return float2(px.x / w * 2.0 - 1.0, 1.0 - px.y / h * 2.0);
}

float3 turned(float3 dir) {
    return view_rotate(float4(pc.qx, pc.qy, pc.qz, pc.qw), dir);
}

float2 project_px(float3 dir, float radius) {
    float3 t = turned(dir);
    return hub_px() + float2(t.x, -t.y) * radius;
}

float2 quad_corner(float2 a, float2 b, float half_w, uint which) {
    float2 d = b - a;
    float len = length(d);
    float2 n = float2(0.0, 1.0);
    if (len > 0.001) {
        d /= len;
        n = float2(-d.y, d.x);
    }
    float2 o = n * half_w;
    if (which == 0u) return a - o;
    if (which == 1u) return b - o;
    if (which == 2u) return b + o;
    return a + o;
}

VSOutput make_emit(float2 px, float3 color) {
    VSOutput out;
    out.sv_pos = float4(to_ndc(px), 0.0, 1.0);
    out.v_color = color;
    return out;
}

VSOutput make_drop() {
    VSOutput out;
    out.sv_pos = float4(0.0, 0.0, -2.0, 1.0);
    out.v_color = float3(0.0);
    return out;
}

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    uint vid = vert_id;
    if (vid < HUB_VERTS) {
        uint seg = vid / 3u;
        uint corner = vid % 3u;
        float t0 = 6.28318530718 * float(seg) / float(SEG);
        float t1 = 6.28318530718 * float(seg + 1u) / float(SEG);
        float2 c = hub_px();
        float2 p = c;
        if (corner == 1u) {
            p = c + float2(cos(t0), sin(t0)) * 4.0;
        } else if (corner == 2u) {
            p = c + float2(cos(t1), sin(t1)) * 4.0;
        }
        return make_emit(p, float3(0.82, 0.86, 0.90));
    }
    vid -= HUB_VERTS;

    if (vid < ARC_VERTS) {
        uint arc = vid / (ARC_SEG * 6u);
        uint rem = vid % (ARC_SEG * 6u);
        uint seg = rem / 6u;
        uint which = rem % 6u;
        uint corner = which;
        if (which >= 3u) {
            corner = which == 3u ? 0u : (which == 4u ? 2u : 3u);
        }
        float3 u = float3(1.0, 0.0, 0.0);
        float3 v = float3(0.0, 1.0, 0.0);
        float3 col = axis_rgb(2u);
        if (arc == 1u) {
            u = float3(0.0, 1.0, 0.0);
            v = float3(0.0, 0.0, 1.0);
            col = axis_rgb(0u);
        } else if (arc == 2u) {
            u = float3(0.0, 0.0, 1.0);
            v = float3(1.0, 0.0, 0.0);
            col = axis_rgb(1u);
        }
        float a0 = 1.57079632679 * float(seg) / float(ARC_SEG);
        float a1 = 1.57079632679 * float(seg + 1u) / float(ARC_SEG);
        float3 d0 = u * cos(a0) + v * sin(a0);
        float3 d1 = u * cos(a1) + v * sin(a1);
        if (turned(d0).z < -0.05 && turned(d1).z < -0.05) {
            return make_drop();
        }
        float2 p0 = project_px(d0, 36.0);
        float2 p1 = project_px(d1, 36.0);
        return make_emit(quad_corner(p0, p1, 1.35, corner), col);
    }
    vid -= ARC_VERTS;

    uint slot = vid / AXIS_VERTS;
    uint local = vid % AXIS_VERTS;
    if (slot >= 6u) {
        return make_drop();
    }
    uint a = (pc.ord >> (slot * 3u)) & 7u;
    bool positive = a < 3u;
    float reach = positive ? 54.0 : 30.0;
    float disc = positive ? 11.0 : 7.0;
    float3 dir = axis_dir(a);
    float3 col = axis_rgb(a);
    float2 end = project_px(dir, reach);
    float2 center = hub_px();
    float stem = length(end - center);
    bool facing = stem < 8.0;

    if (local < 6u) {
        if (facing) {
            return make_drop();
        }
        uint which = local;
        uint corner = which;
        if (which >= 3u) {
            corner = which == 3u ? 0u : (which == 4u ? 2u : 3u);
        }
        return make_emit(quad_corner(center, end, 1.35, corner), col);
    }
    local -= 6u;

    float2 at = facing ? center : end;
    if (local < SEG * 6u) {
        uint seg = local / 6u;
        uint which = local % 6u;
        uint corner = which;
        if (which >= 3u) {
            corner = which == 3u ? 0u : (which == 4u ? 2u : 3u);
        }
        float t0 = 6.28318530718 * float(seg) / float(SEG);
        float t1 = 6.28318530718 * float(seg + 1u) / float(SEG);
        float inner = positive ? 0.0 : disc - 1.8;
        float outer = disc;
        float2 i0 = at + float2(cos(t0), sin(t0)) * inner;
        float2 o0 = at + float2(cos(t0), sin(t0)) * outer;
        float2 i1 = at + float2(cos(t1), sin(t1)) * inner;
        float2 o1 = at + float2(cos(t1), sin(t1)) * outer;
        float2 p = i0;
        if (corner == 1u) p = o0;
        else if (corner == 2u) p = o1;
        else if (corner == 3u) p = i1;
        return make_emit(p, col);
    }
    local -= SEG * 6u;

    if (local < SEG * 3u) {
        if (!positive) {
            return make_drop();
        }
        uint seg = local / 3u;
        uint corner = local % 3u;
        float t0 = 6.28318530718 * float(seg) / float(SEG);
        float t1 = 6.28318530718 * float(seg + 1u) / float(SEG);
        float2 p = at;
        if (corner == 1u) {
            p = at + float2(cos(t0), sin(t0)) * (disc - 1.6);
        } else if (corner == 2u) {
            p = at + float2(cos(t1), sin(t1)) * (disc - 1.6);
        }
        return make_emit(p, col);
    }
    local -= SEG * 3u;

    if (!positive) {
        return make_drop();
    }
    uint quad = local / 6u;
    uint which = local % 6u;
    uint corner = which;
    if (which >= 3u) {
        corner = which == 3u ? 0u : (which == 4u ? 2u : 3u);
    }

    float2 s0 = at;
    float2 s1 = at;
    bool live = false;
    float hw = 1.15;
    if (a == 0u) {
        if (quad == 0u) {
            s0 = at + float2(-4.2, 4.2);
            s1 = at + float2(4.2, -4.2);
            live = true;
        } else if (quad == 1u) {
            s0 = at + float2(-4.2, -4.2);
            s1 = at + float2(4.2, 4.2);
            live = true;
        }
    } else if (a == 1u) {
        if (quad == 0u) {
            s0 = at + float2(-4.4, -4.6);
            s1 = at + float2(0.0, -0.6);
            live = true;
        } else if (quad == 1u) {
            s0 = at + float2(4.4, -4.6);
            s1 = at + float2(0.0, -0.6);
            live = true;
        } else if (quad == 2u) {
            s0 = at + float2(0.0, -0.6);
            s1 = at + float2(0.0, 4.4);
            live = true;
        }
    } else if (quad == 0u) {
        s0 = at + float2(-4.2, -3.8);
        s1 = at + float2(4.2, -3.8);
        live = true;
    } else if (quad == 1u) {
        s0 = at + float2(4.2, -3.8);
        s1 = at + float2(-4.2, 3.8);
        live = true;
    } else if (quad == 2u) {
        s0 = at + float2(-4.2, 3.8);
        s1 = at + float2(4.2, 3.8);
        live = true;
    }
    if (!live) {
        return make_drop();
    }
    return make_emit(quad_corner(s0, s1, hw, corner), float3(0.97, 0.98, 0.99));
}
