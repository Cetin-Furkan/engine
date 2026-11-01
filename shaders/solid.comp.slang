
// One invocation writes one vertex. Faceted cages duplicate a face normal.
// Rounded polyhedra subdivide each face into 4x4 and project onto the
// circumsphere, so the silhouette becomes an arc. Rounded lathes use a
// denser ring and an analytic normal. Counts match src/frame/solid.c.
//
// slot    kind        cage      verts
// 0       tetrahedron faceted   12
// 12      tetrahedron rounded   192
// 204     octahedron  faceted   24
// 228     octahedron  rounded   384
// 612     icosahedron faceted   60
// 672     icosahedron rounded   960
// 1632    cylinder    faceted   72
// 1704    cylinder    rounded   576
// 2280    cone        faceted   36
// 2316    cone        rounded   288
// 2604    torus       faceted   288
// 2892    torus       rounded   3072
// end 5964



struct BakePush { uint64_t verts; };
[[vk::push_constant]] BakePush bake;

static const uint k_total = 5964u;
static const uint k_sub = 4u;
static const float k_tau = 6.28318530718;
static const float k_cyl_r = 0.72;
static const float k_cyl_y = 0.85;
static const float k_cone_r = 0.78;
static const float k_cone_y = 0.88;
static const float k_torus_r = 0.70;
static const float k_torus_s = 0.26;

static const float3 k_tetra[4] = {
    float3(0.57735027, 0.57735027, 0.57735027),
    float3(0.57735027, -0.57735027, -0.57735027),
    float3(-0.57735027, 0.57735027, -0.57735027),
    float3(-0.57735027, -0.57735027, 0.57735027)
};
static const int3 k_tetra_f[4] = {
    int3(0, 1, 2), int3(0, 2, 3), int3(0, 3, 1), int3(1, 3, 2)
};

static const float3 k_octa[6] = {
    float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0),
    float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0)
};
static const int3 k_octa_f[8] = {
    int3(0, 2, 4), int3(0, 5, 2), int3(0, 4, 3), int3(0, 3, 5),
    int3(1, 4, 2), int3(1, 2, 5), int3(1, 3, 4), int3(1, 5, 3)
};

static const float3 k_ico[12] = {
    float3(-0.52573111, -0.85065081, 0.0),
    float3(-0.52573111, 0.85065081, 0.0),
    float3(0.52573111, -0.85065081, 0.0),
    float3(0.52573111, 0.85065081, 0.0),
    float3(0.0, -0.52573111, -0.85065081),
    float3(0.0, -0.52573111, 0.85065081),
    float3(0.0, 0.52573111, -0.85065081),
    float3(0.0, 0.52573111, 0.85065081),
    float3(-0.85065081, 0.0, -0.52573111),
    float3(0.85065081, 0.0, -0.52573111),
    float3(-0.85065081, 0.0, 0.52573111),
    float3(0.85065081, 0.0, 0.52573111)
};
static const int3 k_ico_f[20] = {
    int3(0, 4, 2), int3(0, 2, 5), int3(0, 8, 4), int3(0, 5, 10), int3(0, 10, 8),
    int3(1, 3, 6), int3(1, 7, 3), int3(1, 6, 8), int3(1, 10, 7), int3(1, 8, 10),
    int3(2, 4, 9), int3(2, 11, 5), int3(2, 9, 11),
    int3(3, 9, 6), int3(3, 7, 11), int3(3, 11, 9),
    int3(4, 8, 6), int3(4, 6, 9),
    int3(5, 7, 10), int3(5, 11, 7)
};

bool locate(uint slot, out uint kind, out uint curved, out uint local_id) {
    if (slot < 12u) { kind = 0u; curved = 0u; local_id = slot; return true; }
    if (slot < 204u) { kind = 0u; curved = 1u; local_id = slot - 12u; return true; }
    if (slot < 228u) { kind = 1u; curved = 0u; local_id = slot - 204u; return true; }
    if (slot < 612u) { kind = 1u; curved = 1u; local_id = slot - 228u; return true; }
    if (slot < 672u) { kind = 2u; curved = 0u; local_id = slot - 612u; return true; }
    if (slot < 1632u) { kind = 2u; curved = 1u; local_id = slot - 672u; return true; }
    if (slot < 1704u) { kind = 3u; curved = 0u; local_id = slot - 1632u; return true; }
    if (slot < 2280u) { kind = 3u; curved = 1u; local_id = slot - 1704u; return true; }
    if (slot < 2316u) { kind = 4u; curved = 0u; local_id = slot - 2280u; return true; }
    if (slot < 2604u) { kind = 4u; curved = 1u; local_id = slot - 2316u; return true; }
    if (slot < 2892u) { kind = 5u; curved = 0u; local_id = slot - 2604u; return true; }
    if (slot < k_total) { kind = 5u; curved = 1u; local_id = slot - 2892u; return true; }
    kind = 0u;
    curved = 0u;
    local_id = 0u;
    return false;
}

float3 kind_color(uint kind) {
    if (kind == 0u) return float3(0.86, 0.32, 0.28);
    if (kind == 1u) return float3(0.32, 0.74, 0.42);
    if (kind == 2u) return float3(0.36, 0.52, 0.88);
    if (kind == 3u) return float3(0.90, 0.74, 0.28);
    if (kind == 4u) return float3(0.66, 0.40, 0.84);
    return float3(0.26, 0.74, 0.76);
}

float3 face_normal(float3 a, float3 b, float3 c) {
    return normalize(cross(b - a, c - a));
}

void poly_corners(uint kind, uint face, out float3 a, out float3 b, out float3 c) {
    int3 f = int3(0);
    if (kind == 0u) {
        f = k_tetra_f[face];
        a = k_tetra[f.x];
        b = k_tetra[f.y];
        c = k_tetra[f.z];
    } else if (kind == 1u) {
        f = k_octa_f[face];
        a = k_octa[f.x];
        b = k_octa[f.y];
        c = k_octa[f.z];
    } else {
        f = k_ico_f[face];
        a = k_ico[f.x];
        b = k_ico[f.y];
        c = k_ico[f.z];
    }
}

float3 sphere_point(float3 a, float3 b, float3 c, uint i, uint j) {
    float3 p = float(k_sub - i - j) * a + float(i) * b + float(j) * c;
    return normalize(p);
}

void poly_vertex(uint kind, uint curved, uint local_id, out float3 pos, out float3 nrm) {
    if (curved == 0u) {
        uint face = local_id / 3u;
        uint corner = local_id % 3u;
        float3 a, b, c;
        poly_corners(kind, face, a, b, c);
        pos = corner == 0u ? a : (corner == 1u ? b : c);
        nrm = face_normal(a, b, c);
        return;
    }
    uint per_face = k_sub * k_sub * 3u;
    uint face = local_id / per_face;
    uint rem = local_id % per_face;
    uint tri = rem / 3u;
    uint corner = rem % 3u;
    uint ii = 0u;
    uint jj = 0u;
    uint which = 0u;
    uint seen = 0u;
    for (uint i = 0u; i < k_sub; ++i) {
        for (uint j = 0u; j < k_sub - i; ++j) {
            if (seen == tri) {
                ii = i;
                jj = j;
                which = 0u;
            }
            seen++;
            if (i + j + 1u < k_sub) {
                if (seen == tri) {
                    ii = i;
                    jj = j;
                    which = 1u;
                }
                seen++;
            }
        }
    }
    uint i0 = ii;
    uint j0 = jj;
    uint i1 = ii + 1u;
    uint j1 = jj;
    uint i2 = ii;
    uint j2 = jj + 1u;
    if (which == 1u) {
        i0 = ii + 1u;
        j0 = jj;
        i1 = ii + 1u;
        j1 = jj + 1u;
        i2 = ii;
        j2 = jj + 1u;
    }
    uint pi = corner == 0u ? i0 : (corner == 1u ? i1 : i2);
    uint pj = corner == 0u ? j0 : (corner == 1u ? j1 : j2);
    float3 a, b, c;
    poly_corners(kind, face, a, b, c);
    pos = sphere_point(a, b, c, pi, pj);
    nrm = pos;
}

void cylinder_vertex(uint curved, uint local_id, out float3 pos, out float3 nrm) {
    uint n = curved == 0u ? 6u : 48u;
    uint s = local_id / 12u;
    uint k = local_id % 12u;
    float a0 = k_tau * float(s) / float(n);
    float a1 = k_tau * float(s + 1u) / float(n);
    float3 b0 = float3(k_cyl_r * cos(a0), -k_cyl_y, k_cyl_r * sin(a0));
    float3 b1 = float3(k_cyl_r * cos(a1), -k_cyl_y, k_cyl_r * sin(a1));
    float3 t0 = float3(k_cyl_r * cos(a0), k_cyl_y, k_cyl_r * sin(a0));
    float3 t1 = float3(k_cyl_r * cos(a1), k_cyl_y, k_cyl_r * sin(a1));
    float3 radial0 = float3(cos(a0), 0.0, sin(a0));
    float3 radial1 = float3(cos(a1), 0.0, sin(a1));
    float3 side = face_normal(b0, t0, t1);
    if (k < 3u) {
        pos = k == 0u ? b0 : (k == 1u ? t0 : t1);
        nrm = curved == 0u ? side : (k == 0u ? radial0 : (k == 1u ? radial0 : radial1));
        return;
    }
    if (k < 6u) {
        uint e = k - 3u;
        pos = e == 0u ? b0 : (e == 1u ? t1 : b1);
        nrm = curved == 0u ? side : (e == 0u ? radial0 : (e == 1u ? radial1 : radial1));
        return;
    }
    if (k < 9u) {
        uint e = k - 6u;
        float3 cap = float3(0.0, k_cyl_y, 0.0);
        pos = e == 0u ? cap : (e == 1u ? t1 : t0);
        nrm = float3(0.0, 1.0, 0.0);
        return;
    }
    uint e = k - 9u;
    float3 cap = float3(0.0, -k_cyl_y, 0.0);
    pos = e == 0u ? cap : (e == 1u ? b0 : b1);
    nrm = float3(0.0, -1.0, 0.0);
}

void cone_vertex(uint curved, uint local_id, out float3 pos, out float3 nrm) {
    uint n = curved == 0u ? 6u : 48u;
    uint s = local_id / 6u;
    uint k = local_id % 6u;
    float a0 = k_tau * float(s) / float(n);
    float a1 = k_tau * float(s + 1u) / float(n);
    float3 apex = float3(0.0, k_cone_y, 0.0);
    float3 b0 = float3(k_cone_r * cos(a0), -k_cone_y, k_cone_r * sin(a0));
    float3 b1 = float3(k_cone_r * cos(a1), -k_cone_y, k_cone_r * sin(a1));
    float rise = k_cone_y * 2.0;
    float3 n0 = normalize(float3(cos(a0) * rise, k_cone_r, sin(a0) * rise));
    float3 n1 = normalize(float3(cos(a1) * rise, k_cone_r, sin(a1) * rise));
    float3 facet = face_normal(apex, b1, b0);
    if (k < 3u) {
        pos = k == 0u ? apex : (k == 1u ? b1 : b0);
        nrm = curved == 0u ? facet : (k == 1u ? n1 : n0);
        return;
    }
    uint e = k - 3u;
    float3 cap = float3(0.0, -k_cone_y, 0.0);
    pos = e == 0u ? cap : (e == 1u ? b0 : b1);
    nrm = float3(0.0, -1.0, 0.0);
}

float3 torus_p(float u, float v) {
    float ring = k_torus_r + k_torus_s * cos(v);
    return float3(ring * cos(u), k_torus_s * sin(v), ring * sin(u));
}

float3 torus_n(float u, float v) {
    return float3(cos(v) * cos(u), sin(v), cos(v) * sin(u));
}

void torus_vertex(uint curved, uint local_id, out float3 pos, out float3 nrm) {
    uint nu = curved == 0u ? 8u : 32u;
    uint nv = curved == 0u ? 6u : 16u;
    uint q = local_id / 6u;
    uint k = local_id % 6u;
    uint iu = q / nv;
    uint iv = q % nv;
    float u0 = k_tau * float(iu) / float(nu);
    float u1 = k_tau * float(iu + 1u) / float(nu);
    float v0 = k_tau * float(iv) / float(nv);
    float v1 = k_tau * float(iv + 1u) / float(nv);
    float3 p00 = torus_p(u0, v0);
    float3 p10 = torus_p(u1, v0);
    float3 p11 = torus_p(u1, v1);
    float3 p01 = torus_p(u0, v1);
    float3 facet = face_normal(p00, p01, p10);
    if (k < 3u) {
        pos = k == 0u ? p00 : (k == 1u ? p01 : p10);
        nrm = curved == 0u ? facet : (k == 0u ? torus_n(u0, v0) : (k == 1u ? torus_n(u0, v1) : torus_n(u1, v0)));
        return;
    }
    uint e = k - 3u;
    facet = face_normal(p01, p11, p10);
    pos = e == 0u ? p01 : (e == 1u ? p11 : p10);
    nrm = curved == 0u ? facet : (e == 0u ? torus_n(u0, v1) : (e == 1u ? torus_n(u1, v1) : torus_n(u1, v0)));
}

void store(uint slot, float3 pos, float3 nrm, float3 color) {
    float* mesh = (float*)bake.verts;
    uint o = slot * 9u;
    mesh[o + 0u] = pos.x;
    mesh[o + 1u] = pos.y;
    mesh[o + 2u] = pos.z;
    mesh[o + 3u] = nrm.x;
    mesh[o + 4u] = nrm.y;
    mesh[o + 5u] = nrm.z;
    mesh[o + 6u] = color.x;
    mesh[o + 7u] = color.y;
    mesh[o + 8u] = color.z;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint slot = id.x;
    if (slot >= k_total) {
        return;
    }
    uint kind = 0u;
    uint curved = 0u;
    uint local_id = 0u;
    if (!locate(slot, kind, curved, local_id)) {
        return;
    }
    float3 pos = float3(0.0);
    float3 nrm = float3(0.0, 1.0, 0.0);
    if (kind < 3u) {
        poly_vertex(kind, curved, local_id, pos, nrm);
    } else if (kind == 3u) {
        cylinder_vertex(curved, local_id, pos, nrm);
    } else if (kind == 4u) {
        cone_vertex(curved, local_id, pos, nrm);
    } else {
        torus_vertex(curved, local_id, pos, nrm);
    }
    store(slot, pos, nrm, kind_color(kind));
}
