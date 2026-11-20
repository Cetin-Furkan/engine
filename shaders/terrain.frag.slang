// Slang 2026 Ultra-HD Satellite + DEM Terrain Fragment Shader:
// - Resolves 256x256 quadtree tiles via 2D Self-Relative Pointers (RelPtr2D)
// - Performs 16-tap Catmull-Rom bicubic satellite sampling + local unsharp CAS enhancement
// - Computes C1-continuous analytical bicubic DEM surface normals (zero tile border seams)
// - Synthesizes 4-octave analytical-derivative FBM micro-topography for crisp sub-meter zoom
// - Applies physically-inspired Aegean sunlight, sky hemisphere fill, canyon bounce & aerial haze.

#include "camera.slang"
import terrain_math;
import lighting;

struct TerrainPush {
    uint64_t dir_bda;
    uint64_t indirect_bda;
    uint     width;
    uint     height;
    float    elev_base_m;
    float    vertical_scale;
    float    marker_x;
    float    marker_y;
    float    marker_active;
    float    marker_b_x;
    float    marker_b_y;
    float    marker_b_active;
    float    cell_med;
    float    coarse;
    uint     flags;
    uint     pad0;
    ViewPush view;
};

[[vk::push_constant]] TerrainPush pc;

struct FSInput {
    float4   sv_pos       : SV_Position;
    float3   view_nrm     : TEXCOORD0;
    float3   world_pos    : TEXCOORD1;
    float2   tile_uv      : TEXCOORD2;
    float3   elev_span    : TEXCOORD3; // x = elev_m, y = resolved_tile_world_span, z = view_pos.z
    nointerpolation uint64_t tile_rel_ptr : TEXCOORD4;
    nointerpolation uint64_t dem_rel_ptr  : TEXCOORD5;
    float3                   dem_uv_span  : TEXCOORD6; // xy = dem_uv, z = dem_span
};

float slang_mod1(float x, float s) {
    return x - s * floor(x / s);
}

float2 slang_mod2(float2 x, float s) {
    return x - s * floor(x / s);
}

float grid_line_mask(float2 world_xy, float step_world, float width_px) {
    float s = max(step_world, 1.0e-4);
    float2 half_s = float2(s * 0.5);
    float2 dist_w = abs(slang_mod2(world_xy + half_s, s) - half_s);
    float2 fw     = max(fwidth(world_xy), float2(1.0e-5));
    float2 dist_px = dist_w / fw;
    float2 line = 1.0 - smoothstep(float2(width_px - 0.5), float2(width_px + 0.5), dist_px);
    float2 vis  = smoothstep(float2(6.0), float2(16.0), float2(s) / fw);
    line *= vis;
    return max(line.x, line.y);
}

// Screen-space derivative anti-aliased topographic contour line mask for interval step_m (meters)
float contour_line_mask(float elev_m, float step_m, float width_px) {
    float s = max(step_m, 0.1);
    float half_s = s * 0.5;
    float dist_m = abs(slang_mod1(elev_m + half_s, s) - half_s);
    float fw_e   = clamp(fwidth(elev_m), 1.0e-4, s * 0.5);
    float dist_px = dist_m / fw_e;
    float line = 1.0 - smoothstep(width_px - 0.5, width_px + 0.5, dist_px);
    // Fade out contours when lines compress closer than 3.5 screen pixels on near-vertical cliffs
    float vis  = smoothstep(3.0, 8.5, s / fw_e);
    return line * vis;
}

float3 hypsometric_tint_linear(float t) {
    float3 c0 = srgb_to_linear(float3(0.14, 0.44, 0.38)); // low valley / wetland
    float3 c1 = srgb_to_linear(float3(0.32, 0.58, 0.30)); // lower montane
    float3 c2 = srgb_to_linear(float3(0.76, 0.68, 0.30)); // mid benchland
    float3 c3 = srgb_to_linear(float3(0.74, 0.40, 0.24)); // upper slope / scree
    float3 c4 = srgb_to_linear(float3(0.86, 0.88, 0.92)); // summit / ridge
    if (t < 0.25) {
        return lerp(c0, c1, t * 4.0);
    } else if (t < 0.50) {
        return lerp(c1, c2, (t - 0.25) * 4.0);
    } else if (t < 0.78) {
        return lerp(c2, c3, (t - 0.50) / 0.28);
    }
    return lerp(c3, c4, (t - 0.78) / 0.22);
}

// Fast deterministic 2D hash in [-1, +1] for analytical derivative noise
float hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return -1.0 + 2.0 * frac((p3.x + p3.y) * p3.z);
}

// 2D Quintic Hermite Value Noise with exact analytical partial derivatives:
// returns float3(value, d_value/dx, d_value/dy)
float3 noise2d_grad(float2 x) {
    float2 i = floor(x);
    float2 f = frac(x);

    // Quintic interpolation u(f) = 6f^5 - 15f^4 + 10f^3 and derivative du(f) = 30f^4 - 60f^3 + 30f^2
    float2 u  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

    float a = hash21(i + float2(0.0, 0.0));
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));

    float k0 = a;
    float k1 = b - a;
    float k2 = c - a;
    float k3 = a - b - c + d;

    float val = k0 + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    float2 grad = du * float2(k1 + k3 * u.y, k2 + k3 * u.x);
    return float3(val, grad.x, grad.y);
}

// 3-Octave Rotated Analytical-Derivative FBM computed once & shared for normal + micro-albedo
float3 terrain_detail_fbm(float2 pos_m, float fw_m) {
    const float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);
    float3 total = float3(0.0);
    float2 p = pos_m * 0.048;
    float amp = 0.54;
    float freq = 0.048;
    float2x2 m = float2x2(1.0, 0.0, 0.0, 1.0);

    [unroll]
    for (int oct = 0; oct < 3; ++oct) {
        float octave_wavelength = 1.0 / freq;
        float band_vis = smoothstep(fw_m * 1.4, fw_m * 3.8, octave_wavelength);
        if (band_vis > 0.005) {
            float3 n = noise2d_grad(p);
            float2 d_world = mul(float2(n.y, n.z), m) * freq;
            total += float3(n.x, d_world.x, d_world.y) * (amp * band_vis);
        }
        p = mul(rot, p) * 2.25;
        m = mul(rot, m);
        freq *= 2.25;
        amp *= 0.46;
    }
    return total;
}

[shader("fragment")]
float4 main(FSInput input) : SV_Target0 {
    uint64_t tile_bda = relptr2d_resolve_gpu(pc.dir_bda, input.tile_rel_ptr);
    uint64_t dem_bda  = (input.dem_rel_ptr != 0ull && input.dem_rel_ptr != input.tile_rel_ptr)
                      ? relptr2d_resolve_gpu(pc.dir_bda, input.dem_rel_ptr)
                      : tile_bda;
    float4 img_srgb   = float4(0.45, 0.48, 0.36, 0.88);
    float3 world_nrm  = float3(0.0, 0.0, 1.0);
    float  elev_m     = input.elev_span.x;
    float2 dem_uv     = input.dem_uv_span.xy;
    float  dem_span   = max(input.dem_uv_span.z, 0.25);

    uint   k_mode     = pc.flags & 3u;
    bool   real_map   = (pc.flags & 4u) != 0u;

    float2 fw_xy = fwidth(input.world_pos.xy);
    float fw_m   = max(length(fw_xy), 0.02);

    // 1. Sample Committed Full-Resolution Bird's-Eye Orthophoto Layers from GPU BDA Arena
    float3 ortho_srgb        = float3(0.0);
    float  ortho_coverage    = 0.0;
    uint   committed_layers  = load_committed_ortho_count(pc.dir_bda);
    for (uint li = 0u; li < committed_layers; ++li) {
        GpuMathOrthoOverlayGpu layer = load_committed_ortho_layer(pc.dir_bda, li);
        if (layer.active == 0u || layer.pixels_bda == 0ull || layer.width_px == 0u || layer.height_px == 0u) {
            continue;
        }
        float2 l_half = max(float2(layer.half_span_x, layer.half_span_y), float2(1.0));
        float2 l_rel  = (input.world_pos.xy - float2(layer.center_x, layer.center_y)) / l_half;
        if (abs(l_rel.x) <= 1.0 && abs(l_rel.y) <= 1.0) {
            float2 l_uv      = float2(0.5 * (l_rel.x + 1.0), 0.5 * (1.0 - l_rel.y));
            float4 l_sample  = overlay_sample_bicubic_rgba(layer.pixels_bda, layer.width_px, layer.height_px, l_uv);
            float  edge_dist = min(1.0 - abs(l_rel.x), 1.0 - abs(l_rel.y));
            float  feather   = smoothstep(0.0, 0.02, edge_dist);
            float  l_alpha   = saturate(layer.opacity * l_sample.a * feather);
            ortho_srgb       = lerp(ortho_srgb, l_sample.rgb, l_alpha);
            ortho_coverage   = saturate(ortho_coverage + l_alpha * (1.0 - ortho_coverage));
        }
    }

    // 2. Sample Interactive Bird's-Eye Orthophoto Alignment Preview (50% default transparency, O/P opacity, WASD move, 0.5% 2D stretch)
    GpuMathOrthoOverlayGpu ov = load_ortho_overlay(pc.dir_bda);
    float  ov_border_mask = 0.0;
    float  ov_corner_mask = 0.0;
    float3 preview_srgb   = float3(0.0);
    float  preview_alpha  = 0.0;
    if (ov.active != 0u && ov.pixels_bda != 0ull && ov.width_px > 0u && ov.height_px > 0u) {
        float2 half_span = max(float2(ov.half_span_x, ov.half_span_y), float2(1.0));
        float2 d_xy = input.world_pos.xy - float2(ov.center_x, ov.center_y);
        float2 rel  = d_xy / half_span;
        if (abs(rel.x) <= 1.0 && abs(rel.y) <= 1.0) {
            float2 ov_uv   = float2(0.5 * (rel.x + 1.0), 0.5 * (1.0 - rel.y));
            float4 ov_srgb = overlay_sample_bicubic_rgba(ov.pixels_bda, ov.width_px, ov.height_px, ov_uv);
            preview_srgb   = ov_srgb.rgb;
            preview_alpha  = saturate(ov.opacity * ov_srgb.a);
        }
        // Screen-space anti-aliased bird's-eye alignment border & corner brackets
        float2 dist_edge_m = abs(abs(d_xy) - half_span);
        float  d_border_m  = (abs(rel.x) <= 1.02 && abs(rel.y) <= 1.02)
                           ? min(dist_edge_m.x, dist_edge_m.y)
                           : 1.0e6;
        float  bw_m = clamp(fw_m * 1.8, 1.5, 22.0);
        ov_border_mask = 1.0 - smoothstep(bw_m * 0.4, bw_m * 1.3, d_border_m);
        float  corner_span_m = min(half_span.x, half_span.y) * 0.14;
        if (abs(d_xy.x) >= half_span.x - corner_span_m &&
            abs(d_xy.y) >= half_span.y - corner_span_m &&
            abs(rel.x) <= 1.02 && abs(rel.y) <= 1.02) {
            ov_corner_mask = 1.0 - smoothstep(bw_m * 0.8, bw_m * 2.2, d_border_m);
        }
        // Subtle center reticle
        float d_center_m = length(d_xy);
        float r_ret = clamp(fw_m * 8.0, 8.0, 60.0);
        float center_cross = (1.0 - smoothstep(bw_m * 0.4, bw_m * 1.1, min(abs(d_xy.x), abs(d_xy.y)))) *
                             (1.0 - smoothstep(r_ret * 0.8, r_ret * 1.2, d_center_m));
        ov_border_mask = max(ov_border_mask, center_cross * 0.75);
    }

    float hi_res_photo_mask = max(ortho_coverage, preview_alpha);

    if (tile_bda != 0ull) {
        float sharp_vis = (1.0 - smoothstep(1.0, 6.0, fw_m)) * (1.0 - ortho_coverage);
        if (fw_m < 6.0) {
            // Close-up Ultra-HD view: 16-tap Catmull-Rom bicubic + fused 5-tap CAS
            img_srgb = tile_sample_bicubic_imagery(tile_bda, input.tile_uv, (real_map ? 0.38 : 0.48) * sharp_vis);
        } else {
            // Mid/Far view: fast single-BC1-block bilinear sample
            img_srgb = tile_sample_bilinear_imagery(tile_bda, input.tile_uv);
        }
    }

    float3 cached_fbm   = float3(0.0);
    bool   have_fbm     = false;

    if (dem_bda != 0ull) {
        if (dem_bda != tile_bda) {
            img_srgb.a = tile_sample_bilinear_ao(dem_bda, dem_uv);
        }

        // Adaptive analytical DEM surface normal: 16-tap C1 bicubic when zoomed in, 4-tap bilinear at mid/far
        float2 dh_duv = float2(0.0);
        if (fw_m < 8.0) {
            elev_m = tile_sample_bicubic_height_grad_m(dem_bda, dem_uv, dh_duv);
        } else {
            elev_m = tile_sample_bilinear_height_grad_m(dem_bda, dem_uv, dh_duv);
        }
        float dzdx = (dh_duv.x / dem_span) * pc.vertical_scale;
        float dzdy = (dh_duv.y / dem_span) * pc.vertical_scale;

        // Single-evaluation analytical-derivative FBM shared across normal & micro-albedo
        float synth_detail_w = real_map ? 0.0 : (1.0 - hi_res_photo_mask);
        if (synth_detail_w > 0.01 && fw_m < 14.0) {
            float slope_raw = saturate(1.0 - 1.0 / sqrt(1.0 + dzdx * dzdx + dzdy * dzdy));
            cached_fbm = terrain_detail_fbm(input.world_pos.xy, fw_m);
            have_fbm   = true;
            float micro_amp = lerp(1.1, 4.4, smoothstep(0.05, 0.32, slope_raw)) * synth_detail_w;
            dzdx += cached_fbm.y * micro_amp;
            dzdy += cached_fbm.z * micro_amp;

            // Horizontal karst limestone stratification on steep slopes
            float strata_vis = smoothstep(0.12, 0.38, slope_raw) * synth_detail_w;
            float strata_dh  = cos(elev_m * 0.42 + cached_fbm.x * 1.8) * 0.16 * strata_vis;
            dzdx += strata_dh;
            dzdy += strata_dh;
        }
        world_nrm = normalize(float3(-dzdx, -dzdy, 1.0));
    }

    // Composite full-resolution committed ortho layers and live preview onto base tile imagery
    if (ortho_coverage > 0.0) {
        img_srgb.rgb = lerp(img_srgb.rgb, ortho_srgb, ortho_coverage);
    }
    if (preview_alpha > 0.0) {
        img_srgb.rgb = lerp(img_srgb.rgb, preview_srgb, preview_alpha);
    }

    // Convert sRGB satellite color to linear light so VK_FORMAT_B8G8R8A8_SRGB
    // hardware encoding displays accurate, rich, unwashed satellite colors
    float3 sat_linear = srgb_to_linear(img_srgb.rgb);

    // Reuse cached FBM for subtle close-up rock & soil albedo variation (zero extra FBM calls!)
    float close_vis = real_map ? 0.0 : (1.0 - smoothstep(0.8, 12.0, fw_m)) * (1.0 - hi_res_photo_mask);
    if (have_fbm && close_vis > 0.01) {
        float slope      = saturate(1.0 - world_nrm.z);
        float steep_mask = smoothstep(0.08, 0.34, slope);
        float micro_mod  = 1.0 + cached_fbm.x * (0.16 * close_vis + 0.16 * steep_mask * close_vis);
        sat_linear *= clamp(micro_mod, 0.74, 1.26);
    }

    // World-space directional sunlight (warm Aegean sun from SW-W at 46 deg elevation)
    const float3 sun_dir  = normalize(float3(-0.50, -0.52, 0.69));
    const float3 fill_dir = normalize(float3(0.55, 0.45, 0.70));
    float ndl  = max(dot(world_nrm, sun_dir), 0.0);
    float fill = max(dot(world_nrm, fill_dir), 0.0);
    float ao   = lerp(0.72, 1.05, img_srgb.a);

    // Balanced physically-inspired daylight + 2026 SSILVB canyon AO & Split HRC cliff-to-valley GI bounce
    float3 sun_light   = float3(1.06, 0.99, 0.92) * (0.68 * ndl);
    float3 sky_fill    = float3(0.42, 0.48, 0.56) * (0.44 + 0.22 * world_nrm.z + 0.18 * fill);
    float3 bounce_fill = float3(0.32, 0.28, 0.22) * (0.14 * saturate(1.0 - world_nrm.z));

    float  gi_ao       = 1.0;
    float3 gi_bent     = normalize(input.view_nrm);
    float3 gi_ind_rgb  = float3(0.0);
    float3 vnrm_cur    = normalize(view_rotate(pc.view.q, world_nrm));
    if (lighting_sample_gi(pc.view.materials, input.sv_pos.xy, input.elev_span.z, vnrm_cur,
                           gi_ao, gi_bent, gi_ind_rgb)) {
        ao          *= lerp(0.52, 1.0, gi_ao);
        bounce_fill  = bounce_fill * 0.35 + gi_ind_rgb * 0.85;
    }

    float3 lit_color;
    if (real_map) {
        float  relief_ao = lerp(0.76, 1.04, img_srgb.a) * lerp(0.72, 1.0, gi_ao);
        float3 hill      = float3(0.46, 0.50, 0.55) * relief_ao
                         + float3(0.68, 0.65, 0.60) * (ndl * lerp(0.82, 1.0, gi_ao));
        lit_color = sat_linear * clamp(hill, float3(0.32), float3(1.18));
    } else {
        float3 synth_lit = sat_linear * (sky_fill * ao + sun_light * lerp(0.75, 1.0, gi_ao) + bounce_fill);
        float3 photo_lit = sat_linear * lerp(0.95, 1.05, ndl);
        lit_color = lerp(synth_lit, photo_lit, hi_res_photo_mask);
    }

    // Subtle aerial perspective haze at extreme view distances (> 12 km) in 3D oblique views
    float cam_dist_m = length(input.world_pos.xy - pc.view.cam.xy);
    float haze = smoothstep(9000.0, 26000.0, cam_dist_m) * 0.16;
    lit_color = lerp(lit_color, float3(0.52, 0.62, 0.74), haze);

    // Whisper-subtle metric grid squares (10m, 100m, 1km) projected onto the terrain
    float g_med    = grid_line_mask(input.world_pos.xy, max(pc.cell_med, 10.0), 0.55) * 0.04;
    float g_coarse = grid_line_mask(input.world_pos.xy, max(pc.coarse, 100.0), 0.85) * 0.08;
    float g_weight = max(g_med, g_coarse) * (real_map ? 0.35 : (1.0 - 0.85 * hi_res_photo_mask));
    lit_color = lerp(lit_color, lit_color + float3(0.24, 0.28, 0.32), g_weight);

    // Topographic Overlay Modes (K key cycle: 0=Off, 1=Zoom-Adaptive Precision Contours, 2=Hypsometric + Contours):
    if (k_mode != 0u) {
        if (k_mode == 2u) {
            float2 rng = dir_elev_range_m(pc.dir_bda);
            float  t_h = saturate((elev_m - rng.x) / max(rng.y - rng.x, 10.0));
            float3 hypso = hypsometric_tint_linear(t_h) * (0.58 + 0.55 * ndl);
            lit_color = lerp(lit_color, hypso, 0.38);
        }

        // Zoom-adaptive intervals with dark contrast halos (reads over snow, canopy, rock, water)
        float w_05 = 1.0 - smoothstep(0.55, 1.80, fw_m);
        float w_1  = 1.0 - smoothstep(1.40, 3.80, fw_m);
        float w_5  = 1.0 - smoothstep(5.00, 12.5, fw_m);

        float c_05  = contour_line_mask(elev_m,   0.5, 0.55) * w_05;
        float c_1   = contour_line_mask(elev_m,   1.0, 0.65) * w_1;
        float c_5   = contour_line_mask(elev_m,   5.0, 0.72) * w_5;
        float c_20  = contour_line_mask(elev_m,  20.0, 0.80);
        float c_100 = contour_line_mask(elev_m, 100.0, 1.35);

        float h_5   = contour_line_mask(elev_m,   5.0, 1.45) * w_5;
        float h_20  = contour_line_mask(elev_m,  20.0, 1.65);
        float h_100 = contour_line_mask(elev_m, 100.0, 2.35);

        float halo  = max(h_5 * 0.28, max(h_20 * 0.42, h_100 * 0.62));
        lit_color = lerp(lit_color, float3(0.02, 0.03, 0.04), halo);

        lit_color = lerp(lit_color, srgb_to_linear(float3(0.62, 0.90, 0.96)), c_05  * 0.34);
        lit_color = lerp(lit_color, srgb_to_linear(float3(0.76, 0.95, 0.86)), c_1   * 0.42);
        lit_color = lerp(lit_color, srgb_to_linear(float3(0.95, 0.82, 0.46)), c_5   * 0.48);
        lit_color = lerp(lit_color, srgb_to_linear(float3(0.96, 0.66, 0.22)), c_20  * 0.58);
        lit_color = lerp(lit_color, srgb_to_linear(float3(1.00, 0.90, 0.38)), c_100 * 0.86);
    }

    // Two-Point Geodesic Distance & Elevation Profile Line Segment (Point A -> Point B)
    if (pc.marker_active > 0.5 && pc.marker_b_active > 0.5) {
        float2 pa = float2(pc.marker_x, pc.marker_y);
        float2 pb = float2(pc.marker_b_x, pc.marker_b_y);
        float2 ab = pb - pa;
        float  ab_len2 = dot(ab, ab);
        if (ab_len2 > 1.0) {
            float  ab_len = sqrt(ab_len2);
            float  t_seg  = clamp(dot(input.world_pos.xy - pa, ab) / ab_len2, 0.0, 1.0);
            float2 closest = pa + ab * t_seg;
            float  d_line = length(input.world_pos.xy - closest);
            float  line_w = clamp(fw_m * 2.2, 2.5, 28.0);
            float  core   = 1.0 - smoothstep(line_w * 0.35, line_w * 0.95, d_line);
            float  glow   = 1.0 - smoothstep(line_w * 0.80, line_w * 2.60, d_line);

            // 100-meter geodesic tick marks along the profile line
            float  along_m = t_seg * ab_len;
            float  tick_d  = abs(slang_mod1(along_m + 50.0, 100.0) - 50.0);
            float  tick    = (1.0 - smoothstep(fw_m * 0.8, fw_m * 1.8, tick_d)) *
                             (1.0 - smoothstep(line_w * 1.2, line_w * 2.2, d_line));

            float3 line_col = lerp(float3(1.0, 0.38, 0.08), float3(0.12, 0.88, 1.0), t_seg);
            lit_color = lerp(lit_color, line_col, glow * 0.35);
            lit_color = lerp(lit_color, float3(1.0, 0.96, 0.78), max(core * 0.88, tick * 0.75));
        }
    }

    // Interactive click survey beacon A (orange-red) on the terrain (scaled in meters)
    if (pc.marker_active > 0.5) {
        float d = length(input.world_pos.xy - float2(pc.marker_x, pc.marker_y));
        float r_base = clamp(fw_m * 14.0, 18.0, 140.0);
        float ring = 1.0 - smoothstep(r_base * 0.12, r_base * 0.28, abs(d - r_base));
        float dot_center = 1.0 - smoothstep(0.0, r_base * 0.24, d);
        float beacon = max(ring, dot_center);
        lit_color = lerp(lit_color, float3(1.0, 0.26, 0.06), beacon * 0.92);
    }

    // Second survey beacon B (cyan-blue) for two-point geodesic profile measurement
    if (pc.marker_b_active > 0.5) {
        float d = length(input.world_pos.xy - float2(pc.marker_b_x, pc.marker_b_y));
        float r_base = clamp(fw_m * 14.0, 18.0, 140.0);
        float ring = 1.0 - smoothstep(r_base * 0.12, r_base * 0.28, abs(d - r_base));
        float dot_center = 1.0 - smoothstep(0.0, r_base * 0.24, d);
        float beacon = max(ring, dot_center);
        lit_color = lerp(lit_color, float3(0.08, 0.82, 1.0), beacon * 0.92);
    }

    // Bird's-Eye Orthophoto Alignment Frame (emerald-cyan perimeter + warm gold corner brackets)
    if (ov_border_mask > 0.01) {
        lit_color = lerp(lit_color, float3(0.10, 0.95, 0.78), ov_border_mask * 0.82);
    }
    if (ov_corner_mask > 0.01) {
        lit_color = lerp(lit_color, float3(1.00, 0.86, 0.18), ov_corner_mask * 0.95);
    }

    return float4(max(lit_color, float3(0.0)), 1.0);
}
