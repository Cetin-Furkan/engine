#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

#include "camera.inc"

// View ball under the bar, in the top-right corner. Pixel size stays put
// when the window changes. Positive axes end in a filled circle and a
// letter. Negative axes are smaller rings. The arc in an axis color is
// the quarter turn around that axis. ord packs six axis ids, far to near,
// three bits each. Vertex counts match gimbal.c.

layout(push_constant, scalar) uniform Push {
    float qx;
    float qy;
    float qz;
    float qw;
    float fb_w;
    float fb_h;
    uint ord;
    uint pad;
} pc;

layout(location = 0) flat out vec3 v_color;

const uint SEG = 16u;
const uint ARC_SEG = 16u;
const uint AXIS_VERTS = 6u + SEG * 6u + SEG * 3u + 18u;
const uint ARC_VERTS = 3u * ARC_SEG * 6u;
const uint HUB_VERTS = SEG * 3u;

vec3 axis_dir(uint a) {
    if (a == 0u) return vec3(1.0, 0.0, 0.0);
    if (a == 1u) return vec3(0.0, 1.0, 0.0);
    if (a == 2u) return vec3(0.0, 0.0, 1.0);
    if (a == 3u) return vec3(-1.0, 0.0, 0.0);
    if (a == 4u) return vec3(0.0, -1.0, 0.0);
    return vec3(0.0, 0.0, -1.0);
}

vec3 axis_rgb(uint a) {
    uint p = a % 3u;
    vec3 c = vec3(0.93, 0.29, 0.24);
    if (p == 1u) {
        c = vec3(0.22, 0.78, 0.38);
    } else if (p == 2u) {
        c = vec3(0.28, 0.52, 1.0);
    }
    if (a >= 3u) {
        c = mix(c, vec3(0.62, 0.68, 0.74), 0.55);
    }
    return c;
}

vec2 hub_px() {
    float margin = 96.0;
    return vec2(pc.fb_w - margin, 36.0 + margin);
}

vec2 to_ndc(vec2 px) {
    float w = max(pc.fb_w, 1.0);
    float h = max(pc.fb_h, 1.0);
    return vec2(px.x / w * 2.0 - 1.0, 1.0 - px.y / h * 2.0);
}

vec3 turned(vec3 dir) {
    return view_rotate(vec4(pc.qx, pc.qy, pc.qz, pc.qw), dir);
}

vec2 project_px(vec3 dir, float radius) {
    vec3 t = turned(dir);
    return hub_px() + vec2(t.x, -t.y) * radius;
}

vec2 quad_corner(vec2 a, vec2 b, float half_w, uint which) {
    vec2 d = b - a;
    float len = length(d);
    vec2 n = vec2(0.0, 1.0);
    if (len > 0.001) {
        d /= len;
        n = vec2(-d.y, d.x);
    }
    vec2 o = n * half_w;
    if (which == 0u) return a - o;
    if (which == 1u) return b - o;
    if (which == 2u) return b + o;
    return a + o;
}

void emit(vec2 px, vec3 color) {
    gl_Position = vec4(to_ndc(px), 0.0, 1.0);
    v_color = color;
}

void emit_drop() {
    gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    v_color = vec3(0.0);
}

void main() {
    uint vid = uint(gl_VertexIndex);
    if (vid < HUB_VERTS) {
        uint seg = vid / 3u;
        uint corner = vid % 3u;
        float t0 = 6.28318530718 * float(seg) / float(SEG);
        float t1 = 6.28318530718 * float(seg + 1u) / float(SEG);
        vec2 c = hub_px();
        vec2 p = c;
        if (corner == 1u) {
            p = c + vec2(cos(t0), sin(t0)) * 4.0;
        } else if (corner == 2u) {
            p = c + vec2(cos(t1), sin(t1)) * 4.0;
        }
        emit(p, vec3(0.82, 0.86, 0.90));
        return;
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
        vec3 u = vec3(1.0, 0.0, 0.0);
        vec3 v = vec3(0.0, 1.0, 0.0);
        vec3 col = axis_rgb(2u);
        if (arc == 1u) {
            u = vec3(0.0, 1.0, 0.0);
            v = vec3(0.0, 0.0, 1.0);
            col = axis_rgb(0u);
        } else if (arc == 2u) {
            u = vec3(0.0, 0.0, 1.0);
            v = vec3(1.0, 0.0, 0.0);
            col = axis_rgb(1u);
        }
        float a0 = 1.57079632679 * float(seg) / float(ARC_SEG);
        float a1 = 1.57079632679 * float(seg + 1u) / float(ARC_SEG);
        vec3 d0 = u * cos(a0) + v * sin(a0);
        vec3 d1 = u * cos(a1) + v * sin(a1);
        if (turned(d0).z < -0.05 && turned(d1).z < -0.05) {
            emit_drop();
            return;
        }
        vec2 p0 = project_px(d0, 36.0);
        vec2 p1 = project_px(d1, 36.0);
        emit(quad_corner(p0, p1, 1.35, corner), col);
        return;
    }
    vid -= ARC_VERTS;

    uint slot = vid / AXIS_VERTS;
    uint local = vid % AXIS_VERTS;
    if (slot >= 6u) {
        emit_drop();
        return;
    }
    uint a = (pc.ord >> (slot * 3u)) & 7u;
    bool positive = a < 3u;
    float reach = positive ? 54.0 : 30.0;
    float disc = positive ? 11.0 : 7.0;
    vec3 dir = axis_dir(a);
    vec3 col = axis_rgb(a);
    vec2 end = project_px(dir, reach);
    vec2 center = hub_px();
    float stem = length(end - center);
    bool facing = stem < 8.0;

    if (local < 6u) {
        if (facing) {
            emit_drop();
            return;
        }
        uint which = local;
        uint corner = which;
        if (which >= 3u) {
            corner = which == 3u ? 0u : (which == 4u ? 2u : 3u);
        }
        emit(quad_corner(center, end, 1.35, corner), col);
        return;
    }
    local -= 6u;

    vec2 at = facing ? center : end;
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
        vec2 i0 = at + vec2(cos(t0), sin(t0)) * inner;
        vec2 o0 = at + vec2(cos(t0), sin(t0)) * outer;
        vec2 i1 = at + vec2(cos(t1), sin(t1)) * inner;
        vec2 o1 = at + vec2(cos(t1), sin(t1)) * outer;
        vec2 p = i0;
        if (corner == 1u) p = o0;
        else if (corner == 2u) p = o1;
        else if (corner == 3u) p = i1;
        emit(p, col);
        return;
    }
    local -= SEG * 6u;

    if (local < SEG * 3u) {
        if (!positive) {
            emit_drop();
            return;
        }
        uint seg = local / 3u;
        uint corner = local % 3u;
        float t0 = 6.28318530718 * float(seg) / float(SEG);
        float t1 = 6.28318530718 * float(seg + 1u) / float(SEG);
        vec2 p = at;
        if (corner == 1u) {
            p = at + vec2(cos(t0), sin(t0)) * (disc - 1.6);
        } else if (corner == 2u) {
            p = at + vec2(cos(t1), sin(t1)) * (disc - 1.6);
        }
        emit(p, col);
        return;
    }
    local -= SEG * 3u;

    if (!positive) {
        emit_drop();
        return;
    }
    uint quad = local / 6u;
    uint which = local % 6u;
    uint corner = which;
    if (which >= 3u) {
        corner = which == 3u ? 0u : (which == 4u ? 2u : 3u);
    }
    // Offsets are +Y up. Pixel Y grows down, so the letter flips here.
    vec2 s0 = at;
    vec2 s1 = at;
    bool live = false;
    float hw = 1.15;
    if (a == 0u) {
        if (quad == 0u) {
            s0 = at + vec2(-4.2, 4.2);
            s1 = at + vec2(4.2, -4.2);
            live = true;
        } else if (quad == 1u) {
            s0 = at + vec2(-4.2, -4.2);
            s1 = at + vec2(4.2, 4.2);
            live = true;
        }
    } else if (a == 1u) {
        if (quad == 0u) {
            s0 = at + vec2(-4.4, -4.6);
            s1 = at + vec2(0.0, -0.6);
            live = true;
        } else if (quad == 1u) {
            s0 = at + vec2(4.4, -4.6);
            s1 = at + vec2(0.0, -0.6);
            live = true;
        } else if (quad == 2u) {
            s0 = at + vec2(0.0, -0.6);
            s1 = at + vec2(0.0, 4.4);
            live = true;
        }
    } else if (quad == 0u) {
        s0 = at + vec2(-4.2, -3.8);
        s1 = at + vec2(4.2, -3.8);
        live = true;
    } else if (quad == 1u) {
        s0 = at + vec2(4.2, -3.8);
        s1 = at + vec2(-4.2, 3.8);
        live = true;
    } else if (quad == 2u) {
        s0 = at + vec2(-4.2, 3.8);
        s1 = at + vec2(4.2, 3.8);
        live = true;
    }
    if (!live) {
        emit_drop();
        return;
    }
    emit(quad_corner(s0, s1, hw, corner), vec3(0.97, 0.98, 0.99));
}
