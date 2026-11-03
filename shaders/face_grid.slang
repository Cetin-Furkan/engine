// 16 by 16 samples on each flat face. The 888 fillet vertices stay where
// bevel.comp put them, and shaders/heat_nbr.slang is not extended.
// Corners of this grid are those fillet vertices. Every other sample is
// appended after them, per face: bottom edge, top edge, left edge, right
// edge, then the interior. cube.c k_grid must stay 16.
//
// The bevel along an edge used to be a 2-by-9 strip that only shared the
// face corners. A dent moved the face membrane and left the strip behind.
// edge_vert welds the two rails of that bevel to this grid, including the
// corners. Samples strictly inside the arc and strictly inside the length
// are appended after k_grid_verts. Their ids are stable additions: nothing
// in 0 .. k_grid_verts moves, so the fillet neighbor table stays valid.

static const float k_radius = 0.10;
static const uint k_grid = 16u;
static const uint k_fillet_verts = 888u;
static const uint k_span = k_grid + 1u;
static const uint k_face_samples = k_span * k_span - 4u;
static const uint k_grid_verts = k_fillet_verts + 6u * k_face_samples;

void face_basis(uint face, out float3 n, out float3 t, out float3 b) {
    if (face == 0u)      { n = float3( 1, 0, 0); t = float3(0, 0,-1); b = float3(0, 1, 0); }
    else if (face == 1u) { n = float3(-1, 0, 0); t = float3(0, 0, 1); b = float3(0, 1, 0); }
    else if (face == 2u) { n = float3(0,  1, 0); t = float3(1, 0, 0); b = float3(0, 0,-1); }
    else if (face == 3u) { n = float3(0, -1, 0); t = float3(1, 0, 0); b = float3(0, 0, 1); }
    else if (face == 4u) { n = float3(0, 0,  1); t = float3(1, 0, 0); b = float3(0, 1, 0); }
    else                 { n = float3(0, 0, -1); t = float3(-1, 0, 0); b = float3(0, 1, 0); }
}

float3 face_point(uint face, uint i, uint j) {
    float3 n;
    float3 t;
    float3 b;
    face_basis(face, n, t, b);
    float m = 1.0 - k_radius;
    float tu = lerp(-m, m, float(i) / float(k_grid));
    float bv = lerp(-m, m, float(j) / float(k_grid));
    return n + t * tu + b * bv;
}

uint grid_vertex(uint face, uint i, uint j) {
    if (i == 0u && j == 0u) {
        return face * 4u;
    }
    if (i == k_grid && j == 0u) {
        return face * 4u + 1u;
    }
    if (i == k_grid && j == k_grid) {
        return face * 4u + 2u;
    }
    if (i == 0u && j == k_grid) {
        return face * 4u + 3u;
    }
    uint edge = k_grid - 1u;
    uint base = k_fillet_verts + face * k_face_samples;
    if (j == 0u) {
        return base + (i - 1u);
    }
    if (j == k_grid) {
        return base + edge + (i - 1u);
    }
    if (i == 0u) {
        return base + 2u * edge + (j - 1u);
    }
    if (i == k_grid) {
        return base + 3u * edge + (j - 1u);
    }
    return base + 4u * edge + (j - 1u) * edge + (i - 1u);
}

void grid_decode(uint id, out uint face, out uint i, out uint j) {
    uint local = id - k_fillet_verts;
    uint edge = k_grid - 1u;
    face = local / k_face_samples;
    uint k = local - face * k_face_samples;
    if (k < edge) {
        i = k + 1u;
        j = 0u;
        return;
    }
    if (k < 2u * edge) {
        i = k - edge + 1u;
        j = k_grid;
        return;
    }
    if (k < 3u * edge) {
        i = 0u;
        j = k - 2u * edge + 1u;
        return;
    }
    if (k < 4u * edge) {
        i = k_grid;
        j = k - 3u * edge + 1u;
        return;
    }
    uint t = k - 4u * edge;
    i = (t % edge) + 1u;
    j = (t / edge) + 1u;
}

bool face_corner_at(float3 p, out uint face, out uint i, out uint j) {
    float m = 1.0 - k_radius;
    const float e = 0.0001;
    float ax = abs(p.x);
    float ay = abs(p.y);
    float az = abs(p.z);
    bool on_x = abs(ax - 1.0) < e && abs(ay - m) < e && abs(az - m) < e;
    bool on_y = abs(ay - 1.0) < e && abs(ax - m) < e && abs(az - m) < e;
    bool on_z = abs(az - 1.0) < e && abs(ax - m) < e && abs(ay - m) < e;
    if (on_x) {
        face = p.x > 0.0 ? 0u : 1u;
    } else if (on_y) {
        face = p.y > 0.0 ? 2u : 3u;
    } else if (on_z) {
        face = p.z > 0.0 ? 4u : 5u;
    } else {
        face = 0u;
        i = 0u;
        j = 0u;
        return false;
    }
    float3 n;
    float3 t;
    float3 b;
    face_basis(face, n, t, b);
    float tu = dot(p - n, t);
    float bv = dot(p - n, b);
    i = tu > 0.0 ? k_grid : 0u;
    j = bv > 0.0 ? k_grid : 0u;
    return true;
}

// k_arc is the bevel tessellation. The two rails (arc 0 and arc k_arc)
// are face-boundary vertices. The two length ends reuse the original
// fillet rows. Everything else is a seam vertex past the face grid.
static const uint k_arc = 8u;
static const uint k_face_corners = 24u;
static const uint k_edge_cols = k_arc + 1u;
static const uint k_seam_verts = 12u * (k_grid - 1u) * (k_arc - 1u);
static const uint k_mesh_verts = k_grid_verts + k_seam_verts;

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

uint face_of_normal(float3 n) {
    if (n.x > 0.5)  return 0u;
    if (n.x < -0.5) return 1u;
    if (n.y > 0.5)  return 2u;
    if (n.y < -0.5) return 3u;
    if (n.z > 0.5)  return 4u;
    return 5u;
}

// along is dot(edge direction, face tangent). across is dot(the other
// face normal, that tangent). Both are exactly -1, 0, or 1, so the index
// is exact and does not round a reconstructed position.
uint axis_index(float along, float across, uint len) {
    if (along > 0.5) {
        return len;
    }
    if (along < -0.5) {
        return k_grid - len;
    }
    return across > 0.0 ? k_grid : 0u;
}

// rail 0 sits on face n0. rail 1 sits on face n1. len runs the edge.
uint face_boundary_vertex(uint e, uint len, uint rail) {
    float3 n0;
    float3 n1;
    float3 d;
    edge_basis(e, n0, n1, d);
    float3 face_n = rail == 0u ? n0 : n1;
    float3 outward = rail == 0u ? n1 : n0;
    uint face = face_of_normal(face_n);
    float3 n;
    float3 t;
    float3 b;
    face_basis(face, n, t, b);
    uint i = axis_index(dot(d, t), dot(outward, t), len);
    uint j = axis_index(dot(d, b), dot(outward, b), len);
    return grid_vertex(face, i, j);
}

void seam_decode(uint id, out uint e, out uint len, out uint arc) {
    uint local = id - k_grid_verts;
    uint span = k_arc - 1u;
    uint per = (k_grid - 1u) * span;
    e = local / per;
    uint rem = local - e * per;
    len = rem / span + 1u;
    arc = rem - (len - 1u) * span + 1u;
}

// Index of one bevel sample. The rails always alias the face grid, so a
// face quad and the bevel quad that leaves that edge share vertices.
uint edge_vert(uint e, uint len, uint arc) {
    if (arc == 0u || arc == k_arc) {
        return face_boundary_vertex(e, len, arc == 0u ? 0u : 1u);
    }
    if (len == 0u || len == k_grid) {
        uint row = len == 0u ? 0u : 1u;
        return k_face_corners + e * (k_edge_cols * 2u) + row * k_edge_cols + arc;
    }
    return k_grid_verts + (e * (k_grid - 1u) + (len - 1u)) * (k_arc - 1u) + (arc - 1u);
}

// Eight spherical caps, one per cube corner, packed after the fillet
// edge rows. Each patch is k_edge_cols by k_edge_cols. (iu, iv) = (0, 0)
// and (k_arc, 0), and the whole iv = k_arc row, are the three inset face
// corners. cube.vert blends those corners. The ids stay put, so
// heat_nbr.slang is not regenerated.
static const uint k_corner_base = k_face_corners + 12u * k_edge_cols * 2u;
static const uint k_corner_samples = k_edge_cols * k_edge_cols;

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

// Inset corner of the face whose normal is face_n, on the side of the
// other two corner axes. The id is a face-grid corner, the sample the
// membrane actually moves.
uint face_corner_vertex(float3 face_n, float3 side_a, float3 side_b) {
    uint face = face_of_normal(face_n);
    float3 n;
    float3 t;
    float3 b;
    face_basis(face, n, t, b);
    float along_t = dot(side_a, t) + dot(side_b, t);
    float along_b = dot(side_a, b) + dot(side_b, b);
    uint i = along_t > 0.0 ? k_grid : 0u;
    uint j = along_b > 0.0 ? k_grid : 0u;
    return grid_vertex(face, i, j);
}
