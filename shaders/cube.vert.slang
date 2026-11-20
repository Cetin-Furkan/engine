#include "camera.slang"
#include "material.slang"
#include "face_grid.slang"

struct CubeXform {
    float4 world_r0;
    float4 world_r1;
    float4 world_r2;
    uint   entity_id;
    uint   pad0;
    uint   pad1;
    uint   pad2;
};

struct DrawPush {
    uint64_t verts;
    uint64_t heat;
    uint64_t xform;
    uint64_t grid;
    uint     width;
    uint     height;
    uint     grid_n;
    float    depth_bias;
    float    phase;
    uint     entity_id;
    uint     pad0;
    uint     pad1;
    ViewPush view;
};
[[vk::push_constant]] DrawPush screen;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit : TEXCOORD2;
    [[vk::location(3)]] float  v_view_z : TEXCOORD3;
    [[vk::location(4)]] nointerpolation uint64_t v_env_bda    : TEXCOORD4;
    [[vk::location(5)]] nointerpolation uint     v_cluster_id : TEXCOORD5;
};

static const float k_edge_px = 240.0;
static const float k_half_pi = 1.5707963;

float3 bevel_rest(uint e, uint len, uint arc) {
    float3 n0;
    float3 n1;
    float3 d;
    edge_basis(e, n0, n1, d);
    float m = 1.0 - k_radius;
    float u = float(arc) / float(k_arc);
    float ang = u * k_half_pi;
    float3 nrm = cos(ang) * n0 + sin(ang) * n1;
    float s = lerp(-m, m, float(len) / float(k_grid));
    return (n0 + n1) * m + d * s + k_radius * nrm;
}

float3 bevel_at(SoftVertexState* soft, uint e, uint len, uint arc) {
    float u = float(arc) / float(k_arc);
    float3 a = soft[face_boundary_vertex(e, len, 0u)].disp;
    float3 b = soft[face_boundary_vertex(e, len, 1u)].disp;
    return bevel_rest(e, len, arc) + lerp(a, b, u);
}

float3 bevel_geometric_normal(SoftVertexState* soft, uint e, uint len, uint arc, float3 baked) {
    uint len0 = len > 0u ? len - 1u : len;
    uint len1 = len < k_grid ? len + 1u : len;
    uint arc0 = arc > 0u ? arc - 1u : arc;
    uint arc1 = arc < k_arc ? arc + 1u : arc;
    float3 t_len = bevel_at(soft, e, len1, arc) - bevel_at(soft, e, len0, arc);
    float3 t_arc = bevel_at(soft, e, len, arc1) - bevel_at(soft, e, len, arc0);
    float3 n = cross(t_arc, t_len);
    if (dot(n, n) < 1e-10) {
        return baked;
    }
    return normalize(n);
}

float3 cap_rest(uint c, uint iu, uint iv) {
    float3 n0;
    float3 n1;
    float3 n2;
    corner_basis(c, n0, n1, n2);
    float a = float(iu) / float(k_arc) * k_half_pi;
    float b = float(iv) / float(k_arc) * k_half_pi;
    float3 dir = normalize(cos(a) * cos(b) * n0 + sin(a) * cos(b) * n1 + sin(b) * n2);
    float m = 1.0 - k_radius;
    return (n0 + n1 + n2) * m + k_radius * dir;
}

float3 cap_at(SoftVertexState* soft, uint c, uint iu, uint iv) {
    float3 n0;
    float3 n1;
    float3 n2;
    corner_basis(c, n0, n1, n2);
    float3 d0 = soft[face_corner_vertex(n0, n1, n2)].disp;
    float3 d1 = soft[face_corner_vertex(n1, n0, n2)].disp;
    float3 d2 = soft[face_corner_vertex(n2, n0, n1)].disp;
    float ua = float(iu) / float(k_arc);
    float ub = float(iv) / float(k_arc);
    return cap_rest(c, iu, iv) + lerp(lerp(d0, d1, ua), d2, ub);
}

float3 cap_geometric_normal(SoftVertexState* soft, uint c, uint iu, uint iv, float3 baked) {
    // iv = k_arc is one collapsed face corner. Shade it with the ring below.
    uint ivc = iv == k_arc ? k_arc - 1u : iv;
    uint iu0 = iu > 0u ? iu - 1u : iu;
    uint iu1 = iu < k_arc ? iu + 1u : iu;
    uint iv0 = ivc > 0u ? ivc - 1u : ivc;
    uint iv1 = ivc < k_arc ? ivc + 1u : ivc;
    float3 t_u = cap_at(soft, c, iu1, ivc) - cap_at(soft, c, iu0, ivc);
    float3 t_v = cap_at(soft, c, iu, iv1) - cap_at(soft, c, iu, iv0);
    float3 n = cross(t_u, t_v);
    if (dot(n, n) < 1e-10) {
        return baked;
    }
    return normalize(n);
}

float3 face_shaded_normal(SoftVertexState* soft, uint face, uint i, uint j) {
    float3 n;
    float3 t;
    float3 b;
    face_basis(face, n, t, b);
    uint i0 = i > 0u ? i - 1u : i;
    uint i1 = i < k_grid ? i + 1u : i;
    uint j0 = j > 0u ? j - 1u : j;
    uint j1 = j < k_grid ? j + 1u : j;
    float du = dot(soft[grid_vertex(face, i1, j)].disp - soft[grid_vertex(face, i0, j)].disp, n);
    float dv = dot(soft[grid_vertex(face, i, j1)].disp - soft[grid_vertex(face, i, j0)].disp, n);
    float step_u = float(i1 - i0) * (2.0 * (1.0 - k_radius) / float(k_grid));
    float step_v = float(j1 - j0) * (2.0 * (1.0 - k_radius) / float(k_grid));
    return normalize(n - t * (du / max(step_u, 1e-4)) - b * (dv / max(step_v, 1e-4)));
}

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    MeshVertex mv = ((MeshVertex*)screen.verts)[vert_id];
    SoftVertexState* soft = (SoftVertexState*)screen.heat;
    SoftVertexState sv = soft[vert_id];

    if (vert_id >= k_grid_verts) {
        // The bevel interior is not simulated. It follows the two face
        // rails this frame, so a side dent stays one surface.
        uint e;
        uint len;
        uint arc;
        seam_decode(vert_id, e, len, arc);
        float u = float(arc) / float(k_arc);
        SoftVertexState a = soft[face_boundary_vertex(e, len, 0u)];
        SoftVertexState b = soft[face_boundary_vertex(e, len, 1u)];
        sv.disp = lerp(a.disp, b.disp, u);
        sv.temp = lerp(a.temp, b.temp, u);
    } else if (vert_id >= k_face_corners && vert_id < k_corner_base) {
        // Fillet rows at the two ends of the edge. The span between them
        // is the seam above. u is the same arc fraction, so the last
        // step of the bevel meets the cap.
        uint local = vert_id - k_face_corners;
        uint span = k_edge_cols * 2u;
        uint e = local / span;
        uint rem = local - e * span;
        uint row = rem / k_edge_cols;
        uint col = rem - row * k_edge_cols;
        uint len = row == 0u ? 0u : k_grid;
        float u = float(col) / float(k_arc);
        SoftVertexState a = soft[face_boundary_vertex(e, len, 0u)];
        SoftVertexState b = soft[face_boundary_vertex(e, len, 1u)];
        sv.disp = lerp(a.disp, b.disp, u);
        sv.temp = lerp(a.temp, b.temp, u);
    } else if (vert_id >= k_corner_base && vert_id < k_fillet_verts) {
        // Spherical cap. ua and ub are arc fractions, matching the bevel
        // lerp, so each side of the patch lands on the quarter circle of
        // the edge that ends here. The iv = k_arc row is one face corner.
        uint local = vert_id - k_corner_base;
        uint c = local / k_corner_samples;
        uint rem = local - c * k_corner_samples;
        uint iv = rem / k_edge_cols;
        uint iu = rem - iv * k_edge_cols;
        float3 n0;
        float3 n1;
        float3 n2;
        corner_basis(c, n0, n1, n2);
        SoftVertexState s0 = soft[face_corner_vertex(n0, n1, n2)];
        SoftVertexState s1 = soft[face_corner_vertex(n1, n0, n2)];
        SoftVertexState s2 = soft[face_corner_vertex(n2, n0, n1)];
        float ua = float(iu) / float(k_arc);
        float ub = float(iv) / float(k_arc);
        sv.disp = lerp(lerp(s0.disp, s1.disp, ua), s2.disp, ub);
        sv.temp = lerp(lerp(s0.temp, s1.temp, ua), s2.temp, ub);
    } else if (vert_id < k_fillet_verts) {
        // Fillet copies that sit on a face corner share that corner's
        // motion. The face grid already aliases the canonical id.
        uint face;
        uint i;
        uint j;
        if (face_corner_at(mv.pos, face, i, j)) {
            uint canon = grid_vertex(face, i, j);
            if (canon != vert_id) {
                SoftVertexState corner = soft[canon];
                sv.disp = corner.disp;
                sv.temp = corner.temp;
            }
        }
    }

    float3 in_pos = mv.pos + sv.disp;
    float3 in_nrm = mv.nrm;
    float3 in_albedo = mv.albedo;

    // The flat face, the bevel, and the cap all tilt with the displacement
    // that was just chosen, so a dent changes the light on the round edge.
    if (vert_id >= k_grid_verts) {
        uint e;
        uint len;
        uint arc;
        seam_decode(vert_id, e, len, arc);
        in_nrm = bevel_geometric_normal(soft, e, len, arc, in_nrm);
    } else if (vert_id >= k_face_corners && vert_id < k_corner_base) {
        uint local = vert_id - k_face_corners;
        uint span = k_edge_cols * 2u;
        uint e = local / span;
        uint rem = local - e * span;
        uint row = rem / k_edge_cols;
        uint col = rem - row * k_edge_cols;
        if (col > 0u && col < k_arc) {
            uint len = row == 0u ? 0u : k_grid;
            in_nrm = bevel_geometric_normal(soft, e, len, col, in_nrm);
        }
    } else if (vert_id >= k_corner_base && vert_id < k_fillet_verts) {
        uint local = vert_id - k_corner_base;
        uint c = local / k_corner_samples;
        uint rem = local - c * k_corner_samples;
        uint iv = rem / k_edge_cols;
        uint iu = rem - iv * k_edge_cols;
        in_nrm = cap_geometric_normal(soft, c, iu, iv, in_nrm);
    } else if (vert_id >= k_fillet_verts && vert_id < k_grid_verts) {
        uint face;
        uint i;
        uint j;
        grid_decode(vert_id, face, i, j);
        in_nrm = face_shaded_normal(soft, face, i, j);
    } else if (vert_id < k_face_corners) {
        uint face;
        uint i;
        uint j;
        if (face_corner_at(mv.pos, face, i, j)) {
            in_nrm = face_shaded_normal(soft, face, i, j);
        }
    }

    CubeXform xf = ((CubeXform *)screen.xform)[0];
    float3 local_px = in_pos * (k_edge_px * 0.5);
    float3 p = float3(
        dot(xf.world_r0.xyz, local_px) + xf.world_r0.w,
        dot(xf.world_r1.xyz, local_px) + xf.world_r1.w,
        dot(xf.world_r2.xyz, local_px) + xf.world_r2.w);

    float3 c0 = cross(xf.world_r1.xyz, xf.world_r2.xyz);
    float3 c1 = cross(xf.world_r2.xyz, xf.world_r0.xyz);
    float3 c2 = cross(xf.world_r0.xyz, xf.world_r1.xyz);
    float  ds = dot(xf.world_r0.xyz, c0) < 0.0 ? -1.0 : 1.0;
    float3 wn = float3(dot(c0, in_nrm), dot(c1, in_nrm), dot(c2, in_nrm)) * ds;
    float  wl = dot(wn, wn);
    float3 posed_nrm = wl > 1.0e-12 ? wn * rsqrt(wl) : float3(0.0, 0.0, 1.0);

    float3 view = view_of(p, screen.view.q, screen.view.cam);
    VSOutput out;
    out.sv_pos       = view_ndc_persp(view, screen.view.zoom, screen.view.persp, screen.width, screen.height, screen.depth_bias);
    out.v_nrm        = view_rotate(screen.view.q, posed_nrm);
    out.v_view_z     = view.z;
    out.v_env_bda    = screen.view.materials;
    out.v_cluster_id = screen.entity_id != 0u ? screen.entity_id : 1u;

    float3 axis = float3(0.57735026919);
    float ang = 6.28318530718 * (screen.phase + dot(in_pos, float3(0.10, 0.14, 0.08)));
    float c = cos(ang);
    float s = sin(ang);
    float3 spun = in_albedo * c + cross(axis, in_albedo) * s + axis * dot(axis, in_albedo) * (1.0 - c);
    float3 graded = clamp(spun, 0.0, 1.0);
    float3 cool = graded;

    if (screen.grid != 0ul && screen.grid_n > 1u) {
        uint  n    = screen.grid_n;
        float edge = float(n - 1u);
        float3 gp  = clamp(in_pos * 0.5 + float3(0.5), float3(0.0), float3(1.0)) * edge;
        uint3  p0  = min(uint3(floor(gp)), uint3(n - 1u));
        uint3  p1  = min(p0 + uint3(1u),   uint3(n - 1u));
        float3 fr  = gp - float3(p0);
        uint   nn  = n * n;
        half*  fv  = (half*)screen.grid;

        float c000 = float(fv[p0.x + p0.y * n + p0.z * nn]);
        float c100 = float(fv[p1.x + p0.y * n + p0.z * nn]);
        float c010 = float(fv[p0.x + p1.y * n + p0.z * nn]);
        float c110 = float(fv[p1.x + p1.y * n + p0.z * nn]);
        float c001 = float(fv[p0.x + p0.y * n + p1.z * nn]);
        float c101 = float(fv[p1.x + p0.y * n + p1.z * nn]);
        float c011 = float(fv[p0.x + p1.y * n + p1.z * nn]);
        float c111 = float(fv[p1.x + p1.y * n + p1.z * nn]);

        float c00  = lerp(c000, c100, fr.x);
        float c10  = lerp(c010, c110, fr.x);
        float c01  = lerp(c001, c101, fr.x);
        float c11  = lerp(c011, c111, fr.x);
        float cell = clamp(lerp(lerp(c00, c10, fr.y), lerp(c01, c11, fr.y), fr.z), 0.0, 1.0);
        cool = lerp(graded, float3(0.45, 0.72, 1.0), cell);
    }

    out.v_albedo = cool;
    out.v_emit   = material_emit(screen.view.materials, screen.view.mat_index);

    return out;
}
