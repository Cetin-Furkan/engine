// Shared Slang 2026 Lighting Constants, BDA Struct Layouts, Surface Interfaces & Math Helpers.

static const uint     k_lighting_magic           = 0x4C474934u;
static const uint64_t k_lighting_header_offset   = 2048ull;
static const uint64_t k_lighting_scene_offset    = 10496ull;
static const uint64_t k_lighting_bounds_offset   = 10560ull;
static const uint64_t k_lighting_shadow_offset   = 12608ull;
static const uint     k_lighting_proxy_obb       = 0u;
static const uint     k_lighting_proxy_ellipsoid = 1u;
static const uint     k_lighting_flag_has_shadow   = 1u << 0u;
static const uint     k_lighting_flag_filt_in_gbuf = 1u << 1u;
static const uint     k_lighting_debug_shift       = 4u;
static const uint     k_lighting_debug_lit         = 0u;
static const uint     k_lighting_debug_albedo      = 1u;
static const uint     k_lighting_debug_normals     = 2u;
static const uint     k_lighting_debug_roughness   = 3u;
static const uint     k_lighting_debug_depth       = 4u;
static const uint     k_lighting_debug_gi          = 5u;
static const uint     k_lighting_debug_ao          = 6u;
static const uint     k_lighting_debug_shadow      = 7u;
static const uint     k_lighting_debug_meshlets    = 8u;
static const float    k_lighting_pi                = 3.14159265359;
static const float    k_lighting_inv_pi            = 0.31830988618;

struct LightingFrameHeader {
    uint     magic;
    uint     gi_enabled;
    uint     oracle_mode;
    uint     frame_index;
    uint     full_width;
    uint     full_height;
    uint     gi_width;
    uint     gi_height;
    uint     hrc_w0;
    uint     hrc_h0;
    uint     proxy_count;
    uint     has_ground;
    float    ground_z;
    float    ground_r;
    float    ground_g;
    float    ground_b;
    uint64_t proxies_bda;
    uint64_t gbuf_bda;
    uint64_t hrc_bda;
    uint64_t gi_curr_bda;
    uint64_t gi_prev_bda;
    uint64_t terrain_dir_bda;
    uint64_t scene_hiz_bda;
    float    elev_base_m;
    float    vertical_scale;
    uint     history_valid;
    uint     ssilvb_slices;
    uint     ssilvb_steps;
    uint     hrc_cascades;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z, zoom;
    float    prev_qx, prev_qy, prev_qz, prev_qw;
    float    prev_cam_x, prev_cam_y, prev_cam_z, prev_zoom;
    float    gi_intensity;
    float    ao_strength;
    float    ssilvb_radius_px;
    float    thickness_world;
    uint     hrc_offset_rays[4];
    uint64_t scene_geom_bda;
    uint     scene_inst_count;
    uint     scene_flags;
};

struct LightingShadowBlock {
    float4 light_vp0;
    float4 light_vp1;
    float4 light_vp2;
    float4 light_vp3;
    float3 sun_dir;
    float  inv_shadow_res;
    float3 sun_color;
    float  persp;
    float3 sky_top;
    float  prev_persp;
    float3 sky_horizon;
    float  normal_bias;
    float3 sky_ground;
    uint   shadow_enabled;
};

struct LightingProxyGpu {
    float3 center;
    uint   kind;
    float3 extents;
    float  roughness;
    float  pitch_rad;
    float  yaw_rad;
    uint   alb_rg;
    uint   alb_b_flags;
    uint   emi_rg;
    uint   emi_b_rad;
    uint   trig_pitch;
    uint   trig_yaw;
};

struct LightingGiTexel {
    float view_z;
    uint  packed_nrm_bent;
    uint  irr_rgb9e5;
    uint  ao_conf_u16;
};

interface IShadedSurface {
    property float3 normal    { get; }
    property float3 albedo    { get; }
    property float3 emissive  { get; }
    property float  roughness { get; }
};

struct SurfaceFragment : IShadedSurface {
    float3 nrm;
    float3 alb;
    float3 emi;
    float  rgh;

    property float3 normal    { get { return normalize(nrm); } }
    property float3 albedo    { get { return alb; } }
    property float3 emissive  { get { return emi; } }
    property float  roughness { get { return rgh; } }
};

struct PbrSurfaceFragment {
    float3 nrm;       // View-space shading normal (already normal-mapped & face-flipped)
    float3 alb_lin;   // Linear RGB baseColor (hardware sRGB decoded * linear factor * vertex COLOR_0)
    float3 emi_lin;   // Linear HDR emissive RGB * KHR_materials_emissive_strength
    float  rgh;       // Perceptual roughness [0..1]
    float  metallic;  // Metallic factor [0..1]
    float  occlusion; // Baked material AO [0..1]
    float  ior;       // KHR_materials_ior (default 1.5)
    bool   unlit;     // KHR_materials_unlit
};

// Accurate fast sRGB -> linear light conversion
float3 lighting_srgb_to_linear(float3 s) {
    float3 c = clamp(s, float3(0.0), float3(1.0));
    return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
}

float3 lighting_unpack_rgb9e5_gpu(uint packed) {
    uint  rm    =  packed         & 0x1FFu;
    uint  gm    = (packed >>  9u) & 0x1FFu;
    uint  bm    = (packed >> 18u) & 0x1FFu;
    int   e     = int((packed >> 27u) & 0x1Fu) - 15 - 9;
    float scale = exp2(float(e));
    return float3(float(rm), float(gm), float(bm)) * scale;
}

float3 lighting_unpack_oct_snorm8_gpu(uint packed16) {
    int   qx = (int(packed16 << 24u)) >> 24;
    int   qy = (int((packed16 >> 8u) << 24u)) >> 24;
    float2 e = clamp(float2(float(qx), float(qy)) * (1.0 / 127.0), float2(-1.0), float2(1.0));
    float3 v = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        float2 s = float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
        v.xy = (float2(1.0) - abs(v.yx)) * s;
    }
    return normalize(v);
}

// Jimenez et al. energy-conserving multi-bounce ambient occlusion tint
float3 lighting_gtao_multibounce_gpu(float visibility, float3 albedo) {
    float  v   = clamp(visibility, 0.0, 1.0);
    float3 rho = clamp(albedo, float3(0.0), float3(0.99));
    float3 a   =  2.0404 * rho - float3(0.3324);
    float3 b   = -4.7951 * rho + float3(0.6417);
    float3 d   =  2.7552 * rho + float3(0.6903);
    return clamp(max(float3(v), ((float3(v) * a + b) * float3(v) + d) * float3(v)), float3(0.0), float3(1.0));
}

float3 lighting_world_sun_dir()     { return normalize(float3(0.38, 0.56, 0.735)); }
float3 lighting_world_fill_dir()    { return normalize(float3(-0.68, -0.42, 0.60)); }
float3 lighting_terrain_sun_dir()   { return normalize(float3(-0.50, -0.52, 0.69)); }
float3 lighting_terrain_fill_dir()  { return normalize(float3(0.55, 0.45, 0.70)); }

float3 lighting_q_rotate(float4 q, float3 v) {
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float3 lighting_q_inv_rotate(float4 q, float3 v) {
    return lighting_q_rotate(float4(-q.xyz, q.w), v);
}

float3 lighting_pose_to_local_fast(float3 v, float sx, float cx, float sy, float cy) {
    float3 ry = float3(v.x * cy - v.z * sy, v.y, v.x * sy + v.z * cy);
    return float3(ry.x, ry.y * cx + ry.z * sx, -ry.y * sx + ry.z * cx);
}

float3 lighting_local_to_pose_fast(float3 v, float sx, float cx, float sy, float cy) {
    float3 rx = float3(v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx);
    return float3(rx.x * cy + rx.z * sy, rx.y, -rx.x * sy + rx.z * cy);
}

// Reconstruct world position from screen pixel + view_z (handles both perspective & ortho)
float3 lighting_sv_to_world_persp(LightingFrameHeader hdr, float persp, float2 sv_pos_xy, float view_z) {
    float  z      = max(hdr.zoom, 1.0e-5);
    float  w_clip = (persp > 1.0e-7) ? max(1.0 - view_z * persp, 1.0e-4) : 1.0;
    float3 vp     = float3(
        ((sv_pos_xy.x - float(hdr.full_width)  * 0.5) / z) * w_clip,
        ((float(hdr.full_height) * 0.5 - sv_pos_xy.y) / z) * w_clip,
        view_z);
    float4 q = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    return float3(hdr.cam_x, hdr.cam_y, hdr.cam_z) + lighting_q_inv_rotate(q, vp);
}
