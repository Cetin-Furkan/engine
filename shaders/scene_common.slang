// Shared Slang 2026 Unified Scene Meshlet, Subgroup Hi-Z, and Visibility Buffer Math
// Matches include/scene/scene.h byte-for-byte.

#include "camera.slang"
#include "material.slang"

static const uint k_scene_max_draws     = 1024u;
static const uint k_scene_max_instances = 256u;
static const uint k_scene_hiz_max_mips  = 12u;
static const uint k_scene_flag_two_sided = 1u;

struct ScenePackedVertex {
    uint16_t px;
    uint16_t py;
    uint16_t pz;
    uint16_t pad_u16;
    int16_t  oct_nx;
    int16_t  oct_ny;
    uint     rgba8;
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
    float origin_x;
    float origin_y;
    float origin_z;
    float uniform_scale;
    float pitch_rad;
    float yaw_rad;
    float squash;
    float roughness;
    float selected;
    float depth_bias;
    uint  mat_index;
    uint  mesh_id;
    uint  first_meshlet;
    uint  meshlet_count;
    uint  flags;
    uint  pad0;
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

// Byte offsets inside SceneCullSlot (must match SceneCullSlot in src/scene/gpu/scene_gpu.c):
//   0:     header counters (16 uints = 64B)
//   64:    single fallback cmds (64B)
//   128:   phase1_cmds[1024] (16384B)
//   16512: phase2_cmds[1024] (16384B)
//   32896: rejected_pairs[1024] (4096B)
//   36992: work_pairs[1024] (4096B)
//   41088: instances[256] (16384B)
static const uint64_t k_slot_off_single1   = 64ull;
static const uint64_t k_slot_off_single2   = 80ull;
static const uint64_t k_slot_off_cmds1     = 128ull;
static const uint64_t k_slot_off_cmds2     = 16512ull;
static const uint64_t k_slot_off_rejected  = 32896ull;
static const uint64_t k_slot_off_work      = 36992ull;
static const uint64_t k_slot_off_instances = 41088ull;

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

// Inverse pose rotation R_pose^T(ax, ay): undoes Y-yaw then X-pitch.
float3 scene_inv_rotate_pose(float3 v, float ax, float ay) {
    float cy = cos(ay), sy = sin(ay);
    float3 ry = float3(v.x * cy - v.z * sy, v.y, v.x * sy + v.z * cy);
    float cx = cos(ax), sx = sin(ax);
    return float3(ry.x, ry.y * cx + ry.z * sx, -ry.y * sx + ry.z * cx);
}

// Inverse camera quaternion rotation R_view^T = view_rotate(conj(q), v).
float3 scene_inv_view_rotate(float4 q, float3 v) {
    return view_rotate(float4(-q.xyz, q.w), v);
}

void scene_transform_vertex(
    ScenePackedVertex pv,
    SceneMeshlet      ml,
    SceneInstance     inst,
    ViewPush          view,
    uint              width,
    uint              height,
    out float4        out_clip,
    out float3        out_view_nrm,
    out float3        out_albedo,
    out float3        out_emit,
    out float         out_roughness)
{
    float3 local_p = scene_dequantize_pos(pv, ml);
    float3 local_n = scene_oct_decode(pv.oct_nx, pv.oct_ny);
    float4 rgba    = scene_unpack_rgba8(pv.rgba8);

    float3 posed_p = rotate_pose(local_p, inst.pitch_rad, inst.yaw_rad);
    float3 posed_n = rotate_pose(local_n, inst.pitch_rad, inst.yaw_rad);

    float sz  = clamp(1.0 + inst.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);

    float3 world_p = posed_p * inst.uniform_scale;
    world_p.x *= sxy;
    world_p.y *= sxy;
    world_p.z *= sz;
    world_p += float3(inst.origin_x, inst.origin_y, inst.origin_z);

    float3 scaled_n = normalize(float3(posed_n.x / sxy, posed_n.y / sxy, posed_n.z / sz));
    float3 vpos     = view_of(world_p, view.q, view.cam);

    out_clip      = view_ndc(vpos, view.zoom, width, height, inst.depth_bias);
    out_view_nrm  = view_rotate(view.q, scaled_n);
    out_albedo    = lerp(rgba.rgb, float3(1.0), clamp(inst.selected, 0.0, 1.0) * 0.42);
    out_emit      = material_emit(view.materials, inst.mat_index);
    out_roughness = inst.roughness > 0.0 ? inst.roughness : rgba.a;
}

uint scene_visbuf_pack_id(uint inst_id, uint meshlet_id, uint prim_id) {
    return (((inst_id + 1u) & 0x3FFFu) << 18u) | ((meshlet_id & 0x7FFu) << 7u) | (prim_id & 0x7Fu);
}

bool scene_visbuf_unpack_id(uint vis_id, out uint inst_id, out uint meshlet_id, out uint prim_id) {
    uint inst_p1 = (vis_id >> 18u) & 0x3FFFu;
    if (inst_p1 == 0u) {
        inst_id    = 0u;
        meshlet_id = 0u;
        prim_id    = 0u;
        return false;
    }
    inst_id    = inst_p1 - 1u;
    meshlet_id = (vis_id >> 7u) & 0x7FFu;
    prim_id    = vis_id & 0x7Fu;
    return true;
}

struct SceneBaryResult {
    float3 lambda;
    float3 dlambda_dx;
    float3 dlambda_dy;
    float  z_ndc;
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
    r.z_ndc      = 1.0;
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
    r.valid        = true;
    return r;
}
