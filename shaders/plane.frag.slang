// True 3D World-Space Studio Floor + Atmospheric Sky Horizon (when is_3d != 0)
// and Anti-Aliased 2D Drafting Metric Sheet (when is_3d == 0).
// - True perspective / orthographic ray intersection with z = ground_z
// - World-space fwidth() anti-aliased infinite architectural grid + crisp X/Y origin axes
// - Full Cook-Torrance PBR floor shading + 9-tap Vogel-disk Hardware PCF soft shadows
// - Live 2026 SSILVB contact AO & Split HRC colored indirect bounce pools
// - Smooth exponential distance fog blending into the studio atmospheric sky dome + AgX tone mapping

#include "camera.slang"
#include "lighting.slang"

[[vk::binding(1, 0)]]
Sampler2DShadow g_shadow_map;

struct ScreenPush {
    uint     width;
    uint     height;
    float    cell_med;
    float    level_frac;
    float    origin_x;
    float    origin_y;
    float    zoom;
    float    coarse;
    uint     is_3d;
    uint     pad0;
    ViewPush view;
};
[[vk::push_constant]] ScreenPush pc;

float wrap_period(float x, float period) {
    float p = max(period, 1.0e-6);
    float m = x - p * floor(x / p);
    if (m < 0.0) m += p;
    if (m >= p)  m -= p;
    return m;
}

float2 slang_mod(float2 x, float y) {
    return x - y * floor(x / y);
}

float metric_grid_2d(float2 world, float2 fw, float step_size, float half_width_px) {
    float  s          = max(step_size, 1.0e-6);
    float2 half_s     = float2(s * 0.5);
    float2 dist_world = abs(slang_mod(world + half_s, s) - half_s);
    float2 dist_px    = dist_world / max(fw, float2(1.0e-7));
    float2 line       = 1.0 - smoothstep(float2(half_width_px - 0.5),
                                         float2(half_width_px + 0.5),
                                         dist_px);
    float2 density    = smoothstep(float2(2.0), float2(6.0), float2(s) / max(fw, float2(1.0e-7)));
    line *= density;
    return max(line.x, line.y);
}

// Analytical derivative anti-aliased infinite 3D ground grid (zero moire at grazing horizon)
float grid_layer_3d(float2 wp, float2 fw, float spacing, float line_px) {
    float2 coord = wp / spacing;
    float2 deriv = max(fw / spacing, float2(1.0e-5));
    float2 grid  = abs(frac(coord - 0.5) - 0.5) / deriv;
    float  dist  = min(grid.x, grid.y);
    float  mask  = 1.0 - smoothstep(line_px - 0.5, line_px + 0.5, dist);
    // Fade cleanly as cells approach Nyquist frequency on the horizon
    float  nyq   = 1.0 - smoothstep(0.18, 0.48, max(deriv.x, deriv.y));
    return mask * nyq;
}

[shader("fragment")]
float4 main(float4 frag : SV_Position) : SV_Target0 {
    float w    = max(float(pc.width),  1.0);
    float h    = max(float(pc.height), 1.0);
    float zoom = max(pc.zoom, 1.0e-8);

    // ------------------------------------------------------------------------
    // 2D Mode: Crisp Dark-Slate Anti-Aliased Drafting Grid
    // ------------------------------------------------------------------------
    if (pc.is_3d == 0u) {
        float2 ndc    = float2(frag.x / w * 2.0 - 1.0, frag.y / h * 2.0 - 1.0);
        float2 delta  = float2(ndc.x * (w * 0.5) / zoom, -ndc.y * (h * 0.5) / zoom);
        float  coarse = max(pc.coarse, 1.0e-6);
        float2 world  = float2(wrap_period(pc.origin_x + delta.x, coarse),
                               wrap_period(pc.origin_y + delta.y, coarse));
        float2 fw     = float2(1.0 / zoom);

        float fade_fine = 1.0 - smoothstep(0.0, 0.90, pc.level_frac);
        float fade_med  = lerp(0.75, 0.30, pc.level_frac);

        float g_fine   = metric_grid_2d(world, fw, pc.cell_med * 0.1, 0.60) * fade_fine * 0.25;
        float g_med    = metric_grid_2d(world, fw, pc.cell_med,       0.80) * fade_med * 0.55;
        float g_coarse = metric_grid_2d(world, fw, coarse,            1.15) * 0.85;

        float  weight   = clamp(max(max(g_fine, g_med), g_coarse), 0.0, 1.0);
        float3 base_col = float3(0.024, 0.030, 0.044);
        float3 line_col = float3(0.140, 0.215, 0.320);
        float3 col      = lerp(base_col, line_col, weight);

        // Subtle 2D origin axes
        float2 abs_w  = float2(pc.view.cam_x + delta.x, pc.view.cam_y + delta.y);
        float  axis_x = 1.0 - smoothstep(0.6, 1.6, abs(abs_w.y) * zoom);
        float  axis_y = 1.0 - smoothstep(0.6, 1.6, abs(abs_w.x) * zoom);
        col = lerp(col, float3(0.55, 0.16, 0.18), axis_x * 0.65);
        col = lerp(col, float3(0.16, 0.52, 0.24), axis_y * 0.65);
        return float4(col, 1.0);
    }

    // ------------------------------------------------------------------------
    // 3D Mode: True World-Space Studio Floor + Atmospheric Sky Dome
    // ------------------------------------------------------------------------
    float2 vxy   = float2((frag.x - w * 0.5) / zoom, (h * 0.5 - frag.y) / zoom);
    float4 q_inv = float4(-pc.view.qx, -pc.view.qy, -pc.view.qz, pc.view.qw);

    float3 ro, rd;
    if (pc.view.persp > 1.0e-7) {
        float  d_cam = 1.0 / pc.view.persp;
        float3 rd_v  = normalize(float3(vxy.x * pc.view.persp, vxy.y * pc.view.persp, -1.0));
        ro = pc.view.cam + view_rotate(q_inv, float3(0.0, 0.0, d_cam));
        rd = normalize(view_rotate(q_inv, rd_v));
    } else {
        ro = pc.view.cam + view_rotate(q_inv, float3(vxy.x, vxy.y, 65536.0));
        rd = normalize(view_rotate(q_inv, float3(0.0, 0.0, -1.0)));
    }

    // Read live lighting / shadow / studio sky parameters from BDA environment slab
    float3 sun_dir     = lighting_world_sun_dir();
    float3 sun_col     = float3(2.95, 2.72, 2.38);
    float3 sky_top     = float3(0.34, 0.52, 0.84);
    float3 sky_horizon = float3(0.62, 0.70, 0.80);
    float3 sky_ground  = float3(0.16, 0.19, 0.25);
    float  ground_z    = 0.0;
    bool   has_hdr     = false;
    bool   gi_on       = false;
    LightingShadowBlock sb = {};

    if (pc.view.materials != 0ull) {
        LightingFrameHeader *hdr = (LightingFrameHeader *)(pc.view.materials + k_lighting_header_offset);
        if (hdr->magic == k_lighting_magic) {
            has_hdr     = true;
            gi_on       = (hdr->gi_enabled != 0u);
            ground_z    = hdr->ground_z;
            sb          = ((LightingShadowBlock *)(pc.view.materials + k_lighting_shadow_offset))[0];
            sun_dir     = sb.sun_dir;
            sun_col     = sb.sun_color;
            sky_top     = sb.sky_top;
            sky_horizon = sb.sky_horizon;
            sky_ground  = sb.sky_ground;
        }
    }

    float3 sky_hdr = lighting_eval_sky_studio(rd, sun_dir, sky_top, sky_horizon, sky_ground, 0.16);

    // Ray-plane intersection with z = ground_z
    bool hit_ground = (rd.z < -0.002) && (ro.z > ground_z);
    if (!hit_ground) {
        return float4(lighting_agx_tonemap(sky_hdr), 1.0);
    }

    float  tg      = (ground_z - ro.z) / rd.z;
    float3 wp      = ro + rd * tg;
    float  cam_d   = length(wp.xy - pc.view.cam.xy);
    float  fog_d   = max(2600.0, 1800.0 / max(zoom, 0.15));
    float  fog_t   = 1.0 - exp(-pow(cam_d / fog_d, 1.55));
    float  grazing = 1.0 - smoothstep(0.002, 0.045, -rd.z);
    float  sky_mix = clamp(max(fog_t, grazing), 0.0, 1.0);

    if (sky_mix >= 0.998) {
        return float4(lighting_agx_tonemap(sky_hdr), 1.0);
    }

    // World-space derivative anti-aliased multi-scale architectural grid
    float2 fw_wp   = max(fwidth(wp.xy), float2(1.0 / max(zoom, 0.01)));
    float  g_minor = grid_layer_3d(wp.xy, fw_wp,  60.0, 0.85) * 0.28;
    float  g_major = grid_layer_3d(wp.xy, fw_wp, 300.0, 1.25) * 0.62;
    float  grid_w  = clamp(max(g_minor, g_major), 0.0, 1.0);

    // Crisp X (red) and Y (green) world origin axes
    float  ax_x = (1.0 - smoothstep(0.5, 1.6, abs(wp.y) / max(fw_wp.y, 1.0e-4))) * (1.0 - smoothstep(0.15, 0.45, fw_wp.y / 60.0));
    float  ax_y = (1.0 - smoothstep(0.5, 1.6, abs(wp.x) / max(fw_wp.x, 1.0e-4))) * (1.0 - smoothstep(0.15, 0.45, fw_wp.x / 60.0));

    // Subtle studio floor linear albedo (clean neutral warm-slate matte floor + crisp architectural lines)
    float3 floor_base = float3(0.145, 0.162, 0.195);
    float3 floor_line = float3(0.285, 0.335, 0.415);
    float3 alb_lin    = lerp(floor_base, floor_line, grid_w);
    alb_lin = lerp(alb_lin, float3(0.62, 0.14, 0.14), ax_x * 0.78);
    alb_lin = lerp(alb_lin, float3(0.14, 0.56, 0.22), ax_y * 0.78);

    float3 wnrm     = float3(0.0, 0.0, 1.0);
    float3 vnrm     = normalize(view_rotate(pc.view.q, wnrm));
    float3 view_dir = -rd;
    float  ndv      = max(dot(wnrm, view_dir), 1.0e-3);
    float  ndl      = max(dot(wnrm, sun_dir),  0.0);

    // 9-tap hardware PCF soft shadows from all 3D cubes, solids, and meshes
    float sun_vis = (has_hdr && sb.shadow_enabled != 0u)
                  ? lighting_sample_shadow(sb, g_shadow_map, wp, wnrm, frag.xy)
                  : 1.0;

    // Live 2026 SSILVB contact AO & Split HRC colored indirect bounce pools
    float  ao       = 1.0;
    float3 bent_n   = vnrm;
    float3 ind_rgb  = float3(0.0);
    bool   have_gi  = false;
    if (gi_on && pc.view.materials != 0ull) {
        float3 vp = view_of(wp, pc.view.q, pc.view.cam);
        have_gi   = lighting_sample_gi(pc.view.materials, frag.xy, vp.z, vnrm, ao, bent_n, ind_rgb);
    }

    // Cook-Torrance floor BRDF (matte architectural studio surface, roughness = 0.58)
    const float rgh  = lerp(0.56, 0.42, grid_w);
    const float3 f0  = float3(0.04);
    float3 half_v    = normalize(sun_dir + view_dir);
    float  ndh       = max(dot(wnrm, half_v), 0.0);
    float  vdh       = max(dot(view_dir, half_v), 0.0);
    float  a2        = max(rgh * rgh * rgh * rgh, 0.002);
    float  D         = lighting_d_ggx(ndh, a2);
    float  V         = lighting_v_smith_ggx_correlated(ndv, max(ndl, 1.0e-3), a2);
    float3 F_sun     = lighting_fresnel_schlick(vdh, f0);
    float3 kD        = (float3(1.0) - F_sun) * k_lighting_inv_pi;

    float3 ao_mb     = have_gi ? lighting_gtao_multibounce_gpu(ao, alb_lin) : float3(1.0);
    float  spec_occ  = have_gi ? clamp(pow(ndv + ao, exp2(-16.0 * rgh - 1.0)) - 1.0 + ao, 0.0, 1.0) : 1.0;

    float3 sky_amb   = lighting_eval_sky_studio(wnrm, sun_dir, sky_top, sky_horizon, sky_ground, 0.92);
    float3 refl_w    = reflect(-view_dir, wnrm);
    float3 env_spec  = lighting_eval_sky_studio(refl_w, sun_dir, sky_top, sky_horizon, sky_ground, rgh);
    float3 F_ibl     = f0 + (max(float3(1.0 - rgh), f0) - f0) * pow(1.0 - ndv, 5.0);

    float3 direct    = alb_lin * kD * (sun_col * (ndl * sun_vis))
                     + sun_col * (D * V * F_sun * (ndl * sun_vis) * spec_occ);
    float3 ibl       = alb_lin * sky_amb * (ao_mb * 0.62)
                     + env_spec * F_ibl * (spec_occ * 0.65);
    float3 gi_bounce = alb_lin * min(ind_rgb, float3(4.0)) * (k_lighting_inv_pi * 2.40);

    float3 floor_hdr = direct + ibl + gi_bounce;
    float3 final_hdr = lerp(floor_hdr, sky_hdr, sky_mix);
    return float4(lighting_agx_tonemap(final_hdr), 1.0);
}
