// Shared Slang 2026 Physically-Based Direct + Hardware Shadow + Global Illumination Module.
// - Full Cook-Torrance BRDF (GGX NDF + Height-Correlated Smith Visibility + Schlick Fresnel)
// - 9-tap IGN-Rotated Vogel-Disk Hardware PCF Shadow Mapping (Vulkan 1.4 Sampler2DShadow)
// - Analytic Studio Sky / Horizon / Ground-Bounce IBL (diffuse + roughness-prefiltered specular)
// - 2026 SSILVB (32-bit visibility bitmask AO & bent normal) + Split HRC GI via BDA
// - AgX Filmic Tone Mapping (Troy Sobotka inset/outset + contrast curve, linearized for sRGB swapchain)

static const uint     k_lighting_magic           = 0x4C474934u;
static const uint64_t k_lighting_header_offset   = 2048ull;
static const uint64_t k_lighting_scene_offset    = 10496ull;
static const uint64_t k_lighting_bounds_offset   = 10560ull;
static const uint64_t k_lighting_shadow_offset   = 12608ull;
static const uint     k_lighting_proxy_obb       = 0u;
static const uint     k_lighting_proxy_ellipsoid = 1u;
static const uint     k_lighting_flag_has_shadow   = 1u << 0u;
static const uint     k_lighting_flag_filt_in_gbuf = 1u << 1u;
static const float    k_lighting_pi              = 3.14159265359;
static const float    k_lighting_inv_pi          = 0.31830988618;

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

// Accurate fast sRGB -> linear light conversion
float3 lighting_srgb_to_linear(float3 s) {
    float3 c = clamp(s, float3(0.0), float3(1.0));
    return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
}

// Troy Sobotka AgX filmic tone mapping (output linearized for VK_FORMAT_B8G8R8A8_SRGB hardware OETF)
float3 lighting_agx_tonemap(float3 col) {
    float3 c = max(col, float3(0.0));
    const float3x3 agx_mat = float3x3(
        0.842479062253094,  0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.878468636469772,  0.0791661274605434,
        0.0423756549057051, 0.0784336,          0.879142973793104
    );
    const float3x3 agx_inv = float3x3(
         1.19687900512017,  -0.0980208811401368, -0.0990297440797205,
        -0.0528968517574562, 1.15190312990417,   -0.0989611768448433,
        -0.0529716355144438,-0.0980434501171241,  1.15107367264116
    );
    float3 v = mul(agx_mat, c);
    v = clamp(log2(max(v, float3(1.0e-6))), float3(-12.47393), float3(4.026069));
    v = (v - float3(-12.47393)) * (1.0 / (4.026069 - (-12.47393)));
    float3 v2 = v * v;
    float3 v4 = v2 * v2;
    v =  15.5    * v4 * v2
       - 40.14   * v4 * v
       + 31.96   * v4
       -  6.868  * v2 * v
       +  0.4298 * v2
       +  0.1191 * v
       -  0.00232;
    v = mul(agx_inv, v);
    return lighting_srgb_to_linear(v);
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

float3 lighting_world_sun_dir() {
    return normalize(float3(0.38, 0.56, 0.735));
}

float3 lighting_world_fill_dir() {
    return normalize(float3(-0.68, -0.42, 0.60));
}

float3 lighting_terrain_sun_dir() {
    return normalize(float3(-0.50, -0.52, 0.69));
}

float3 lighting_terrain_fill_dir() {
    return normalize(float3(0.55, 0.45, 0.70));
}

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

float3 lighting_proxy_albedo(LightingProxyGpu p) {
    return float3(
        f16tof32(p.alb_rg & 0xFFFFu),
        f16tof32((p.alb_rg >> 16u) & 0xFFFFu),
        f16tof32(p.alb_b_flags & 0xFFFFu)
    );
}

float3 lighting_proxy_emissive(LightingProxyGpu p) {
    return float3(
        f16tof32(p.emi_rg & 0xFFFFu),
        f16tof32((p.emi_rg >> 16u) & 0xFFFFu),
        f16tof32(p.emi_b_rad & 0xFFFFu)
    );
}

float lighting_proxy_bound_radius(LightingProxyGpu p) {
    float r = f16tof32((p.emi_b_rad >> 16u) & 0xFFFFu);
    return r > 1.0e-4 ? r : (length(p.extents) * 1.02);
}

// Fast exact analytical OBB / Ellipsoid proxy ray intersection (zero SDF march loops!)
bool lighting_ray_proxy(
    float3           ro,
    float3           rd,
    float            t_min,
    float            t_max,
    LightingProxyGpu proxy,
    out float        out_t,
    out float3       out_world_nrm)
{
    out_t         = t_max;
    out_world_nrm = float3(0.0, 0.0, 1.0);

    float3 rel   = ro - proxy.center;
    float  r_sph = lighting_proxy_bound_radius(proxy);
    float  b_sph = dot(rel, rd);
    float  c_sph = dot(rel, rel) - r_sph * r_sph;
    if (c_sph > 0.0 && b_sph > 0.0) {
        return false;
    }
    if (b_sph * b_sph - c_sph < 0.0) {
        return false;
    }
    if (-b_sph > t_max + r_sph || -b_sph < t_min - r_sph) {
        return false;
    }

    float sx = f16tof32(proxy.trig_pitch & 0xFFFFu);
    float cx = f16tof32((proxy.trig_pitch >> 16u) & 0xFFFFu);
    float sy = f16tof32(proxy.trig_yaw   & 0xFFFFu);
    float cy = f16tof32((proxy.trig_yaw   >> 16u) & 0xFFFFu);

    float3 lo = lighting_pose_to_local_fast(rel, sx, cx, sy, cy);
    float3 ld = lighting_pose_to_local_fast(rd,  sx, cx, sy, cy);

    if (proxy.kind == k_lighting_proxy_obb) {
        float t0 = t_min;
        float t1 = t_max;
        int   hit_axis = -1;
        float hit_sign = 1.0;

        [unroll]
        for (int a = 0; a < 3; ++a) {
            float ext = max(proxy.extents[a], 1.0e-4);
            if (abs(ld[a]) < 1.0e-7) {
                if (lo[a] < -ext || lo[a] > ext) {
                    return false;
                }
            } else {
                float inv_d = 1.0 / ld[a];
                float s0    = (-ext - lo[a]) * inv_d;
                float s1    = ( ext - lo[a]) * inv_d;
                float sgn   = -1.0;
                if (s0 > s1) {
                    float tmp = s0; s0 = s1; s1 = tmp;
                    sgn = 1.0;
                }
                if (s0 > t0) {
                    t0       = s0;
                    hit_axis = a;
                    hit_sign = sgn;
                }
                t1 = min(t1, s1);
                if (t0 > t1) {
                    return false;
                }
            }
        }
        if (t0 < t_min || t0 > t_max) {
            return false;
        }
        float3 ln;
        if (hit_axis == 0)      ln = float3(hit_sign, 0.0, 0.0);
        else if (hit_axis == 1) ln = float3(0.0, hit_sign, 0.0);
        else if (hit_axis == 2) ln = float3(0.0, 0.0, hit_sign);
        else {
            float3 hp  = lo + ld * t0;
            float3 rem = abs(proxy.extents - abs(hp));
            if (rem.x <= rem.y && rem.x <= rem.z)      ln = float3(hp.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
            else if (rem.y <= rem.z)                   ln = float3(0.0, hp.y >= 0.0 ? 1.0 : -1.0, 0.0);
            else                                       ln = float3(0.0, 0.0, hp.z >= 0.0 ? 1.0 : -1.0);
        }
        out_t         = t0;
        out_world_nrm = normalize(lighting_local_to_pose_fast(ln, sx, cx, sy, cy));
        return true;
    }

    float3 inv_r = 1.0 / max(proxy.extents, float3(1.0e-4));
    float3 so    = lo * inv_r;
    float3 sd    = ld * inv_r;
    float  qa    = dot(sd, sd);
    if (qa < 1.0e-10) {
        return false;
    }
    float qb   = dot(so, sd);
    float qc   = dot(so, so) - 1.0;
    float disc = qb * qb - qa * qc;
    if (disc < 0.0) {
        return false;
    }
    float root  = sqrt(disc);
    float t_hit = (-qb - root) / qa;
    if (t_hit < t_min || t_hit > t_max) {
        t_hit = (-qb + root) / qa;
        if (t_hit < t_min || t_hit > t_max) {
            return false;
        }
    }
    float3 hp     = lo + ld * t_hit;
    float3 ln     = normalize(hp * inv_r * inv_r);
    out_t         = t_hit;
    out_world_nrm = normalize(lighting_local_to_pose_fast(ln, sx, cx, sy, cy));
    return true;
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

// Studio atmospheric / sky dome radiance (linear HDR) for backdrop, reflections, and HRC sky
float3 lighting_eval_sky_studio(float3 world_dir, float3 sun_dir, float3 sky_top,
                                float3 sky_horizon, float3 sky_ground, float roughness) {
    float z_up = world_dir.z;
    float3 dome;
    if (z_up >= 0.0) {
        float t = pow(clamp(z_up, 0.0, 1.0), lerp(0.58, 1.0, roughness));
        dome = lerp(sky_horizon, sky_top, t);
    } else {
        float t = pow(clamp(-z_up, 0.0, 1.0), lerp(0.55, 1.0, roughness));
        dome = lerp(sky_horizon * 0.68, sky_ground, t);
    }
    float sun_dot = max(dot(world_dir, sun_dir), 0.0);
    float sun_exp = lerp(110.0, 6.0, roughness);
    float sun_amp = lerp(1.45, 0.28, roughness);
    float halo    = pow(sun_dot, 7.0) * 0.22;
    return dome + float3(1.00, 0.90, 0.72) * (pow(sun_dot, sun_exp) * sun_amp + halo);
}

float3 lighting_eval_sky_env_hdr(LightingFrameHeader hdr, float3 world_dir) {
    if (hdr.terrain_dir_bda != 0ull) {
        float3 sun_dir = lighting_terrain_sun_dir();
        float  up      = clamp(world_dir.z * 0.5 + 0.5, 0.0, 1.0);
        float  sun     = pow(max(dot(world_dir, sun_dir), 0.0), 8.0);
        float3 sky     = lerp(float3(0.18, 0.16, 0.14), float3(0.36, 0.46, 0.62), up);
        return sky + float3(0.55, 0.48, 0.36) * sun;
    }
    return lighting_eval_sky_studio(
        world_dir,
        lighting_world_sun_dir(),
        float3(0.34, 0.52, 0.84),
        float3(0.62, 0.70, 0.80),
        float3(0.16, 0.19, 0.25),
        0.55);
}

// Joint 2x2 depth + normal bilateral upsampler from BDA LightingGiTexel grid
bool lighting_sample_gi(
    uint64_t   env_bda,
    float2     sv_pos_xy,
    float      view_z,
    float3     view_nrm,
    out float  out_ao,
    out float3 out_bent_nrm,
    out float3 out_indirect_rgb)
{
    out_ao           = 1.0;
    out_bent_nrm     = view_nrm;
    out_indirect_rgb = float3(0.0);

    if (env_bda == 0ull) {
        return false;
    }
    LightingFrameHeader *hdr = (LightingFrameHeader *)(env_bda + k_lighting_header_offset);
    if (hdr->magic != k_lighting_magic || hdr->gi_enabled == 0u) {
        return false;
    }
    uint64_t gi_bda = ((hdr->scene_flags & k_lighting_flag_filt_in_gbuf) != 0u)
                    ? hdr->gbuf_bda
                    : hdr->gi_curr_bda;
    uint gi_w = hdr->gi_width;
    uint gi_h = hdr->gi_height;
    if (gi_bda == 0ull || gi_w == 0u || gi_h == 0u) {
        return false;
    }

    float full_w      = max(float(hdr->full_width),  1.0);
    float full_h      = max(float(hdr->full_height), 1.0);
    float zoom        = hdr->zoom;
    float ao_strength = hdr->ao_strength;
    float gi_intens   = hdr->gi_intensity;

    float2 uv   = sv_pos_xy / float2(full_w, full_h);
    float2 tc   = clamp(uv, float2(0.0), float2(1.0)) * float2(float(gi_w), float(gi_h)) - float2(0.5);
    int2   base = int2(floor(tc));
    float2 f    = tc - float2(base);

    int2 max_c = int2(int(gi_w) - 1, int(gi_h) - 1);
    LightingGiTexel *grid = (LightingGiTexel *)gi_bda;

    float  inv_sigma_z = max(zoom * 0.085, 0.004);
    float  w_sum       = 0.0;
    float  ao_sum      = 0.0;
    float3 bent_sum    = float3(0.0);
    float3 irr_sum     = float3(0.0);

    float  fb_w_sum    = 0.0;
    float  fb_ao_sum   = 0.0;
    float3 fb_bent_sum = float3(0.0);
    float3 fb_irr_sum  = float3(0.0);

    [unroll]
    for (int dy = 0; dy <= 1; ++dy) {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx) {
            int2 p = clamp(base + int2(dx, dy), int2(0), max_c);
            LightingGiTexel tex = grid[uint(p.y) * gi_w + uint(p.x)];
            if (tex.view_z <= -1.0e19) {
                continue;
            }
            float  wb    = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            float3 tnrm  = lighting_unpack_oct_snorm8_gpu(tex.packed_nrm_bent & 0xFFFFu);
            float3 tbent = lighting_unpack_oct_snorm8_gpu((tex.packed_nrm_bent >> 16u) & 0xFFFFu);
            float3 tirr  = lighting_unpack_rgb9e5_gpu(tex.irr_rgb9e5);
            float  tao   = float(tex.ao_conf_u16 & 0xFFFFu) * (1.0 / 65535.0);

            float dz = abs(tex.view_z - view_z) * inv_sigma_z;
            float wz = rcp(1.0 + dz + 0.48 * dz * dz);
            float nd = max(dot(tnrm, view_nrm), 0.0);
            float wn = nd * nd;
            wn       = wn * wn;

            float w = max(wb, 0.02) * wz * max(wn, 0.002);
            w_sum    += w;
            ao_sum   += tao * w;
            bent_sum += tbent * w;
            irr_sum  += tirr * w;

            float fbw = max(wb, 0.05);
            fb_w_sum    += fbw;
            fb_ao_sum   += tao * fbw;
            fb_bent_sum += tbent * fbw;
            fb_irr_sum  += tirr * fbw;
        }
    }

    if (w_sum > 1.0e-5) {
        float inv_w      = 1.0 / w_sum;
        out_ao           = clamp(lerp(1.0, ao_sum * inv_w, ao_strength), 0.0, 1.0);
        float3 b_dir     = bent_sum * inv_w;
        out_bent_nrm     = dot(b_dir, b_dir) > 1.0e-6 ? normalize(b_dir) : view_nrm;
        out_indirect_rgb = (irr_sum * inv_w) * gi_intens;
        return true;
    }
    if (fb_w_sum > 1.0e-5) {
        float inv_fb     = 1.0 / fb_w_sum;
        out_ao           = clamp(lerp(1.0, fb_ao_sum * inv_fb, ao_strength * 0.65), 0.0, 1.0);
        float3 b_dir     = fb_bent_sum * inv_fb;
        out_bent_nrm     = dot(b_dir, b_dir) > 1.0e-6 ? normalize(b_dir) : view_nrm;
        out_indirect_rgb = (fb_irr_sum * inv_fb) * (gi_intens * 0.75);
        return true;
    }
    return false;
}

// Fast 1-tap hardware PCF lookup for GI compute pass
float lighting_sample_shadow_1tap(
    LightingShadowBlock sb,
    Sampler2DShadow     shadow_map,
    float3              world_pos,
    float3              world_nrm)
{
    if (sb.shadow_enabled == 0u) {
        return 1.0;
    }
    float ndl = dot(world_nrm, sb.sun_dir);
    if (ndl <= 0.002) {
        return 0.0;
    }
    float3 biased = world_pos + world_nrm * (sb.normal_bias * (1.25 - 0.55 * ndl));
    float4 wp     = float4(biased, 1.0);
    float3 suv    = float3(dot(sb.light_vp0, wp), dot(sb.light_vp1, wp), dot(sb.light_vp2, wp));
    if (suv.x <= 0.002 || suv.x >= 0.998 || suv.y <= 0.002 || suv.y >= 0.998 ||
        suv.z <= 0.0001 || suv.z >= 0.9999) {
        return 1.0;
    }
    return shadow_map.SampleCmpLevelZero(suv.xy, suv.z);
}

// 9-tap Interleaved-Gradient-Noise rotated Vogel-disk Hardware PCF soft shadow sampler
float lighting_sample_shadow(
    LightingShadowBlock sb,
    Sampler2DShadow     shadow_map,
    float3              world_pos,
    float3              world_nrm,
    float2              sv_pos_xy)
{
    if (sb.shadow_enabled == 0u) {
        return 1.0;
    }
    float ndl = dot(world_nrm, sb.sun_dir);
    if (ndl <= 0.001) {
        return 0.0;
    }
    float3 biased = world_pos + world_nrm * (sb.normal_bias * (1.30 - 0.60 * ndl));
    float4 wp     = float4(biased, 1.0);
    float3 suv    = float3(dot(sb.light_vp0, wp), dot(sb.light_vp1, wp), dot(sb.light_vp2, wp));

    float edge_d = min(min(suv.x, 1.0 - suv.x), min(suv.y, 1.0 - suv.y));
    if (edge_d <= 0.0 || suv.z <= 0.0001 || suv.z >= 0.9999) {
        return 1.0;
    }

    float ign      = frac(52.9829189 * frac(dot(sv_pos_xy, float2(0.06711056, 0.00583715))));
    float phi0     = ign * 6.28318530718;
    float filter_r = sb.inv_shadow_res * 2.25;
    float vis      = 0.0;

    [unroll]
    for (int i = 0; i < 9; ++i) {
        float  r     = sqrt((float(i) + 0.5) * (1.0 / 9.0)) * filter_r;
        float  theta = float(i) * 2.3999632297 + phi0;
        float2 off   = float2(cos(theta), sin(theta)) * r;
        vis += shadow_map.SampleCmpLevelZero(suv.xy + off, suv.z);
    }
    vis *= (1.0 / 9.0);
    float border_fade = smoothstep(0.0, 0.04, edge_d);
    return lerp(1.0, vis, border_fade);
}

// Cook-Torrance GGX Normal Distribution Function
float lighting_d_ggx(float ndh, float a2) {
    float d = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(k_lighting_pi * d * d, 1.0e-5);
}

// Heitz Height-Correlated Smith GGX Visibility Function
float lighting_v_smith_ggx_correlated(float ndv, float ndl, float a2) {
    float gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1.0e-5);
}

float3 lighting_fresnel_schlick(float cos_theta, float3 f0) {
    float t = 1.0 - clamp(cos_theta, 0.0, 1.0);
    float t2 = t * t;
    return f0 + (float3(1.0) - f0) * (t2 * t2 * t);
}

// Core Physically-Based Surface Shading + AgX Tone Mapping
float3 shade_surface_core<S : IShadedSurface>(
    S        surf,
    bool     use_gi,
    bool     use_shadow,
    float    sun_vis_in,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda,
    float3   world_pos_override,
    bool     have_world_pos)
{
    float3 n        = surf.normal;
    float  rgh      = clamp(surf.roughness, 0.08, 0.98);
    float3 alb_lin  = lighting_srgb_to_linear(surf.albedo);
    float3 emit_lin = max(surf.emissive, float3(0.0));

    LightingFrameHeader hdr = {};
    LightingShadowBlock sb  = {};
    bool has_hdr = false;
    if (env_bda != 0ull) {
        hdr     = ((LightingFrameHeader *)(env_bda + k_lighting_header_offset))[0];
        has_hdr = (hdr.magic == k_lighting_magic);
        if (has_hdr) {
            sb = ((LightingShadowBlock *)(env_bda + k_lighting_shadow_offset))[0];
        }
    }

    float  ao          = 1.0;
    float3 bent_n      = n;
    float3 indirect_gi = float3(0.0);
    bool   have_gi     = false;
    if (use_gi && has_hdr && hdr.gi_enabled != 0u) {
        have_gi = lighting_sample_gi(env_bda, sv_pos_xy, view_z, n, ao, bent_n, indirect_gi);
    }

    float3 sun_dir     = lighting_world_sun_dir();
    float3 fill_dir    = lighting_world_fill_dir();
    float3 sun_col     = float3(2.95, 2.72, 2.38);
    float3 fill_col    = float3(0.42, 0.58, 0.88);
    float3 sky_top     = float3(0.34, 0.52, 0.84);
    float3 sky_horizon = float3(0.62, 0.70, 0.80);
    float3 sky_ground  = float3(0.16, 0.19, 0.25);

    float3 eval_n    = n;
    float3 eval_bent = bent_n;
    float3 view_dir  = float3(0.0, 0.0, 1.0);
    float  sun_vis   = use_shadow ? sun_vis_in : 1.0;

    if (has_hdr) {
        float4 q  = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
        eval_n    = normalize(lighting_q_inv_rotate(q, n));
        eval_bent = normalize(lighting_q_inv_rotate(q, bent_n));

        float3 wpos = have_world_pos
            ? world_pos_override
            : lighting_sv_to_world_persp(hdr, sb.persp, sv_pos_xy, view_z);

        if (sb.persp > 1.0e-7) {
            float3 eye_w = float3(hdr.cam_x, hdr.cam_y, hdr.cam_z)
                         + lighting_q_inv_rotate(q, float3(0.0, 0.0, 1.0 / sb.persp));
            view_dir = normalize(eye_w - wpos);
        } else {
            view_dir = normalize(lighting_q_inv_rotate(q, float3(0.0, 0.0, 1.0)));
        }

        if (hdr.terrain_dir_bda != 0ull) {
            sun_dir  = lighting_terrain_sun_dir();
            fill_dir = lighting_terrain_fill_dir();
        } else {
            sun_dir     = sb.sun_dir;
            sun_col     = sb.sun_color;
            sky_top     = sb.sky_top;
            sky_horizon = sb.sky_horizon;
            sky_ground  = sb.sky_ground;
        }
    }

    float3 half_v = normalize(sun_dir + view_dir);
    float  ndl    = max(dot(eval_n, sun_dir),  0.0);
    float  ndfill = max(dot(eval_n, fill_dir), 0.0);
    float  ndv    = max(dot(eval_n, view_dir), 1.0e-3);
    float  ndh    = max(dot(eval_n, half_v),   0.0);
    float  vdh    = max(dot(view_dir, half_v), 0.0);

    // Contact micro-shadowing from 32-bit visibility bitmask AO, gated by hardware shadow map
    float micro_sh = have_gi ? clamp(ndl + 2.0 * ao * ao - 1.0, 0.0, 1.0) : ndl;
    float sun_term = lerp(ndl, micro_sh, 0.35) * sun_vis;

    // Cook-Torrance dielectric microfacet BRDF (F0 = 0.04)
    const float3 f0 = float3(0.04);
    float  alpha = rgh * rgh;
    float  a2    = max(alpha * alpha, 0.002);
    float  D     = lighting_d_ggx(ndh, a2);
    float  V     = lighting_v_smith_ggx_correlated(ndv, max(ndl, 1.0e-3), a2);
    float3 F_sun = lighting_fresnel_schlick(vdh, f0);
    float3 kD    = (float3(1.0) - F_sun) * k_lighting_inv_pi;

    // Lagarde / de Rousiers physically-based specular occlusion from VBAO
    float spec_occ = have_gi
        ? clamp(pow(ndv + ao, exp2(-16.0 * rgh - 1.0)) - 1.0 + ao, 0.0, 1.0)
        : 1.0;

    float3 direct_diff = alb_lin * kD * (sun_col * sun_term + fill_col * (ndfill * 0.42 * lerp(0.40, 1.0, ao)));
    float3 direct_spec = sun_col * (D * V * F_sun * sun_term * spec_occ);

    // Analytic sky / horizon / ground-bounce diffuse IBL modulated by Jimenez multi-bounce AO
    float3 sky_diff_irr = lighting_eval_sky_studio(eval_bent, sun_dir, sky_top, sky_horizon, sky_ground, 0.92);
    float3 ao_mb        = have_gi ? lighting_gtao_multibounce_gpu(ao, alb_lin) : float3(1.0);
    float3 ibl_diff     = alb_lin * sky_diff_irr * (ao_mb * 0.62);

    // Analytic roughness-prefiltered specular IBL (zero software raytracing!)
    float3 refl_w    = reflect(-view_dir, eval_n);
    float3 env_spec  = lighting_eval_sky_studio(refl_w, sun_dir, sky_top, sky_horizon, sky_ground, rgh);
    float3 F_ibl     = f0 + (max(float3(1.0 - rgh), f0) - f0) * pow(1.0 - ndv, 5.0);
    float3 ibl_spec  = env_spec * F_ibl * (spec_occ * 0.78);

    // Multi-bounce SSILVB + Split HRC indirect diffuse radiance
    float3 gi_diff = alb_lin * indirect_gi * (k_lighting_inv_pi * 2.10);

    // Rich emissive glow with subtle Fresnel rim lift
    float  fresnel_edge = pow(1.0 - ndv, 3.0);
    float3 emit_term    = emit_lin * (2.40 + 0.85 * fresnel_edge);

    float3 hdr_col = direct_diff + direct_spec + ibl_diff + ibl_spec + gi_diff + emit_term;
    return lighting_agx_tonemap(hdr_col);
}

float3 shade_surface_gi_shadow<S : IShadedSurface>(
    S               surf,
    float2          sv_pos_xy,
    float           view_z,
    uint64_t        env_bda,
    Sampler2DShadow shadow_map)
{
    float  sun_vis = 1.0;
    float3 wpos    = float3(0.0);
    bool   have_wp = false;
    if (env_bda != 0ull) {
        LightingFrameHeader hdr = ((LightingFrameHeader *)(env_bda + k_lighting_header_offset))[0];
        if (hdr.magic == k_lighting_magic) {
            LightingShadowBlock sb = ((LightingShadowBlock *)(env_bda + k_lighting_shadow_offset))[0];
            wpos    = lighting_sv_to_world_persp(hdr, sb.persp, sv_pos_xy, view_z);
            have_wp = true;
            if (sb.shadow_enabled != 0u) {
                float4 q      = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
                float3 eval_n = normalize(lighting_q_inv_rotate(q, surf.normal));
                sun_vis       = lighting_sample_shadow(sb, shadow_map, wpos, eval_n, sv_pos_xy);
            }
        }
    }
    return shade_surface_core(surf, true, true, sun_vis, sv_pos_xy, view_z, env_bda, wpos, have_wp);
}

float3 shade_surface_gi<S : IShadedSurface>(
    S        surf,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda)
{
    return shade_surface_core(surf, true, false, 1.0, sv_pos_xy, view_z, env_bda, float3(0.0), false);
}

float3 shade_surface<S : IShadedSurface>(S surf) {
    return shade_surface_core(surf, false, false, 1.0, float2(0.0), 0.0, 0ull, float3(0.0), false);
}
