#include "face_grid.slang"
#include "camera.slang"

static const float k_half_pi = 1.5707963;
static const uint k_face_verts = 24u;
static const uint k_edge_verts = 216u;
static const uint k_face_quads = 6u * k_grid * k_grid;
static const uint k_edge_quads = 12u * k_grid * k_arc;
static const uint k_quads = k_face_quads + k_edge_quads + 8u * k_arc * k_arc;

struct Push {
    uint64_t verts;
    uint64_t indices;
};
[[vk::push_constant]] Push pc;

void store_vert(uint vi, float3 p, float3 n, float3 c) {
    MeshVertex mv = { p, n, c };
    ((MeshVertex*)pc.verts)[vi] = mv;
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

float3 corner_dir(float3 n0, float3 n1, float3 n2, float a, float b) {
    return normalize(cos(a) * cos(b) * n0 + sin(a) * cos(b) * n1 + sin(b) * n2);
}

float3 corner_color(float3 dir, float3 n0, float3 n1, float3 n2, float3 c0, float3 c1, float3 c2) {
    float w0 = max(dot(dir, n0), 0.0);
    float w1 = max(dot(dir, n1), 0.0);
    float w2 = max(dot(dir, n2), 0.0);
    return (w0 * c0 + w1 * c1 + w2 * c2) / (w0 + w1 + w2);
}

void write_seam(uint id) {
    uint e;
    uint len;
    uint arc;
    seam_decode(id, e, len, arc);
    float3 n0;
    float3 n1;
    float3 d;
    edge_basis(e, n0, n1, d);
    float m = 1.0 - k_radius;
    float u = float(arc) / float(k_arc);
    float ang = u * k_half_pi;
    float3 nrm = cos(ang) * n0 + sin(ang) * n1;
    float s = lerp(-m, m, float(len) / float(k_grid));
    float3 p = (n0 + n1) * m + d * s + k_radius * nrm;
    store_vert(id, p, nrm, lerp(color_of(n0), color_of(n1), u));
}

void write_vert(uint id) {
    if (id >= k_grid_verts) {
        write_seam(id);
        return;
    }
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
    if (q < k_face_quads + k_edge_quads) {
        uint local = q - k_face_quads;
        uint per = k_grid * k_arc;
        uint e = local / per;
        uint rem = local - e * per;
        uint len = rem / k_arc;
        uint arc = rem - len * k_arc;
        store_quad(q,
                   edge_vert(e, len, arc),
                   edge_vert(e, len, arc + 1u),
                   edge_vert(e, len + 1u, arc + 1u),
                   edge_vert(e, len + 1u, arc));
        return;
    }
    uint local = q - (k_face_quads + k_edge_quads);
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
    if (i < k_mesh_verts) {
        write_vert(i);
    }
    if (i < k_quads) {
        write_quad(i);
    }
}
