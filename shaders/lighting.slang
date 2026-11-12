// Shared Slang 2026 Physically-Based Direct + Global Illumination Surface Module.
// Combines directional sun key light, cool rim fill, GGX microfacet specular,
// 2026 SSILVB (32-Bit Visibility Bitmask Ambient & Bent-Normal Occlusion + Bounce),
// and Split / Holographic Radiance Cascades (HRC) multi-bounce irradiance via BDA.

static const uint     k_lighting_magic         = 0x4C474934u;
static const uint64_t k_lighting_header_offset = 2048ull;

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
    uint     reserved[4];
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
    // Dereference only touched scalar uniform fields via pointer (avoids 256-byte struct copy per pixel)
    LightingFrameHeader *hdr = (LightingFrameHeader *)(env_bda + k_lighting_header_offset);
    if (hdr->magic != k_lighting_magic || hdr->gi_enabled == 0u) {
        return false;
    }
    uint64_t gi_curr_bda = hdr->gi_curr_bda;
    uint     gi_w        = hdr->gi_width;
    uint     gi_h        = hdr->gi_height;
    if (gi_curr_bda == 0ull || gi_w == 0u || gi_h == 0u) {
        return false;
    }

    float  full_w      = max(float(hdr->full_width),  1.0);
    float  full_h      = max(float(hdr->full_height), 1.0);
    float  zoom        = hdr->zoom;
    float  ao_strength = hdr->ao_strength;
    float  gi_intens   = hdr->gi_intensity;

    float2 uv   = sv_pos_xy / float2(full_w, full_h);
    float2 tc   = clamp(uv, float2(0.0), float2(1.0)) * float2(float(gi_w), float(gi_h)) - float2(0.5);
    int2   base = int2(floor(tc));
    float2 f    = tc - float2(base);

    int2 max_c = int2(int(gi_w) - 1, int(gi_h) - 1);
    LightingGiTexel *grid = (LightingGiTexel *)gi_curr_bda;

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
            float wb = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            float3 tnrm  = lighting_unpack_oct_snorm8_gpu(tex.packed_nrm_bent & 0xFFFFu);
            float3 tbent = lighting_unpack_oct_snorm8_gpu((tex.packed_nrm_bent >> 16u) & 0xFFFFu);
            float3 tirr  = lighting_unpack_rgb9e5_gpu(tex.irr_rgb9e5);
            float  tao   = float(tex.ao_conf_u16 & 0xFFFFu) * (1.0 / 65535.0);

            float dz = abs(tex.view_z - view_z) * inv_sigma_z;
            float wz = rcp(1.0 + dz + 0.48 * dz * dz);
            float nd = max(dot(tnrm, view_nrm), 0.0);
            float wn = nd * nd;
            wn       = wn * wn; // nd^4

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

float ggx_d(float ndh, float alpha) {
    float a2 = alpha * alpha;
    float d  = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1.0e-5);
}

float3 shade_surface_impl<S : IShadedSurface>(
    S        surf,
    bool     use_gi,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda)
{
    float3 n = surf.normal;
    const float3 to_sun  = normalize(float3(0.28, 0.86, 0.42));
    const float3 to_fill = normalize(float3(-0.62, -0.24, 0.74));
    const float3 to_view = float3(0.0, 0.0, 1.0);
    const float3 half_v  = normalize(to_sun + to_view);

    const float3 sun_col  = float3(1.00, 0.96, 0.88);
    const float3 fill_col = float3(0.20, 0.30, 0.46);
    const float3 sky_top  = float3(0.24, 0.30, 0.40);
    const float3 sky_bot  = float3(0.11, 0.13, 0.18);

    float  ao           = 1.0;
    float3 bent_n       = n;
    float3 indirect_gi  = float3(0.0);
    bool   have_gi      = false;
    if (use_gi) {
        have_gi = lighting_sample_gi(env_bda, sv_pos_xy, view_z, n, ao, bent_n, indirect_gi);
    }

    float  hemi    = clamp(bent_n.y * 0.5 + 0.5, 0.0, 1.0);
    float3 ambient = lerp(sky_bot, sky_top, hemi);
    float3 ao_mb   = have_gi ? lighting_gtao_multibounce_gpu(ao, surf.albedo) : float3(1.0);

    float ndl      = max(dot(n, to_sun), 0.0);
    float ndfill   = max(dot(n, to_fill), 0.0) * 0.22;
    float ndv      = max(abs(n.z), 1.0e-3);
    float ndh      = max(dot(n, half_v), 0.0);

    // Contact micro-shadowing from 32-bit visibility bitmask AO
    float micro_sh = have_gi ? clamp(ndl + 2.0 * ao * ao - 1.0, 0.0, 1.0) : ndl;
    float sun_term = lerp(ndl, micro_sh, 0.45);

    float alpha    = max(surf.roughness * surf.roughness, 0.08);
    float spec_d   = ggx_d(ndh, alpha);
    float fresnel  = 0.04 + 0.96 * pow(1.0 - clamp(ndv, 0.0, 1.0), 4.0);
    // Lagarde / de Rousiers physically-based specular occlusion from VBAO
    float spec_occ = have_gi
        ? clamp(pow(ndv + ao, exp2(-16.0 * surf.roughness - 1.0)) - 1.0 + ao, 0.0, 1.0)
        : 1.0;
    float3 spec    = sun_col * (spec_d * fresnel * sun_term * 0.14 * spec_occ);

    float  rim     = pow(1.0 - clamp(ndl, 0.0, 1.0), 2.0);
    float3 direct  = sun_col * sun_term + fill_col * (ndfill * lerp(0.35, 1.0, ao));
    float3 diffuse = surf.albedo * (ambient * ao_mb + direct + indirect_gi);
    float3 emit    = surf.emissive * (0.68 + 0.32 * max(rim, fresnel));

    float3 hdr_col = diffuse + spec + emit;
    // Smooth highlight shoulder above 0.85 so bright emissive/thermal GI bounces keep rich chroma
    float  peak    = max(hdr_col.r, max(hdr_col.g, hdr_col.b));
    if (peak > 0.85) {
        float mapped = 0.85 + 0.15 * ((peak - 0.85) / (0.15 + (peak - 0.85)));
        hdr_col *= mapped / peak;
    }
    return clamp(hdr_col, float3(0.0), float3(1.0));
}

float3 shade_surface<S : IShadedSurface>(S surf) {
    return shade_surface_impl(surf, false, float2(0.0), 0.0, 0ull);
}

float3 shade_surface_gi<S : IShadedSurface>(
    S        surf,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda)
{
    return shade_surface_impl(surf, true, sv_pos_xy, view_z, env_bda);
}
