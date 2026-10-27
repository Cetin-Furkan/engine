#version 450

// Live metric grid: three nested base-10 decades (10^(k-1), 10^k,
// 10^(k+1)) around the medium cell the vertex stage selected. Line
// widths are in screen pixels via fwidth(v_world), so they stay
// constant at any window size or zoom. A decade fades out when its
// screen spacing drops below ~6 px (zoomed out or tilted edge-on),
// so the grid never aliases into moire.

layout(location = 0) in vec2 v_world;
layout(location = 1) flat in float v_cell_med;
layout(location = 2) flat in float v_frac;
layout(location = 0) out vec4 out_color;

float metric_grid(vec2 world, vec2 fw, float step_size, float half_width_px) {
    float s = max(step_size, 1.0e-6);
    vec2 half_s = vec2(s * 0.5);
    vec2 dist_world = abs(mod(world + half_s, s) - half_s);
    vec2 dist_px    = dist_world / max(fw, vec2(1.0e-7));
    vec2 line = 1.0 - smoothstep(vec2(half_width_px - 0.5),
                                 vec2(half_width_px + 0.5),
                                 dist_px);
    vec2 density_vis = smoothstep(vec2(2.0), vec2(6.0),
                                  vec2(s) / max(fw, vec2(1.0e-7)));
    line *= density_vis;
    return max(line.x, line.y);
}

void main() {
    vec2 fw = fwidth(v_world);

    float d_fine   = v_cell_med * 0.1;
    float d_med    = v_cell_med;
    float d_coarse = v_cell_med * 10.0;

    float fade_fine = 1.0 - smoothstep(0.0, 0.90, v_frac);
    float fade_med  = mix(0.80, 0.32, v_frac);

    float g_fine   = metric_grid(v_world, fw, d_fine,   0.65) * fade_fine * 0.40;
    float g_med    = metric_grid(v_world, fw, d_med,    0.85) * fade_med;
    float g_coarse = metric_grid(v_world, fw, d_coarse, 1.25) * 0.95;

    float w = max(max(g_fine, g_med), g_coarse);

    vec3 base_col = vec3(0.055, 0.16, 0.34);
    vec3 line_col = vec3(0.62, 0.82, 0.96);
    out_color = vec4(mix(base_col, line_col, clamp(w, 0.0, 1.0)), 1.0);
}
