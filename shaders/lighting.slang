// Shared Slang 2026 Physically-Based Direct + Hardware Shadow + Global Illumination Module.
// Composed from:
//   - lighting_types.slang     : Constants, BDA structs, interfaces, packing & camera transforms
//   - lighting_proxy.slang     : Analytical OBB/Ellipsoid ray proxies & studio sky dome
//   - lighting_gi_shadow.slang : Joint bilateral SSILVB/HRC GI upsampler & 9-tap Vogel-disk PCF shadows
//   - lighting.slang (here)    : Cook-Torrance Metallic-Roughness BRDF, Debug Views & shading entrypoints

#include "lighting_types.slang"
#include "lighting_proxy.slang"
#include "lighting_gi_shadow.slang"

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
    float t  = 1.0 - clamp(cos_theta, 0.0, 1.0);
    float t2 = t * t;
    return f0 + (float3(1.0) - f0) * (t2 * t2 * t);
}

// Hash-based vivid diagnostic palette for meshlet / entity cluster visualization (returns linear RGB)
float3 lighting_debug_cluster_color(uint id) {
    uint h = id * 0x9E3779B1u;
    h ^= h >> 16u;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13u;
    float hue   = float(h & 0xFFFFu) * (1.0 / 65536.0);
    float sat   = 0.62 + 0.28 * float((h >> 16u) & 0xFFu) * (1.0 / 255.0);
    float val   = 0.68 + 0.26 * float((h >> 24u) & 0xFFu) * (1.0 / 255.0);
    float3 k    = abs(frac(float3(hue) + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    float3 srgb = val * lerp(float3(1.0), clamp(k - 1.0, float3(0.0), float3(1.0)), sat);
    return lighting_srgb_to_linear(srgb);
}

// Smooth perceptual ramp for roughness [0 = mirror teal, 0.5 = amber, 1.0 = matte magenta-white]
float3 lighting_debug_roughness_ramp(float r) {
    float  t    = clamp(r, 0.0, 1.0);
    float3 c0   = float3(0.06, 0.72, 0.92);
    float3 c1   = float3(0.95, 0.74, 0.18);
    float3 c2   = float3(0.86, 0.22, 0.42);
    float3 srgb = (t < 0.5) ? lerp(c0, c1, t * 2.0) : lerp(c1, c2, (t - 0.5) * 2.0);
    return lighting_srgb_to_linear(srgb);
}

// View-space depth diagnostic ramp (works seamlessly for both reverse-Z perspective & ortho)
float3 lighting_debug_depth_ramp(float view_z, float persp, float zoom) {
    float  dist   = (persp > 1.0e-7) ? max((1.0 / persp) - view_z, 0.0)
                                     : max(900.0 / max(zoom, 0.05) - view_z, 0.0);
    float  scale  = (persp > 1.0e-7) ? (1.0 / max(persp, 1.0e-5)) : (1200.0 / max(zoom, 0.05));
    float  d      = 1.0 / (1.0 + dist / max(scale * 0.65, 1.0));
    float3 far_c  = float3(0.05, 0.08, 0.24);
    float3 mid_c  = float3(0.10, 0.68, 0.74);
    float3 near_c = float3(0.98, 0.92, 0.58);
    float3 srgb   = (d < 0.5) ? lerp(far_c, mid_c, d * 2.0) : lerp(mid_c, near_c, (d - 0.5) * 2.0);
    return lighting_srgb_to_linear(srgb);
}

// Core Physically-Based Metallic-Roughness Surface Shading + Diagnostic Debug Views
float3 shade_surface_pbr_core(
    float3   n_in,
    float3   alb_lin_in,
    float3   emit_lin_in,
    float    rgh_in,
    float    metallic_in,
    float    mat_occ_in,
    float    ior_in,
    bool     unlit,
    bool     is_pbr,
    bool     use_gi,
    bool     use_shadow,
    float    sun_vis_in,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda,
    float3   world_pos_override,
    bool     have_world_pos,
    uint     cluster_id)
{
    float3 n        = normalize(n_in);
    float  rgh      = clamp(rgh_in, 0.045, 0.99);
    float  metallic = clamp(metallic_in, 0.0, 1.0);
    float  mat_occ  = clamp(mat_occ_in, 0.0, 1.0);
    float3 alb_lin  = max(alb_lin_in, float3(0.0));
    float3 emit_lin = max(emit_lin_in, float3(0.0));

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

    uint debug_view = has_hdr ? ((hdr.scene_flags >> k_lighting_debug_shift) & 0xFu) : 0u;

    float  ao          = 1.0;
    float3 bent_n      = n;
    float3 indirect_gi = float3(0.0);
    bool   have_gi     = false;
    if (use_gi && has_hdr && hdr.gi_enabled != 0u) {
        have_gi = lighting_sample_gi(env_bda, sv_pos_xy, view_z, n, ao, bent_n, indirect_gi);
    }
    float total_ao = ao * mat_occ;

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

    if (debug_view != k_lighting_debug_lit) {
        if (debug_view == k_lighting_debug_albedo)    return alb_lin;
        if (debug_view == k_lighting_debug_normals)   return lighting_srgb_to_linear(eval_n * 0.5 + 0.5);
        if (debug_view == k_lighting_debug_roughness) return lighting_debug_roughness_ramp(rgh);
        if (debug_view == k_lighting_debug_depth)     return lighting_debug_depth_ramp(view_z, sb.persp, hdr.zoom);
        if (debug_view == k_lighting_debug_gi) {
            float3 gi_vis = indirect_gi / (float3(1.0) + indirect_gi);
            return clamp(gi_vis, float3(0.0), float3(1.0));
        }
        if (debug_view == k_lighting_debug_ao)        return lighting_srgb_to_linear(float3(total_ao));
        if (debug_view == k_lighting_debug_shadow)    return lighting_srgb_to_linear(float3(sun_vis));
        if (debug_view == k_lighting_debug_meshlets) {
            float shade = 0.55 + 0.45 * max(dot(eval_n, sun_dir), 0.0);
            return lighting_debug_cluster_color(cluster_id != 0u ? cluster_id : 1u) * shade;
        }
    }

    if (unlit) {
        return max(alb_lin + emit_lin, float3(0.0));
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

    // Cook-Torrance Metallic-Roughness microfacet BRDF with KHR_materials_ior
    float  ior_c    = clamp(ior_in, 1.0, 4.0);
    float  ior_r    = (ior_c - 1.0) / (ior_c + 1.0);
    float  f0_d     = ior_r * ior_r;
    float3 f0       = lerp(float3(f0_d), alb_lin, metallic);
    float3 diff_alb = alb_lin * (1.0 - metallic);

    float  alpha = rgh * rgh;
    float  a2    = max(alpha * alpha, 0.0012);
    float  D     = lighting_d_ggx(ndh, a2);
    float  V     = lighting_v_smith_ggx_correlated(ndv, max(ndl, 1.0e-3), a2);
    float3 F_sun = lighting_fresnel_schlick(vdh, f0);
    float3 kD    = (float3(1.0) - F_sun) * k_lighting_inv_pi;

    // Lagarde / de Rousiers physically-based specular occlusion from VBAO + baked AO
    float spec_occ = (have_gi || mat_occ < 0.999)
        ? clamp(pow(ndv + total_ao, exp2(-16.0 * rgh - 1.0)) - 1.0 + total_ao, 0.0, 1.0)
        : 1.0;

    float3 direct_diff = diff_alb * kD * (sun_col * sun_term + fill_col * (ndfill * 0.42 * lerp(0.40, 1.0, total_ao)));
    float3 direct_spec = sun_col * (D * V * F_sun * sun_term * spec_occ);

    // Analytic sky / horizon / ground-bounce diffuse IBL modulated by Jimenez multi-bounce AO
    float3 sky_diff_irr = lighting_eval_sky_studio(eval_bent, sun_dir, sky_top, sky_horizon, sky_ground, 0.92);
    float3 ao_mb        = (have_gi || mat_occ < 0.999) ? lighting_gtao_multibounce_gpu(total_ao, alb_lin) : float3(1.0);
    float3 ibl_diff     = diff_alb * sky_diff_irr * (ao_mb * 0.62);

    // Analytic roughness-prefiltered specular IBL (metals gain full colored Fresnel sky reflections)
    float3 refl_w   = reflect(-view_dir, eval_n);
    float3 env_spec = lighting_eval_sky_studio(refl_w, sun_dir, sky_top, sky_horizon, sky_ground, rgh);
    float3 F_ibl    = f0 + (max(float3(1.0 - rgh), f0) - f0) * pow(1.0 - ndv, 5.0);
    float3 ibl_spec = env_spec * F_ibl * (spec_occ * lerp(0.78, 1.18, metallic));

    // Multi-bounce SSILVB + Split HRC indirect diffuse radiance
    float3 gi_diff = diff_alb * indirect_gi * (k_lighting_inv_pi * 2.10);

    // HDR emissive radiance (exact physical emissive for glTF/Blender; Fresnel lift for legacy glow)
    float3 emit_term = is_pbr ? emit_lin : (emit_lin * (2.40 + 0.85 * pow(1.0 - ndv, 3.0)));

    float3 hdr_col = direct_diff + direct_spec + ibl_diff + ibl_spec + gi_diff + emit_term;
    return max(hdr_col, float3(0.0));
}

float3 shade_surface_core<S : IShadedSurface>(
    S        surf,
    bool     use_gi,
    bool     use_shadow,
    float    sun_vis_in,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda,
    float3   world_pos_override,
    bool     have_world_pos,
    uint     cluster_id)
{
    return shade_surface_pbr_core(
        surf.normal,
        lighting_srgb_to_linear(surf.albedo),
        surf.emissive,
        clamp(surf.roughness, 0.08, 0.98),
        0.0, 1.0, 1.5, false, false,
        use_gi, use_shadow, sun_vis_in,
        sv_pos_xy, view_z, env_bda,
        world_pos_override, have_world_pos, cluster_id);
}

float3 shade_pbr_gi_shadow_cluster(
    PbrSurfaceFragment surf,
    float2             sv_pos_xy,
    float              view_z,
    uint64_t           env_bda,
    Sampler2DShadow    shadow_map,
    uint               cluster_id)
{
    float3 wpos;
    bool   have_wp;
    float  sun_vis;
    lighting_eval_world_shadow(surf.nrm, sv_pos_xy, view_z, env_bda, shadow_map, wpos, have_wp, sun_vis);
    return shade_surface_pbr_core(
        surf.nrm, surf.alb_lin, surf.emi_lin, surf.rgh,
        surf.metallic, surf.occlusion, surf.ior, surf.unlit, true,
        true, true, sun_vis,
        sv_pos_xy, view_z, env_bda,
        wpos, have_wp, cluster_id);
}

float3 shade_surface_gi_shadow_cluster<S : IShadedSurface>(
    S               surf,
    float2          sv_pos_xy,
    float           view_z,
    uint64_t        env_bda,
    Sampler2DShadow shadow_map,
    uint            cluster_id)
{
    float3 wpos;
    bool   have_wp;
    float  sun_vis;
    lighting_eval_world_shadow(surf.normal, sv_pos_xy, view_z, env_bda, shadow_map, wpos, have_wp, sun_vis);
    return shade_surface_core(surf, true, true, sun_vis, sv_pos_xy, view_z, env_bda, wpos, have_wp, cluster_id);
}

float3 shade_surface_gi_shadow<S : IShadedSurface>(
    S               surf,
    float2          sv_pos_xy,
    float           view_z,
    uint64_t        env_bda,
    Sampler2DShadow shadow_map)
{
    return shade_surface_gi_shadow_cluster(surf, sv_pos_xy, view_z, env_bda, shadow_map, 0u);
}

float3 shade_surface_gi<S : IShadedSurface>(
    S        surf,
    float2   sv_pos_xy,
    float    view_z,
    uint64_t env_bda)
{
    return shade_surface_core(surf, true, false, 1.0, sv_pos_xy, view_z, env_bda, float3(0.0), false, 0u);
}

float3 shade_surface<S : IShadedSurface>(S surf) {
    return shade_surface_core(surf, false, false, 1.0, float2(0.0), 0.0, 0ull, float3(0.0), false, 0u);
}
