// Screen-space metric backdrop + 3D z = ground_z Global Illumination receiver.
// When is_3d != 0 and GI is active, intersects the pixel's camera ray with z = ground_z
// to cast crisp 32-bit visibility bitmask contact shadows and colored indirect bounce pools.

#include "camera.slang"
#include "lighting.slang"

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
    if (m < 0.0) {
        m += p;
    }
    if (m >= p) {
        m -= p;
    }
    return m;
}

float2 slang_mod(float2 x, float y) {
    return x - y * floor(x / y);
}

float metric_grid(float2 world, float2 fw, float step_size, float half_width_px) {
    float s = max(step_size, 1.0e-6);
    float2 half_s = float2(s * 0.5);
    float2 dist_world = abs(slang_mod(world + half_s, s) - half_s);
    float2 dist_px    = dist_world / max(fw, float2(1.0e-7));
    float2 line = 1.0 - smoothstep(float2(half_width_px - 0.5),
                                   float2(half_width_px + 0.5),
                                   dist_px);
    float2 density_vis = smoothstep(float2(2.0), float2(6.0),
                                    float2(s) / max(fw, float2(1.0e-7)));
    line *= density_vis;
    return max(line.x, line.y);
}

[shader("fragment")]
float4 main(float4 frag : SV_Position) : SV_Target0 {
    float w = max(float(pc.width), 1.0);
    float h = max(float(pc.height), 1.0);
    // WaveRotate of a uniform is the same value on every lane.
    float zoom = max(WaveRotate(pc.zoom, 1u), 1.0e-8);
    float2 ndc = float2(frag.x / w * 2.0 - 1.0, frag.y / h * 2.0 - 1.0);
    float2 delta = float2(ndc.x * (w * 0.5) / zoom, -ndc.y * (h * 0.5) / zoom);
    float coarse = max(pc.coarse, 1.0e-6);
    float2 world = float2(wrap_period(pc.origin_x + delta.x, coarse),
                         wrap_period(pc.origin_y + delta.y, coarse));
    float2 fw = float2(1.0 / zoom);

    float d_fine   = pc.cell_med * 0.1;
    float d_med    = pc.cell_med;
    float d_coarse = coarse;
    float fade_fine = 1.0 - smoothstep(0.0, 0.90, pc.level_frac);
    float fade_med  = lerp(0.80, 0.32, pc.level_frac);

    float g_fine   = metric_grid(world, fw, d_fine,   0.65) * fade_fine * 0.40;
    float g_med    = metric_grid(world, fw, d_med,    0.85) * fade_med;
    float g_coarse = metric_grid(world, fw, d_coarse, 1.25) * 0.95;

    float weight    = clamp(max(max(g_fine, g_med), g_coarse), 0.0, 1.0);
    float3 base_col = float3(0.055, 0.16, 0.34);
    float3 line_col = float3(0.62, 0.82, 0.96);
    float3 col      = lerp(base_col, line_col, weight);

    if (pc.is_3d != 0u && pc.view.materials != 0ull) {
        LightingFrameHeader *hdr = (LightingFrameHeader *)(pc.view.materials + k_lighting_header_offset);
        if (hdr->magic == k_lighting_magic && hdr->gi_enabled != 0u && hdr->has_ground != 0u) {
            float4 q_inv = float4(-pc.view.qx, -pc.view.qy, -pc.view.qz, pc.view.qw);
            float3 rd    = normalize(view_rotate(q_inv, float3(0.0, 0.0, -1.0)));
            if (abs(rd.z) > 1.0e-4) {
                float3 ro     = pc.view.cam + view_rotate(q_inv, float3(delta.x, delta.y, 65536.0));
                float tg      = (hdr->ground_z - ro.z) / rd.z;
                float gz_view = 65536.0 - tg;
                if (tg > 0.05 && abs(gz_view) <= 2400.0) {
                    float3 wnrm     = float3(0.0, 0.0, rd.z < 0.0 ? 1.0 : -1.0);
                    float3 vnrm     = normalize(view_rotate(pc.view.q, wnrm));
                    float  ao       = 1.0;
                    float3 bent_n   = vnrm;
                    float3 ind_rgb  = float3(0.0);
                    if (lighting_sample_gi(pc.view.materials, frag.xy, gz_view, vnrm, ao, bent_n, ind_rgb)) {
                        float3 floor_alb = lerp(float3(0.24, 0.32, 0.44), float3(0.58, 0.68, 0.80), weight);
                        col = col * lerp(0.32, 1.0, ao) + floor_alb * min(ind_rgb, float3(1.6)) * 0.42;
                    }
                }
            }
        }
    }

    return float4(clamp(col, float3(0.0), float3(1.0)), 1.0);
}
