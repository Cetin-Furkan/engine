#include "face_grid.slang"

static const uint k_arc = 8u;
static const float k_half_pi = 1.5707963;
static const uint k_face_verts = 24u;
static const uint k_edge_verts = 216u;
static const uint k_face_quads = 6u * k_grid * k_grid;
static const uint k_quads = k_face_quads + 12u * k_arc + 8u * k_arc * k_arc;

struct Push {
    uint64_t verts;
    uint64_t indices;
};
[[vk::push_constant]] Push pc;

void store_vert(uint vi, float3 p, float3 n, float3 c) {
    float* mesh = (float*)pc.verts;
    uint o = vi * 9u;
    mesh[o + 0u] = p.x;
    mesh[o + 1u] = p.y;
    mesh[o + 2u] = p.z;
    mesh[o + 3u] = n.x;
    mesh[o + 4u] = n.y;
    mesh[o + 5u] = n.z;
    mesh[o + 6u] = c.x;
    mesh[o + 7u] = c.y;
    mesh[o + 8u] = c.z;
}

void store_quad(uint q, uint a, uint b, uint c, uint d) {
    uint16_t* idx = (uint16_t*)pc.indices;
    uint o = q * 6u;
    idx[o + 0u] = uint16_t(a);
    idx[o + 1u] = uint16_t(b);
    idx[o + 2u] = uint16_t(c);
    idx[o + 3u] = uint16_t(a);
    idx[o + 4u] = uint16_t(c);
    idx[o + 5u] = uint16_t(d);
}

float3 color_of(float3 n) {
    if (n.x > 0.5)  return float3(0.72, 0.28, 0.22);
    if (n.x < -0.5) return float3(0.25, 0.48, 0.32);
    if (n.y > 0.5)  return float3(0.78, 0.74, 0.66);
    if (n.y < -0.5) return float3(0.30, 0.32, 0.38);
    if (n.z > 0.5)  return float3(0.82, 0.62, 0.28);
    return float3(0.35, 0.38, 0.62);
}

void edge_basis(uint e, out float3 n0, out float3 n1, out float3 d) {
    uint pair = e / 4u;
    uint bits = e % 4u;
    float a = (bits & 1u) == 0u ? 1.0 : -1.0;
    float bsgn = (bits & 2u) == 0u ? 1.0 : -1.0;
    if (pair == 0u)      { n0 = float3(a, 0, 0); n1 = float3(0, bsgn, 0); }
    else if (pair == 1u) { n0 = float3(0, a, 0); n1 = float3(0, 0, bsgn); }
    else                 { n0 = float3(0, 0, a); n1 = float3(bsgn, 0, 0); }
    d = cross(n0, n1);
}

void corner_basis(uint c, out float3 n0, out float3 n1, out float3 n2) {
    float sx = (c & 1u) == 0u ? 1.0 : -1.0;
    float sy = (c & 2u) == 0u ? 1.0 : -1.0;
    float sz = (c & 4u) == 0u ? 1.0 : -1.0;
    n0 = float3(sx, 0.0, 0.0);
    n1 = float3(0.0, sy, 0.0);
    n2 = float3(0.0, 0.0, sz);
    if (dot(n0, cross(n1, n2)) < 0.0) {
        float3 swap_n = n1;
        n1 = n2;
        n2 = swap_n;
    }
}

float3 corner_dir(float3 n0, float3 n1, float3 n2, float a, float b) {
    return normalize(cos(a) * cos(b) * n0 + sin(a) * cos(b) * n1 + sin(b) * n2);
}

float3 corner_color(float3 dir, float3 n0, float3 n1, float3 n2, float3 c0, float3 c1, float3 c2) {
    float w0 = max(dot(dir, n0), 0.0);
    float w1 = max(dot(dir, n1), 0.0);
    float w2 = max(dot(dir, n2), 0.0);
    return (w0 * c0 + w1 * c1 + w2 * c2) / (w0 + w1 + w2);
}

void write_vert(uint id) {
    if (id >= k_fillet_verts) {
        uint face;
        uint i;
        uint j;
        grid_decode(id, face, i, j);
        float3 n;
        float3 t;
        float3 b;
        face_basis(face, n, t, b);
        store_vert(id, face_point(face, i, j), n, color_of(n));
        return;
    }
    float m = 1.0 - k_radius;
    if (id < k_face_verts) {
        uint face = id / 4u;
        uint corner = id % 4u;
        float3 n;
        float3 t;
        float3 b;
        face_basis(face, n, t, b);
        float tu = (corner == 1u || corner == 2u) ? m : -m;
        float bv = (corner >= 2u) ? m : -m;
        store_vert(id, n + t * tu + b * bv, n, color_of(n));
        return;
    }
    if (id < k_face_verts + k_edge_verts) {
        uint local = id - k_face_verts;
        uint e = local / 18u;
        uint rem = local % 18u;
        uint row = rem / 9u;
        uint col = rem % 9u;
        float3 n0;
        float3 n1;
        float3 d;
        edge_basis(e, n0, n1, d);
        float u = float(col) / float(k_arc);
        float ang = u * k_half_pi;
        float3 nrm = cos(ang) * n0 + sin(ang) * n1;
        float s = row == 0u ? -m : m;
        float3 p = (n0 + n1) * m + d * s + k_radius * nrm;
        store_vert(id, p, nrm, lerp(color_of(n0), color_of(n1), u));
        return;
    }
    uint local = id - (k_face_verts + k_edge_verts);
    uint c = local / 81u;
    uint rem = local % 81u;
    uint iv = rem / 9u;
    uint iu = rem % 9u;
    float3 n0;
    float3 n1;
    float3 n2;
    corner_basis(c, n0, n1, n2);
    float3 dir = corner_dir(n0, n1, n2,
                          float(iu) / float(k_arc) * k_half_pi,
                          float(iv) / float(k_arc) * k_half_pi);
    float3 col = corner_color(dir, n0, n1, n2, color_of(n0), color_of(n1), color_of(n2));
    store_vert(id, (n0 + n1 + n2) * m + k_radius * dir, dir, col);
}

void write_quad(uint q) {
    if (q < k_face_quads) {
        uint cell = k_grid * k_grid;
        uint face = q / cell;
        uint rem = q - face * cell;
        uint j = rem / k_grid;
        uint i = rem - j * k_grid;
        store_quad(q,
                   grid_vertex(face, i, j),
                   grid_vertex(face, i + 1u, j),
                   grid_vertex(face, i + 1u, j + 1u),
                   grid_vertex(face, i, j + 1u));
        return;
    }
    uint edge_quads = 12u * k_arc;
    if (q < k_face_quads + edge_quads) {
        uint local = q - k_face_quads;
        uint e = local / k_arc;
        uint i = local % k_arc;
        uint base = k_face_verts + e * 18u;
        uint p00 = base + i;
        uint p10 = base + i + 1u;
        uint p01 = base + 9u + i;
        uint p11 = base + 9u + i + 1u;
        store_quad(q, p00, p10, p11, p01);
        return;
    }
    uint local = q - (k_face_quads + edge_quads);
    uint c = local / 64u;
    uint rem = local % 64u;
    uint iv = rem / k_arc;
    uint iu = rem % k_arc;
    uint base = k_face_verts + k_edge_verts + c * 81u;
    uint d00 = base + iv * 9u + iu;
    uint d10 = base + iv * 9u + iu + 1u;
    uint d01 = base + (iv + 1u) * 9u + iu;
    uint d11 = base + (iv + 1u) * 9u + iu + 1u;
    store_quad(q, d00, d10, d11, d01);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i < k_grid_verts) {
        write_vert(i);
    }
    if (i < k_quads) {
        write_quad(i);
    }
}
