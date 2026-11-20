// Shared Slang 2026 Unified Scene Meshlet, Subgroup Hi-Z, and Visibility Buffer Math
// Matches include/scene/scene.h and src/scene/gpu/scene_gpu.c byte-for-byte.

#include "camera.slang"
#include "material.slang"

static const uint k_scene_max_draws      = 16384u;
static const uint k_scene_max_blend      = 4096u;
static const uint k_scene_max_instances  = 512u;
static const uint k_scene_hiz_max_mips   = 12u;

static const uint k_scene_flag_two_sided    = 1u << 0u;
static const uint k_scene_flag_alpha_mask   = 1u << 1u;
static const uint k_scene_flag_alpha_blend  = 1u << 2u;
static const uint k_scene_flag_pbr          = 1u << 3u;
static const uint k_scene_flag_flip_winding = 1u << 4u;

static const uint k_vert_flag_tan_neg_sign = 1u << 0u;
static const uint k_vert_flag_has_tangent  = 1u << 1u;
static const uint k_vert_flag_has_uv       = 1u << 2u;

// 32-byte quantized PBR vertex (matches ScenePackedVertex in include/scene/scene.h).
struct ScenePackedVertex {
    uint16_t px;
    uint16_t py;
    uint16_t pz;
    uint16_t tan_flags;
    int16_t  oct_nx;
    int16_t  oct_ny;
    int16_t  oct_tx;
    int16_t  oct_ty;
    float    u;
    float    v;
    uint     rgba8;
    uint     pad0;
};

struct SceneMeshlet {
    float3 origin;
    uint   vert_offset;
    float3 scale;
    uint   tri_offset;
    float3 center;
    float  radius;
    int16_t cone_axis_oct_x;
    int16_t cone_axis_oct_y;
    int16_t cone_cutoff_s16;
    uint8_t vert_count;
    uint8_t tri_count;
    uint   mesh_id;
    uint   flags;
};

struct SceneInstance {
    float4 world_r0;
    float4 world_r1;
    float4 world_r2;
    float  squash;
    float  roughness;
    float  depth_bias;
    float  bound_scale;
    uint   mat_index;
    uint   mesh_id;
    uint   first_meshlet;
    uint   meshlet_count;
    uint   flags;
    uint   entity_id;
    uint   pbr_mat_id;
    uint   pad1;
};

struct SceneHizMipInfo {
    uint offset_floats;
    uint width;
    uint height;
    uint pad;
};

struct SceneHizHeader {
    uint   full_width;
    uint   full_height;
    uint   mip_count;
    uint   valid;
    float  prev_qx, prev_qy, prev_qz, prev_qw;
    float  prev_cam_x, prev_cam_y, prev_cam_z, prev_zoom;
    uint   pad0, pad1, pad2, pad3;
    SceneHizMipInfo mips[12];
};

struct SceneDrawIndirectCmd {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

// 112-byte unified draw push block (matches SceneDrawPush in src/scene/gpu/scene_gpu.c).
struct SceneDrawPush {
    uint64_t meshlets_bda;
    uint64_t vertices_bda;
    uint64_t triangles_bda;
    uint64_t cull_slot_bda;
    uint64_t pbr_mats_bda;
    uint     width;
    uint     height;
    uint     phase_cmd_mode; // 0 = firstInstance pair, 1 = p1_cmds, 2 = p2_cmds, 3 = blend_pairs
    uint     draw_flags;     // bit 0: cull_none active (software backface check for single-sided)
    ViewPush view;
};

// Byte offsets inside SceneCullSlot (must match static_asserts in src/scene/gpu/scene_gpu.c):
//   0:      header counters (16 uints = 64B)
//   64:     single fallback cmds (64B)
//   128:    phase1_cmds[16384] (262144B)
//   262272: phase2_cmds[16384] (262144B)
//   524416: rejected_pairs[16384] (65536B)
//   589952: work_pairs[16384] (65536B)
//   655488: blend_pairs[4096] (16384B)
//   671872: instances[512] (49152B)
static const uint64_t k_slot_off_single1   = 64ull;
static const uint64_t k_slot_off_single2   = 80ull;
static const uint64_t k_slot_off_cmds1     = 128ull;
static const uint64_t k_slot_off_cmds2     = 262272ull;
static const uint64_t k_slot_off_rejected  = 524416ull;
static const uint64_t k_slot_off_work      = 589952ull;
static const uint64_t k_slot_off_blend     = 655488ull;
static const uint64_t k_slot_off_instances = 671872ull;

float2 scene_sign_not_zero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float3 scene_oct_decode(int16_t ox, int16_t oy) {
    float2 e = clamp(float2(float(ox), float(oy)) * (1.0 / 32767.0), float2(-1.0), float2(1.0));
    float3 v = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        v.xy = (float2(1.0) - abs(v.yx)) * scene_sign_not_zero(v.xy);
    }
    return normalize(v);
}

float3 scene_dequantize_pos(ScenePackedVertex pv, SceneMeshlet ml) {
    float3 q = float3(float(pv.px), float(pv.py), float(pv.pz)) * (1.0 / 65535.0);
    return ml.origin + ml.scale * q;
}

float4 scene_unpack_rgba8(uint packed) {
    return float4(
        float(packed & 0xFFu),
        float((packed >> 8u) & 0xFFu),
        float((packed >> 16u) & 0xFFu),
        float((packed >> 24u) & 0xFFu)
    ) * (1.0 / 255.0);
}

float3 scene_inv_view_rotate(float4 q, float3 v) {
    return view_rotate(float4(-q.xyz, q.w), v);
}

float3 scene_transform_point_squash(float3 local_p, SceneInstance inst) {
    float sz  = clamp(1.0 + inst.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);
    float3 off = float3(
        dot(inst.world_r0.xyz, local_p) * sxy,
        dot(inst.world_r1.xyz, local_p) * sxy,
        dot(inst.world_r2.xyz, local_p) * sz);
    return off + float3(inst.world_r0.w, inst.world_r1.w, inst.world_r2.w);
}

float3 scene_transform_dir_squash(float3 local_d, SceneInstance inst) {
    float sz  = clamp(1.0 + inst.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);
    float3 wd = float3(
        dot(inst.world_r0.xyz, local_d) * sxy,
        dot(inst.world_r1.xyz, local_d) * sxy,
        dot(inst.world_r2.xyz, local_d) * sz);
    float l2 = dot(wd, wd);
    return l2 > 1.0e-12 ? wd * rsqrt(l2) : float3(1.0, 0.0, 0.0);
}

float3 scene_transform_normal_squash(float3 local_n, SceneInstance inst) {
    float sz  = clamp(1.0 + inst.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);
    float3 a0 = inst.world_r0.xyz * sxy;
    float3 a1 = inst.world_r1.xyz * sxy;
    float3 a2 = inst.world_r2.xyz * sz;
    float3 c0 = cross(a1, a2);
    float3 c1 = cross(a2, a0);
    float3 c2 = cross(a0, a1);
    float  ds = dot(a0, c0) < 0.0 ? -1.0 : 1.0;
    float3 wn = float3(dot(c0, local_n), dot(c1, local_n), dot(c2, local_n)) * ds;
    float  l2 = dot(wn, wn);
    return l2 > 1.0e-12 ? wn * rsqrt(l2) : float3(0.0, 0.0, 1.0);
}

struct SceneVertXform {
    float4 clip;
    float3 view_pos;
    float3 view_nrm;
    float3 view_tan;
    float  bitan_sign;
    bool   has_tan;
    float2 uv;
    float4 rgba;
    float3 emit;
    float  roughness;
};

SceneVertXform scene_transform_vertex_pbr(
    ScenePackedVertex pv,
    SceneMeshlet      ml,
    SceneInstance     inst,
    ViewPush          view,
    uint              width,
    uint              height)
{
    SceneVertXform r;
    float3 local_p = scene_dequantize_pos(pv, ml);
    float3 local_n = scene_oct_decode(pv.oct_nx, pv.oct_ny);
    float3 local_t = scene_oct_decode(pv.oct_tx, pv.oct_ty);
    float4 rgba    = scene_unpack_rgba8(pv.rgba8);

    float3 world_p = scene_transform_point_squash(local_p, inst);
    float3 world_n = scene_transform_normal_squash(local_n, inst);
    float3 world_t = scene_transform_dir_squash(local_t, inst);
    float3 vpos    = view_of(world_p, view.q, view.cam);

    uint tf = uint(pv.tan_flags);
    float tsign = ((tf & k_vert_flag_tan_neg_sign) != 0u) ? -1.0 : 1.0;
    if ((inst.flags & k_scene_flag_flip_winding) != 0u) {
        tsign = -tsign;
    }

    r.clip       = view_ndc_persp(vpos, view.zoom, view.persp, width, height, inst.depth_bias);
    r.view_pos   = vpos;
    r.view_nrm   = view_rotate(view.q, world_n);
    r.view_tan   = view_rotate(view.q, world_t);
    r.bitan_sign = tsign;
    r.has_tan    = (tf & k_vert_flag_has_tangent) != 0u;
    r.uv         = float2(pv.u, pv.v);
    r.rgba       = rgba;
    r.emit       = material_emit(view.materials, inst.mat_index);
    r.roughness  = inst.roughness > 0.0 ? inst.roughness : rgba.a;
    return r;
}

// 11:14:7 packing: ((inst_id + 1) : 11) | (meshlet_id : 14) | (prim_id : 7)
uint scene_visbuf_pack_id(uint inst_id, uint meshlet_id, uint prim_id) {
    return (((inst_id + 1u) & 0x7FFu) << 21u) | ((meshlet_id & 0x3FFFu) << 7u) | (prim_id & 0x7Fu);
}

bool scene_visbuf_unpack_id(uint vis_id, out uint inst_id, out uint meshlet_id, out uint prim_id) {
    uint inst_p1 = (vis_id >> 21u) & 0x7FFu;
    if (inst_p1 == 0u) {
        inst_id    = 0u;
        meshlet_id = 0u;
        prim_id    = 0u;
        return false;
    }
    inst_id    = inst_p1 - 1u;
    meshlet_id = (vis_id >> 7u) & 0x3FFFu;
    prim_id    = vis_id & 0x7Fu;
    return true;
}

struct SceneBaryResult {
    float3 lambda;
    float3 dlambda_dx;
    float3 dlambda_dy;
    float  z_ndc;
    bool   front_face;
    bool   valid;
};

SceneBaryResult scene_eval_barycentrics(
    float4 c0,
    float4 c1,
    float4 c2,
    float2 px_center,
    float2 viewport)
{
    SceneBaryResult r;
    r.lambda     = float3(1.0, 0.0, 0.0);
    r.dlambda_dx = float3(0.0);
    r.dlambda_dy = float3(0.0);
    r.z_ndc      = 0.0;
    r.front_face = true;
    r.valid      = false;

    float iw0 = 1.0 / c0.w;
    float iw1 = 1.0 / c1.w;
    float iw2 = 1.0 / c2.w;

    float2 s0 = c0.xy * iw0;
    float2 s1 = c1.xy * iw1;
    float2 s2 = c2.xy * iw2;

    float2 p_ndc = (2.0 * px_center / max(viewport, float2(1.0))) - float2(1.0);
    float2 e1    = s1 - s0;
    float2 e2    = s2 - s0;
    float  det   = e1.x * e2.y - e1.y * e2.x;
    if (abs(det) <= 1.0e-12) {
        return r;
    }

    float  inv_det = 1.0 / det;
    float2 d       = p_ndc - s0;
    float  u_scr   = (d.x * e2.y - d.y * e2.x) * inv_det;
    float  v_scr   = (e1.x * d.y - e1.y * d.x) * inv_det;
    float  w_scr   = 1.0 - u_scr - v_scr;

    float du_dx = ( 2.0 * e2.y / max(viewport.x, 1.0)) * inv_det;
    float du_dy = (-2.0 * e2.x / max(viewport.y, 1.0)) * inv_det;
    float dv_dx = (-2.0 * e1.y / max(viewport.x, 1.0)) * inv_det;
    float dv_dy = ( 2.0 * e1.x / max(viewport.y, 1.0)) * inv_det;
    float dw_dx = -(du_dx + dv_dx);
    float dw_dy = -(du_dy + dv_dy);

    float3 n      = float3(w_scr * iw0, u_scr * iw1, v_scr * iw2);
    float  z_inv  = n.x + n.y + n.z;
    if (abs(z_inv) <= 1.0e-12) {
        return r;
    }
    float  z_persp = 1.0 / z_inv;
    r.lambda       = n * z_persp;

    float3 dn_dx   = float3(dw_dx * iw0, du_dx * iw1, dv_dx * iw2);
    float3 dn_dy   = float3(dw_dy * iw0, du_dy * iw1, dv_dy * iw2);
    float  dzi_dx  = dn_dx.x + dn_dx.y + dn_dx.z;
    float  dzi_dy  = dn_dy.x + dn_dy.y + dn_dy.z;

    r.dlambda_dx   = (dn_dx - r.lambda * dzi_dx) * z_persp;
    r.dlambda_dy   = (dn_dy - r.lambda * dzi_dy) * z_persp;
    r.z_ndc        = w_scr * (c0.z * iw0) + u_scr * (c1.z * iw1) + v_scr * (c2.z * iw2);
    r.front_face   = (det < 0.0);
    r.valid        = true;
    return r;
}
