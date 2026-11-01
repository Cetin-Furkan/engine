// 16 by 16 samples on each flat face. The 888 fillet vertices stay where
// bevel.comp put them, and shaders/heat_nbr.slang is not extended.
// Corners of this grid are those fillet vertices. Every other sample is
// appended after them, per face: bottom edge, top edge, left edge, right
// edge, then the interior. cube.c k_grid must stay 16.

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
