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
    uint     width;
    uint     height;
    float    elev_base_m;
    float    vertical_scale;
    float    marker_x;
    float    marker_y;
    float    marker_active;
    float    cell_med;
    float    coarse;
    uint     pad0;
    ViewPush view;
};

[[vk::push_constant]] TerrainPush pc;

struct FSInput {
    float4   sv_pos       : SV_Position;
    float3   view_nrm     : TEXCOORD0;
    float3   world_pos    : TEXCOORD1;
    float2   tile_uv      : TEXCOORD2;
    float2   elev_span    : TEXCOORD3; // x = elev_m, y = resolved_tile_world_span
    nointerpolation uint64_t tile_rel_ptr : TEXCOORD4;
    nointerpolation uint     tile_lod     : TEXCOORD5;
};

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

// 4-Octave Rotated Analytical-Derivative FBM for sub-meter mountain & ground micro-detail
float3 terrain_detail_fbm(float2 pos_m, float fw_m) {
    const float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);
    float3 total = float3(0.0);
    float2 p = pos_m * 0.045; // Base wavelength ~22 meters
    float amp = 0.52;
    float freq = 0.045;
    float2x2 m = float2x2(1.0, 0.0, 0.0, 1.0);

    [unroll]
    for (int oct = 0; oct < 4; ++oct) {
        // Band-limit octaves against screen pixel footprint fw_m to prevent aliasing
        float octave_wavelength = 1.0 / freq;
        float band_vis = smoothstep(fw_m * 1.4, fw_m * 3.8, octave_wavelength);
        float3 n = noise2d_grad(p);
        float2 d_world = mul(float2(n.y, n.z), m) * freq;
        total += float3(n.x, d_world.x, d_world.y) * (amp * band_vis);
        p = mul(rot, p) * 2.15;
        m = mul(rot, m);
        freq *= 2.15;
        amp *= 0.47;
    }
    return total;
}

[shader("fragment")]
float4 main(FSInput input) : SV_Target0 {
    uint64_t tile_bda = relptr2d_resolve_gpu(pc.dir_bda, input.tile_rel_ptr);
    float4 img_srgb = float4(0.45, 0.48, 0.36, 0.88);
    float3 world_nrm = float3(0.0, 0.0, 1.0);
    float elev_m = input.elev_span.x;
    float tile_span = max(input.elev_span.y, 1.0);

    float2 fw_xy = fwidth(input.world_pos.xy);
    float fw_m   = max(length(fw_xy), 0.05);

    if (tile_bda != 0ull) {
        // 16-tap Catmull-Rom bicubic satellite imagery sample
        img_srgb = tile_sample_bicubic_imagery(tile_bda, input.tile_uv);

        // High-frequency Contrast Adaptive Sharpening (CAS) from neighboring texels when zoomed in
        float texel_step = 0.85 / float(k_tile_dim - 1u);
        float sharp_vis  = 1.0 - smoothstep(1.5, 16.0, fw_m);
        if (sharp_vis > 0.02) {
            float3 c_n = tile_sample_bicubic_imagery(tile_bda, input.tile_uv + float2(0.0, texel_step)).rgb;
            float3 c_s = tile_sample_bicubic_imagery(tile_bda, input.tile_uv - float2(0.0, texel_step)).rgb;
            float3 c_e = tile_sample_bicubic_imagery(tile_bda, input.tile_uv + float2(texel_step, 0.0)).rgb;
            float3 c_w = tile_sample_bicubic_imagery(tile_bda, input.tile_uv - float2(texel_step, 0.0)).rgb;
            float3 blur = 0.25 * (c_n + c_s + c_e + c_w);
            img_srgb.rgb = clamp(img_srgb.rgb + (img_srgb.rgb - blur) * (0.48 * sharp_vis), float3(0.0), float3(1.0));
        }

        // Exact C1-continuous analytical bicubic DEM surface normal (zero tile-edge seams)
        float2 dh_duv = float2(0.0);
        elev_m = tile_sample_bicubic_height_grad_m(tile_bda, input.tile_uv, dh_duv);
        float dzdx = (dh_duv.x / tile_span) * pc.vertical_scale;
        float dzdy = (dh_duv.y / tile_span) * pc.vertical_scale;

        // Fuse multi-octave analytical-derivative FBM micro-relief for crisp close-up HD detail
        float slope_raw = saturate(1.0 - 1.0 / sqrt(1.0 + dzdx * dzdx + dzdy * dzdy));
        float3 fbm = terrain_detail_fbm(input.world_pos.xy, fw_m);
        float micro_amp = lerp(1.1, 4.8, smoothstep(0.05, 0.32, slope_raw));
        dzdx += fbm.y * micro_amp;
        dzdy += fbm.z * micro_amp;

        // Horizontal karst limestone stratification on steep Beşparmak Dağı cliffs
        float strata_vis = smoothstep(0.12, 0.38, slope_raw);
        float strata_dh  = cos(elev_m * 0.42 + fbm.x * 1.8) * 0.16 * strata_vis;
        world_nrm = normalize(float3(-(dzdx + strata_dh), -(dzdy + strata_dh), 1.0));
    }

    // Convert sRGB satellite color to linear light so VK_FORMAT_B8G8R8A8_SRGB
    // hardware encoding displays accurate, rich, unwashed satellite colors
    float3 sat_linear = srgb_to_linear(img_srgb.rgb);

    // Modulate close-up rock & ground micro-albedo so zoomed-in surfaces look razor-sharp
    float slope = saturate(1.0 - world_nrm.z);
    float steep_mask = smoothstep(0.08, 0.34, slope);
    float3 fbm_close = terrain_detail_fbm(input.world_pos.xy * 3.2, fw_m * 3.2);
    float close_vis  = 1.0 - smoothstep(0.8, 14.0, fw_m);
    float micro_mod  = 1.0 + fbm_close.x * (0.14 * close_vis + 0.16 * steep_mask);
    sat_linear *= clamp(micro_mod, 0.78, 1.24);

    // World-space directional sunlight (warm Aegean sun from SW-W at 46 deg elevation)
    const float3 sun_dir  = normalize(float3(-0.50, -0.52, 0.69));
    const float3 fill_dir = normalize(float3(0.55, 0.45, 0.70));
    float ndl  = max(dot(world_nrm, sun_dir), 0.0);
    float fill = max(dot(world_nrm, fill_dir), 0.0);
    float ao   = lerp(0.72, 1.05, img_srgb.a);

    // Balanced physically-inspired daylight illumination (sun + sky dome + warm canyon bounce)
    float3 sun_light   = float3(1.06, 0.99, 0.92) * (0.68 * ndl);
    float3 sky_fill    = float3(0.42, 0.48, 0.56) * (0.44 + 0.22 * world_nrm.z + 0.18 * fill);
    float3 bounce_fill = float3(0.32, 0.28, 0.22) * (0.14 * saturate(1.0 - world_nrm.z));
    float3 lit_color   = sat_linear * (sky_fill + sun_light + bounce_fill) * ao;

    // Subtle aerial perspective haze at extreme view distances (> 12 km) in 3D oblique views
    float cam_dist_m = length(input.world_pos.xy - pc.view.cam.xy);
    float haze = smoothstep(9000.0, 26000.0, cam_dist_m) * 0.16;
    lit_color = lerp(lit_color, float3(0.52, 0.62, 0.74), haze);

    // Whisper-subtle metric grid squares (10m, 100m, 1km) projected onto the terrain
    float g_med    = grid_line_mask(input.world_pos.xy, max(pc.cell_med, 10.0), 0.55) * 0.04;
    float g_coarse = grid_line_mask(input.world_pos.xy, max(pc.coarse, 100.0), 0.85) * 0.08;
    float g_weight = max(g_med, g_coarse);
    lit_color = lerp(lit_color, lit_color + float3(0.24, 0.28, 0.32), g_weight);

    // Interactive click survey beacon ring on the terrain (scaled in meters)
    if (pc.marker_active > 0.5) {
        float d = length(input.world_pos.xy - float2(pc.marker_x, pc.marker_y));
        float r_base = clamp(fw_m * 14.0, 18.0, 140.0);
        float ring = 1.0 - smoothstep(r_base * 0.12, r_base * 0.28, abs(d - r_base));
        float dot_center = 1.0 - smoothstep(0.0, r_base * 0.24, d);
        float beacon = max(ring, dot_center);
        lit_color = lerp(lit_color, float3(1.0, 0.26, 0.06), beacon * 0.90);
    }

    return float4(clamp(lit_color, float3(0.0), float3(1.0)), 1.0);
}
