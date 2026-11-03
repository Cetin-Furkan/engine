#include "frame/cube.h"
#include "render/cube_int.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// CPU triangle walk of the fillet the vertex shader draws.
// Rest positions and indices are the bake. Displacement is the heat
// step whose acquire wait has retired. Seam samples and the fillet rows
// at each edge end lerp the two face rails. The spherical cap blends the
// three face corners around it. A fillet copy on a face corner uses that
// corner. The rules are the ones in cube.vert. The walk does not submit.

enum : uint32_t {
    k_arc            = 8u,
    k_grid           = 16u,
    k_face_corners   = 24u,
    k_edge_cols      = k_arc + 1u,
    k_corner_base    = k_face_corners + 12u * k_edge_cols * 2u,
    k_corner_samples = k_edge_cols * k_edge_cols,
    k_fillet_verts   = 888u,
    k_grid_verts     = k_pick_grid_verts,
    k_verts          = k_pick_verts,
    k_indices        = k_pick_indices,
    k_quads          = k_indices / 6u,
};

static_assert(k_grid_verts == 2598u, "pick grid verts");
static_assert(k_verts == 3858u, "pick verts");
static_assert(k_indices == 21504u, "pick indices");
static_assert(k_quads == 3584u, "pick quads");
static_assert(k_corner_base == 240u, "corner cap base");
static_assert(k_corner_base + 8u * k_corner_samples == k_fillet_verts, "corner cap count");

static const float k_radius = 0.10f;
static const float k_deg    = 0.017453292519943295f;
static const float k_edge   = 120.0f;

static float    s_rest[k_verts * 3u];
static float    s_nrm[k_verts * 3u];
static uint16_t s_index[k_indices];
static bool     s_ready = false;

static float s_disp[k_grid_verts * 3u];
static float s_world[k_verts * 3u];

void cube_pick_reset(void) {
    s_ready = false;
}

static void v3_set(float o[3], float x, float y, float z) {
    o[0] = x;
    o[1] = y;
    o[2] = z;
}

static float v3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void v3_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void face_basis(uint32_t face, float n[3], float t[3], float b[3]) {
    if (face == 0u) {
        v3_set(n, 1, 0, 0);
        v3_set(t, 0, 0, -1);
        v3_set(b, 0, 1, 0);
    } else if (face == 1u) {
        v3_set(n, -1, 0, 0);
        v3_set(t, 0, 0, 1);
        v3_set(b, 0, 1, 0);
    } else if (face == 2u) {
        v3_set(n, 0, 1, 0);
        v3_set(t, 1, 0, 0);
        v3_set(b, 0, 0, -1);
    } else if (face == 3u) {
        v3_set(n, 0, -1, 0);
        v3_set(t, 1, 0, 0);
        v3_set(b, 0, 0, 1);
    } else if (face == 4u) {
        v3_set(n, 0, 0, 1);
        v3_set(t, 1, 0, 0);
        v3_set(b, 0, 1, 0);
    } else {
        v3_set(n, 0, 0, -1);
        v3_set(t, -1, 0, 0);
        v3_set(b, 0, 1, 0);
    }
}

static uint32_t grid_vertex(uint32_t face, uint32_t i, uint32_t j) {
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
    uint32_t edge = k_grid - 1u;
    uint32_t base = k_fillet_verts + face * ((k_grid + 1u) * (k_grid + 1u) - 4u);
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

static bool face_corner_at(const float p[3], uint32_t *face, uint32_t *i, uint32_t *j) {
    float m = 1.0f - k_radius;
    const float e = 0.0001f;
    float ax = fabsf(p[0]);
    float ay = fabsf(p[1]);
    float az = fabsf(p[2]);
    bool on_x = fabsf(ax - 1.0f) < e && fabsf(ay - m) < e && fabsf(az - m) < e;
    bool on_y = fabsf(ay - 1.0f) < e && fabsf(ax - m) < e && fabsf(az - m) < e;
    bool on_z = fabsf(az - 1.0f) < e && fabsf(ax - m) < e && fabsf(ay - m) < e;
    if (on_x) {
        *face = p[0] > 0.0f ? 0u : 1u;
    } else if (on_y) {
        *face = p[1] > 0.0f ? 2u : 3u;
    } else if (on_z) {
        *face = p[2] > 0.0f ? 4u : 5u;
    } else {
        *face = 0u;
        *i = 0u;
        *j = 0u;
        return false;
    }
    float n[3];
    float t[3];
    float b[3];
    face_basis(*face, n, t, b);
    float rel[3] = { p[0] - n[0], p[1] - n[1], p[2] - n[2] };
    float tu = v3_dot(rel, t);
    float bv = v3_dot(rel, b);
    *i = tu > 0.0f ? k_grid : 0u;
    *j = bv > 0.0f ? k_grid : 0u;
    return true;
}

static uint32_t face_of_normal(const float n[3]) {
    if (n[0] > 0.5f) {
        return 0u;
    }
    if (n[0] < -0.5f) {
        return 1u;
    }
    if (n[1] > 0.5f) {
        return 2u;
    }
    if (n[1] < -0.5f) {
        return 3u;
    }
    if (n[2] > 0.5f) {
        return 4u;
    }
    return 5u;
}

static uint32_t axis_index(float along, float across, uint32_t len) {
    if (along > 0.5f) {
        return len;
    }
    if (along < -0.5f) {
        return k_grid - len;
    }
    return across > 0.0f ? k_grid : 0u;
}

static void edge_basis(uint32_t e, float n0[3], float n1[3], float d[3]) {
    uint32_t pair = e / 4u;
    uint32_t bits = e % 4u;
    float a = (bits & 1u) == 0u ? 1.0f : -1.0f;
    float bsgn = (bits & 2u) == 0u ? 1.0f : -1.0f;
    if (pair == 0u) {
        v3_set(n0, a, 0, 0);
        v3_set(n1, 0, bsgn, 0);
    } else if (pair == 1u) {
        v3_set(n0, 0, a, 0);
        v3_set(n1, 0, 0, bsgn);
    } else {
        v3_set(n0, 0, 0, a);
        v3_set(n1, bsgn, 0, 0);
    }
    v3_cross(n0, n1, d);
}

static uint32_t face_boundary_vertex(uint32_t e, uint32_t len, uint32_t rail) {
    float n0[3];
    float n1[3];
    float d[3];
    edge_basis(e, n0, n1, d);
    const float *face_n = rail == 0u ? n0 : n1;
    const float *outward = rail == 0u ? n1 : n0;
    uint32_t face = face_of_normal(face_n);
    float n[3];
    float t[3];
    float b[3];
    face_basis(face, n, t, b);
    uint32_t i = axis_index(v3_dot(d, t), v3_dot(outward, t), len);
    uint32_t j = axis_index(v3_dot(d, b), v3_dot(outward, b), len);
    return grid_vertex(face, i, j);
}

static void corner_basis(uint32_t c, float n0[3], float n1[3], float n2[3]) {
    float sx = (c & 1u) == 0u ? 1.0f : -1.0f;
    float sy = (c & 2u) == 0u ? 1.0f : -1.0f;
    float sz = (c & 4u) == 0u ? 1.0f : -1.0f;
    v3_set(n0, sx, 0.0f, 0.0f);
    v3_set(n1, 0.0f, sy, 0.0f);
    v3_set(n2, 0.0f, 0.0f, sz);
    float cross_n[3];
    v3_cross(n1, n2, cross_n);
    if (v3_dot(n0, cross_n) < 0.0f) {
        float swap_n[3] = { n1[0], n1[1], n1[2] };
        n1[0] = n2[0];
        n1[1] = n2[1];
        n1[2] = n2[2];
        n2[0] = swap_n[0];
        n2[1] = swap_n[1];
        n2[2] = swap_n[2];
    }
}

static uint32_t face_corner_vertex(const float face_n[3], const float side_a[3], const float side_b[3]) {
    uint32_t face = face_of_normal(face_n);
    float n[3];
    float t[3];
    float b[3];
    face_basis(face, n, t, b);
    float along_t = v3_dot(side_a, t) + v3_dot(side_b, t);
    float along_b = v3_dot(side_a, b) + v3_dot(side_b, b);
    uint32_t i = along_t > 0.0f ? k_grid : 0u;
    uint32_t j = along_b > 0.0f ? k_grid : 0u;
    return grid_vertex(face, i, j);
}

static void seam_decode(uint32_t id, uint32_t *e, uint32_t *len, uint32_t *arc) {
    uint32_t local = id - k_grid_verts;
    uint32_t span = k_arc - 1u;
    uint32_t per = (k_grid - 1u) * span;
    *e = local / per;
    uint32_t rem = local - *e * per;
    *len = rem / span + 1u;
    *arc = rem - (*len - 1u) * span + 1u;
}

static void raw_disp(uint32_t id, float o[3]) {
    if (id >= k_grid_verts) {
        v3_set(o, 0, 0, 0);
        return;
    }
    const float *d = s_disp + (size_t)id * 3u;
    o[0] = d[0];
    o[1] = d[1];
    o[2] = d[2];
}

static void v3_lerp(const float a[3], const float b[3], float u, float o[3]) {
    o[0] = a[0] + (b[0] - a[0]) * u;
    o[1] = a[1] + (b[1] - a[1]) * u;
    o[2] = a[2] + (b[2] - a[2]) * u;
}

// The displacement cube.vert adds before the pose.
static void shown_disp(uint32_t id, float o[3]) {
    if (id >= k_verts) {
        v3_set(o, 0, 0, 0);
        return;
    }
    if (id >= k_grid_verts) {
        uint32_t e = 0;
        uint32_t len = 0;
        uint32_t arc = 0;
        seam_decode(id, &e, &len, &arc);
        float u = (float)arc / (float)k_arc;
        float a[3];
        float b[3];
        raw_disp(face_boundary_vertex(e, len, 0u), a);
        raw_disp(face_boundary_vertex(e, len, 1u), b);
        v3_lerp(a, b, u, o);
        return;
    }
    if (id >= k_face_corners && id < k_corner_base) {
        uint32_t local = id - k_face_corners;
        uint32_t span = k_edge_cols * 2u;
        uint32_t e = local / span;
        uint32_t rem = local - e * span;
        uint32_t row = rem / k_edge_cols;
        uint32_t col = rem - row * k_edge_cols;
        uint32_t len = row == 0u ? 0u : k_grid;
        float u = (float)col / (float)k_arc;
        float a[3];
        float b[3];
        raw_disp(face_boundary_vertex(e, len, 0u), a);
        raw_disp(face_boundary_vertex(e, len, 1u), b);
        v3_lerp(a, b, u, o);
        return;
    }
    if (id >= k_corner_base && id < k_fillet_verts) {
        uint32_t local = id - k_corner_base;
        uint32_t c = local / k_corner_samples;
        uint32_t rem = local - c * k_corner_samples;
        uint32_t iv = rem / k_edge_cols;
        uint32_t iu = rem - iv * k_edge_cols;
        float n0[3];
        float n1[3];
        float n2[3];
        corner_basis(c, n0, n1, n2);
        float d0[3];
        float d1[3];
        float d2[3];
        raw_disp(face_corner_vertex(n0, n1, n2), d0);
        raw_disp(face_corner_vertex(n1, n0, n2), d1);
        raw_disp(face_corner_vertex(n2, n0, n1), d2);
        float ua = (float)iu / (float)k_arc;
        float ub = (float)iv / (float)k_arc;
        float ab[3];
        v3_lerp(d0, d1, ua, ab);
        v3_lerp(ab, d2, ub, o);
        return;
    }
    if (id < k_fillet_verts) {
        const float *rest = s_rest + (size_t)id * 3u;
        uint32_t face = 0;
        uint32_t i = 0;
        uint32_t j = 0;
        if (face_corner_at(rest, &face, &i, &j)) {
            uint32_t canon = grid_vertex(face, i, j);
            if (canon != id) {
                raw_disp(canon, o);
                return;
            }
        }
    }
    raw_disp(id, o);
}

static void pose_world(const float in[3], float yaw_deg, float origin_x, float origin_y, float o[3]) {
    float ax = 28.0f * k_deg;
    float ay = yaw_deg * k_deg;
    float cx = cosf(ax);
    float sx = sinf(ax);
    float rx = in[0];
    float ry = in[1] * cx - in[2] * sx;
    float rz = in[1] * sx + in[2] * cx;
    float cy = cosf(ay);
    float sy = sinf(ay);
    o[0] = (rx * cy + rz * sy) * k_edge + origin_x;
    o[1] = ry * k_edge + origin_y;
    o[2] = (-rx * sy + rz * cy) * k_edge;
}

static bool ray_tri(const float o[3], const float d[3], const float a[3], const float b[3], const float c[3],
                    float *t_hit) {
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float p[3];
    v3_cross(d, e2, p);
    float det = v3_dot(e1, p);
    if (fabsf(det) < 1.0e-6f) {
        return false;
    }
    float inv = 1.0f / det;
    float s[3] = { o[0] - a[0], o[1] - a[1], o[2] - a[2] };
    float u = v3_dot(s, p) * inv;
    if (u < -1.0e-4f || u > 1.0f + 1.0e-4f) {
        return false;
    }
    float q[3];
    v3_cross(s, e1, q);
    float v = v3_dot(d, q) * inv;
    if (v < -1.0e-4f || u + v > 1.0f + 1.0e-4f) {
        return false;
    }
    float t = v3_dot(e2, q) * inv;
    if (!(t > 0.0f)) {
        return false;
    }
    *t_hit = t;
    return true;
}

static bool near3(const float *p, float x, float y, float z, float tol) {
    return fabsf(p[0] - x) <= tol && fabsf(p[1] - y) <= tol && fabsf(p[2] - z) <= tol;
}

bool cube_pick_store_rest(const void *mesh_bytes, const uint16_t *indices) {
    s_ready = false;
    if (mesh_bytes == nullptr || indices == nullptr) {
        return false;
    }
    const uint8_t *bytes = mesh_bytes;
    for (uint32_t i = 0; i < k_verts; ++i) {
        memcpy(s_rest + (size_t)i * 3u, bytes + (size_t)i * 36u, sizeof(float) * 3u);
        memcpy(s_nrm + (size_t)i * 3u, bytes + (size_t)i * 36u + 12u, sizeof(float) * 3u);
    }
    memcpy(s_index, indices, sizeof s_index);
    // +X, corner 0 is (1, -m, m) with normal +X. The face center sample catches a wrong stride.
    float m = 1.0f - k_radius;
    uint32_t mid = grid_vertex(0u, k_grid / 2u, k_grid / 2u);
    if (!near3(s_rest, 1.0f, -m, m, 0.02f) || !near3(s_nrm, 1.0f, 0.0f, 0.0f, 1.0e-3f) || mid >= k_verts ||
        !near3(s_rest + (size_t)mid * 3u, 1.0f, 0.0f, 0.0f, 1.0e-4f)) {
        fprintf(stderr, "[Cube Error] Rest mesh readback does not match the +X face\n");
        return false;
    }
    for (uint32_t i = 0; i < k_indices; ++i) {
        if (s_index[i] >= k_verts) {
            fprintf(stderr, "[Cube Error] Rest index %u is out of range\n", (unsigned)s_index[i]);
            return false;
        }
    }
    s_ready = true;
    return true;
}

bool cube_surface_ready(void) {
    return s_ready;
}

bool cube_surface_hit(uint32_t heat_slot, float yaw_deg, float origin_x, float origin_y,
                      const CameraRay *ray, float *t_hit, float *hx, float *hy, float *hz) {
    if (!s_ready || ray == nullptr || heat_slot >= k_cube_heat_slots) {
        return false;
    }
    cube_heat_shown_disp(heat_slot, s_disp);
    for (uint32_t i = 0; i < k_verts; ++i) {
        float disp[3];
        shown_disp(i, disp);
        float local[3] = {
            s_rest[(size_t)i * 3u] + disp[0],
            s_rest[(size_t)i * 3u + 1u] + disp[1],
            s_rest[(size_t)i * 3u + 2u] + disp[2],
        };
        pose_world(local, yaw_deg, origin_x, origin_y, s_world + (size_t)i * 3u);
    }
    float origin[3] = { ray->ox, ray->oy, ray->oz };
    float dir[3] = { ray->dx, ray->dy, ray->dz };
    float best = 0.0f;
    bool found = false;
    for (uint32_t q = 0; q < k_quads; ++q) {
        const uint16_t *ix = s_index + (size_t)q * 6u;
        const uint32_t tri[2][3] = {
            { ix[0], ix[1], ix[2] },
            { ix[3], ix[4], ix[5] },
        };
        for (int t = 0; t < 2; ++t) {
            float hit_t = 0.0f;
            const float *a = s_world + (size_t)tri[t][0] * 3u;
            const float *b = s_world + (size_t)tri[t][1] * 3u;
            const float *c = s_world + (size_t)tri[t][2] * 3u;
            if (!ray_tri(origin, dir, a, b, c, &hit_t)) {
                continue;
            }
            if (!found || hit_t < best) {
                found = true;
                best = hit_t;
            }
        }
    }
    if (!found) {
        return false;
    }
    if (t_hit != nullptr) {
        *t_hit = best;
    }
    if (hx != nullptr) {
        *hx = ray->ox + ray->dx * best;
    }
    if (hy != nullptr) {
        *hy = ray->oy + ray->dy * best;
    }
    if (hz != nullptr) {
        *hz = ray->oz + ray->dz * best;
    }
    return true;
}

static bool tri_probe(void) {
    float a[3] = { 0, 0, 0 };
    float b[3] = { 1, 0, 0 };
    float c[3] = { 0, 1, 0 };
    float o[3] = { 0.25f, 0.25f, 2.0f };
    float d[3] = { 0, 0, -1 };
    float t = 0.0f;
    if (!ray_tri(o, d, a, b, c, &t) || fabsf(t - 2.0f) > 1.0e-3f) {
        return false;
    }
    float miss[3] = { 2, 2, 2 };
    return !ray_tri(miss, d, a, b, c, &t);
}

static bool rest_same(uint32_t a, uint32_t b) {
    const float *pa = s_rest + (size_t)a * 3u;
    const float *pb = s_rest + (size_t)b * 3u;
    return fabsf(pa[0] - pb[0]) <= 1.0e-3f && fabsf(pa[1] - pb[1]) <= 1.0e-3f && fabsf(pa[2] - pb[2]) <= 1.0e-3f;
}

static bool disp_same(const float a[3], const float b[3]) {
    return fabsf(a[0] - b[0]) <= 1.0e-3f && fabsf(a[1] - b[1]) <= 1.0e-3f && fabsf(a[2] - b[2]) <= 1.0e-3f;
}

static uint32_t edge_fillet_id(uint32_t e, uint32_t row, uint32_t col) {
    return k_face_corners + e * (k_edge_cols * 2u) + row * k_edge_cols + col;
}

static uint32_t corner_sample_id(uint32_t c, uint32_t iu, uint32_t iv) {
    return k_corner_base + c * k_corner_samples + iv * k_edge_cols + iu;
}

// The cap is not index-aliased to the edge. The bake must already put
// them on one quarter circle, and the arc blend must give both the same
// displacement. Distinct face-corner displacements make a swapped rail fail.
static bool cap_weld_agrees(void) {
    for (uint32_t id = 0; id < k_grid_verts; ++id) {
        float *d = s_disp + (size_t)id * 3u;
        d[0] = (float)((id * 3u) % 64u);
        d[1] = (float)((id * 5u) % 64u);
        d[2] = (float)((id * 7u) % 64u);
    }
    for (uint32_t c = 0; c < 8u; ++c) {
        float n0[3];
        float n1[3];
        float n2[3];
        corner_basis(c, n0, n1, n2);
        uint32_t d0 = face_corner_vertex(n0, n1, n2);
        uint32_t d1 = face_corner_vertex(n1, n0, n2);
        uint32_t d2 = face_corner_vertex(n2, n0, n1);
        if (d0 >= k_face_corners || d1 >= k_face_corners || d2 >= k_face_corners ||
            !rest_same(corner_sample_id(c, 0u, 0u), d0) ||
            !rest_same(corner_sample_id(c, k_arc, 0u), d1) ||
            !rest_same(corner_sample_id(c, 0u, k_arc), d2) ||
            !rest_same(corner_sample_id(c, k_arc, k_arc), d2)) {
            fprintf(stderr, "[Cube Error] Corner cap patch %u missed a face corner\n", c);
            return false;
        }
    }
    for (uint32_t e = 0; e < 12u; ++e) {
        for (uint32_t row = 0; row < 2u; ++row) {
            uint32_t len = row == 0u ? 0u : k_grid;
            uint32_t rail0 = face_boundary_vertex(e, len, 0u);
            uint32_t rail1 = face_boundary_vertex(e, len, 1u);
            if (rail0 >= k_face_corners || rail1 >= k_face_corners ||
                !rest_same(edge_fillet_id(e, row, 0u), rail0) ||
                !rest_same(edge_fillet_id(e, row, k_arc), rail1)) {
                fprintf(stderr, "[Cube Error] Corner cap rail %u row %u does not meet the face\n", e, row);
                return false;
            }
            for (uint32_t col = 0; col <= k_arc; ++col) {
                uint32_t edge_id = edge_fillet_id(e, row, col);
                float edge_disp[3];
                shown_disp(edge_id, edge_disp);
                bool met = false;
                bool clash = false;
                for (uint32_t c = 0; c < 8u && !clash; ++c) {
                    for (uint32_t s = 0; s <= k_arc && !clash; ++s) {
                        uint32_t ids[3] = {
                            corner_sample_id(c, s, 0u),
                            corner_sample_id(c, 0u, s),
                            corner_sample_id(c, k_arc, s),
                        };
                        for (int k = 0; k < 3; ++k) {
                            if (!rest_same(edge_id, ids[k])) {
                                continue;
                            }
                            float cap[3];
                            shown_disp(ids[k], cap);
                            if (!disp_same(edge_disp, cap)) {
                                clash = true;
                                break;
                            }
                            met = true;
                        }
                    }
                }
                if (clash || !met) {
                    fprintf(stderr, "[Cube Error] Corner cap left the bevel at edge %u row %u col %u\n", e, row, col);
                    return false;
                }
            }
        }
    }
    memset(s_disp, 0, sizeof s_disp);
    return true;
}

static const float k_half_pi = 1.5707963f;

static bool v3_normalize(float o[3]) {
    float len2 = v3_dot(o, o);
    if (!(len2 > 1.0e-10f)) {
        return false;
    }
    float inv = 1.0f / sqrtf(len2);
    o[0] *= inv;
    o[1] *= inv;
    o[2] *= inv;
    return true;
}

static void v3_sub(const float a[3], const float b[3], float o[3]) {
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}

static void bevel_rest(uint32_t e, uint32_t len, uint32_t arc, float o[3]) {
    float n0[3];
    float n1[3];
    float d[3];
    edge_basis(e, n0, n1, d);
    float m = 1.0f - k_radius;
    float ang = ((float)arc / (float)k_arc) * k_half_pi;
    float ca = cosf(ang);
    float sa = sinf(ang);
    float s = -m + (2.0f * m) * ((float)len / (float)k_grid);
    float nrm[3] = {
        ca * n0[0] + sa * n1[0],
        ca * n0[1] + sa * n1[1],
        ca * n0[2] + sa * n1[2],
    };
    o[0] = (n0[0] + n1[0]) * m + d[0] * s + k_radius * nrm[0];
    o[1] = (n0[1] + n1[1]) * m + d[1] * s + k_radius * nrm[1];
    o[2] = (n0[2] + n1[2]) * m + d[2] * s + k_radius * nrm[2];
}

static void bevel_at(uint32_t e, uint32_t len, uint32_t arc, float o[3]) {
    float rest[3];
    float a[3];
    float b[3];
    float u = (float)arc / (float)k_arc;
    bevel_rest(e, len, arc, rest);
    raw_disp(face_boundary_vertex(e, len, 0u), a);
    raw_disp(face_boundary_vertex(e, len, 1u), b);
    o[0] = rest[0] + a[0] + (b[0] - a[0]) * u;
    o[1] = rest[1] + a[1] + (b[1] - a[1]) * u;
    o[2] = rest[2] + a[2] + (b[2] - a[2]) * u;
}

static bool bevel_geometric_normal(uint32_t e, uint32_t len, uint32_t arc, float o[3]) {
    uint32_t len0 = len > 0u ? len - 1u : len;
    uint32_t len1 = len < k_grid ? len + 1u : len;
    uint32_t arc0 = arc > 0u ? arc - 1u : arc;
    uint32_t arc1 = arc < k_arc ? arc + 1u : arc;
    float p_len1[3];
    float p_len0[3];
    float p_arc1[3];
    float p_arc0[3];
    float t_len[3];
    float t_arc[3];
    bevel_at(e, len1, arc, p_len1);
    bevel_at(e, len0, arc, p_len0);
    bevel_at(e, len, arc1, p_arc1);
    bevel_at(e, len, arc0, p_arc0);
    v3_sub(p_len1, p_len0, t_len);
    v3_sub(p_arc1, p_arc0, t_arc);
    v3_cross(t_arc, t_len, o);
    return v3_normalize(o);
}

static void cap_rest(uint32_t c, uint32_t iu, uint32_t iv, float o[3]) {
    float n0[3];
    float n1[3];
    float n2[3];
    float dir[3];
    float m = 1.0f - k_radius;
    float a = ((float)iu / (float)k_arc) * k_half_pi;
    float b = ((float)iv / (float)k_arc) * k_half_pi;
    corner_basis(c, n0, n1, n2);
    dir[0] = cosf(a) * cosf(b) * n0[0] + sinf(a) * cosf(b) * n1[0] + sinf(b) * n2[0];
    dir[1] = cosf(a) * cosf(b) * n0[1] + sinf(a) * cosf(b) * n1[1] + sinf(b) * n2[1];
    dir[2] = cosf(a) * cosf(b) * n0[2] + sinf(a) * cosf(b) * n1[2] + sinf(b) * n2[2];
    if (!v3_normalize(dir)) {
        v3_set(o, 0.0f, 0.0f, 0.0f);
        return;
    }
    o[0] = (n0[0] + n1[0] + n2[0]) * m + k_radius * dir[0];
    o[1] = (n0[1] + n1[1] + n2[1]) * m + k_radius * dir[1];
    o[2] = (n0[2] + n1[2] + n2[2]) * m + k_radius * dir[2];
}

static void cap_at(uint32_t c, uint32_t iu, uint32_t iv, float o[3]) {
    float n0[3];
    float n1[3];
    float n2[3];
    float d0[3];
    float d1[3];
    float d2[3];
    float rest[3];
    float ua = (float)iu / (float)k_arc;
    float ub = (float)iv / (float)k_arc;
    float along[3];
    float blended[3];
    corner_basis(c, n0, n1, n2);
    raw_disp(face_corner_vertex(n0, n1, n2), d0);
    raw_disp(face_corner_vertex(n1, n0, n2), d1);
    raw_disp(face_corner_vertex(n2, n0, n1), d2);
    v3_lerp(d0, d1, ua, along);
    v3_lerp(along, d2, ub, blended);
    cap_rest(c, iu, iv, rest);
    o[0] = rest[0] + blended[0];
    o[1] = rest[1] + blended[1];
    o[2] = rest[2] + blended[2];
}

static bool cap_geometric_normal(uint32_t c, uint32_t iu, uint32_t iv, float o[3]) {
    uint32_t ivc = iv == k_arc ? k_arc - 1u : iv;
    uint32_t iu0 = iu > 0u ? iu - 1u : iu;
    uint32_t iu1 = iu < k_arc ? iu + 1u : iu;
    uint32_t iv0 = ivc > 0u ? ivc - 1u : ivc;
    uint32_t iv1 = ivc < k_arc ? ivc + 1u : ivc;
    float p_u1[3];
    float p_u0[3];
    float p_v1[3];
    float p_v0[3];
    float t_u[3];
    float t_v[3];
    cap_at(c, iu1, ivc, p_u1);
    cap_at(c, iu0, ivc, p_u0);
    cap_at(c, iu, iv1, p_v1);
    cap_at(c, iu, iv0, p_v0);
    v3_sub(p_u1, p_u0, t_u);
    v3_sub(p_v1, p_v0, t_v);
    v3_cross(t_u, t_v, o);
    return v3_normalize(o);
}

static bool face_shaded_normal(uint32_t face, uint32_t i, uint32_t j, float o[3]) {
    float n[3];
    float t[3];
    float b[3];
    float hi[3];
    float lo[3];
    float diff[3];
    uint32_t i0 = i > 0u ? i - 1u : i;
    uint32_t i1 = i < k_grid ? i + 1u : i;
    uint32_t j0 = j > 0u ? j - 1u : j;
    uint32_t j1 = j < k_grid ? j + 1u : j;
    float step_u = (float)(i1 - i0) * (2.0f * (1.0f - k_radius) / (float)k_grid);
    float step_v = (float)(j1 - j0) * (2.0f * (1.0f - k_radius) / (float)k_grid);
    float du;
    float dv;
    face_basis(face, n, t, b);
    raw_disp(grid_vertex(face, i1, j), hi);
    raw_disp(grid_vertex(face, i0, j), lo);
    v3_sub(hi, lo, diff);
    du = v3_dot(diff, n);
    raw_disp(grid_vertex(face, i, j1), hi);
    raw_disp(grid_vertex(face, i, j0), lo);
    v3_sub(hi, lo, diff);
    dv = v3_dot(diff, n);
    if (step_u < 1.0e-4f) {
        step_u = 1.0e-4f;
    }
    if (step_v < 1.0e-4f) {
        step_v = 1.0e-4f;
    }
    o[0] = n[0] - t[0] * (du / step_u) - b[0] * (dv / step_v);
    o[1] = n[1] - t[1] * (du / step_u) - b[1] * (dv / step_v);
    o[2] = n[2] - t[2] * (du / step_u) - b[2] * (dv / step_v);
    return v3_normalize(o);
}

static bool nrm_matches(uint32_t id, const float n[3]) {
    return v3_dot(n, s_nrm + (size_t)id * 3u) > 0.999f;
}

static uint32_t seam_id(uint32_t e, uint32_t len, uint32_t arc) {
    return k_grid_verts + (e * (k_grid - 1u) + (len - 1u)) * (k_arc - 1u) + (arc - 1u);
}

// Zero displacement must reproduce the baked normal. A pushed rail must tilt it.
static bool shaded_normal_agrees(void) {
    float n[3];
    memset(s_disp, 0, sizeof s_disp);
    for (uint32_t face = 0; face < 6u; ++face) {
        for (uint32_t j = 0; j <= k_grid; ++j) {
            for (uint32_t i = 0; i <= k_grid; ++i) {
                uint32_t id = grid_vertex(face, i, j);
                if (!face_shaded_normal(face, i, j, n) || !nrm_matches(id, n)) {
                    fprintf(stderr, "[Cube Error] Face normal left the baked surface at %u\n", id);
                    return false;
                }
            }
        }
    }
    for (uint32_t id = k_grid_verts; id < k_verts; ++id) {
        uint32_t e = 0;
        uint32_t len = 0;
        uint32_t arc = 0;
        seam_decode(id, &e, &len, &arc);
        if (!bevel_geometric_normal(e, len, arc, n) || !nrm_matches(id, n)) {
            fprintf(stderr, "[Cube Error] Bevel normal left the baked surface at %u\n", id);
            return false;
        }
    }
    for (uint32_t e = 0; e < 12u; ++e) {
        for (uint32_t row = 0; row < 2u; ++row) {
            uint32_t len = row == 0u ? 0u : k_grid;
            for (uint32_t col = 1; col < k_arc; ++col) {
                uint32_t id = edge_fillet_id(e, row, col);
                if (!bevel_geometric_normal(e, len, col, n) || !nrm_matches(id, n)) {
                    fprintf(stderr, "[Cube Error] Bevel normal left the baked surface at %u\n", id);
                    return false;
                }
            }
        }
    }
    for (uint32_t c = 0; c < 8u; ++c) {
        for (uint32_t iv = 1; iv < k_arc; ++iv) {
            for (uint32_t iu = 1; iu < k_arc; ++iu) {
                uint32_t id = corner_sample_id(c, iu, iv);
                if (!cap_geometric_normal(c, iu, iv, n) || !nrm_matches(id, n)) {
                    fprintf(stderr, "[Cube Error] Cap normal left the baked surface at %u\n", id);
                    return false;
                }
            }
        }
    }
    uint32_t rail = face_boundary_vertex(0u, 8u, 0u);
    uint32_t sid = seam_id(0u, 8u, 4u);
    s_disp[(size_t)rail * 3u] = 0.30f;
    bool seam_tilted = bevel_geometric_normal(0u, 8u, 4u, n) && v3_dot(n, s_nrm + (size_t)sid * 3u) < 0.99f;
    memset(s_disp, 0, sizeof s_disp);
    if (!seam_tilted) {
        fprintf(stderr, "[Cube Error] Bevel normal ignored a dent\n");
        return false;
    }
    float cn0[3];
    float cn1[3];
    float cn2[3];
    corner_basis(0u, cn0, cn1, cn2);
    uint32_t corner = face_corner_vertex(cn0, cn1, cn2);
    uint32_t cap_id = corner_sample_id(0u, 4u, 4u);
    s_disp[(size_t)corner * 3u] = 0.30f;
    bool cap_tilted = cap_geometric_normal(0u, 4u, 4u, n) && v3_dot(n, s_nrm + (size_t)cap_id * 3u) < 0.99f;
    memset(s_disp, 0, sizeof s_disp);
    if (!cap_tilted) {
        fprintf(stderr, "[Cube Error] Cap normal ignored a dent\n");
        return false;
    }
    uint32_t above = grid_vertex(0u, 8u, 9u);
    uint32_t face_id = grid_vertex(0u, 8u, 8u);
    s_disp[(size_t)above * 3u] = 0.25f;
    bool face_tilted = face_shaded_normal(0u, 8u, 8u, n) && v3_dot(n, s_nrm + (size_t)face_id * 3u) < 0.99f;
    memset(s_disp, 0, sizeof s_disp);
    if (!face_tilted) {
        fprintf(stderr, "[Cube Error] Face normal ignored a dent\n");
        return false;
    }
    return true;
}

bool cube_surface_self_check(void) {
    if (!s_ready) {
        return true;
    }
    if (!tri_probe()) {
        fprintf(stderr, "[Cube Error] Triangle pick probe failed\n");
        return false;
    }
    if (!cap_weld_agrees()) {
        return false;
    }
    if (!shaded_normal_agrees()) {
        return false;
    }
    // Through the interior, and one that clears the rotated 120 px fillet.
    CameraRay mid = {
        .ox = 3.0f,
        .oy = 5.0f,
        .oz = 65536.0f,
        .dx = 0.0f,
        .dy = 0.0f,
        .dz = -1.0f,
    };
    CameraRay far = {
        .ox = 360.0f,
        .oy = 0.0f,
        .oz = 65536.0f,
        .dx = 0.0f,
        .dy = 0.0f,
        .dz = -1.0f,
    };
    float t = 0.0f;
    float hx = 0.0f;
    float hy = 0.0f;
    float hz = 0.0f;
    bool hit = cube_surface_hit(0u, 38.0f, 0.0f, 0.0f, &mid, &t, &hx, &hy, &hz);
    float far_t = 0.0f;
    bool miss = cube_surface_hit(0u, 38.0f, 0.0f, 0.0f, &far, &far_t, &hx, &hy, &hz);
    if (!hit || !(t > 1000.0f) || miss) {
        fprintf(stderr, "[Cube Error] Rest-pose pick missed the center ray or hit x=360\n");
        return false;
    }
    return true;
}
