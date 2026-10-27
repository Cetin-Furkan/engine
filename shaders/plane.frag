#version 450

// Live metric grid: three nested base-10 decades (10^(k-1), 10^k,
// 10^(k+1)) around the medium cell the vertex stage selected. Line
// widths are in screen pixels via the camera's pixel footprint, so they stay
// constant at any window size or zoom. A decade fades out when its
// screen spacing drops below ~6 px (zoomed out or tilted edge-on),
// so the grid never aliases into moire.

layout(location = 0) noperspective in vec2 v_c;
layout(location = 1) flat in float v_cell_med;
layout(location = 2) flat in float v_frac;
layout(location = 3) flat in vec2 v_cam_mod;
layout(location = 4) flat in float v_span_steps;
layout(location = 5) flat in float v_span_mod;
layout(location = 6) flat in vec2 v_fw;
layout(location = 0) out vec4 out_color;

// mod(wrapped_camera + c * span, coarse). span is split into whole
// coarse cells and a remainder so the multiply never adds the pan to
// a corner of several thousand units. Matches plane_grid_phase.
float axis_phase(float wrapped_cam, float c, float coarse) {
    float turns = c * v_span_steps;
    float frac_turns = turns - floor(turns);
    float phase = frac_turns * coarse + c * v_span_mod;
    float m = wrapped_cam + phase;
    float p = max(coarse, 1.0e-6);
    m = m - p * floor(m / p);
    if (m < 0.0) {
        m += p;
    }
    if (m >= p) {
        m -= p;
    }
    return m;
}

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
    float d_fine   = v_cell_med * 0.1;
    float d_med    = v_cell_med;
    float d_coarse = max(v_cell_med * 10.0, 1.0e-6);
    vec2 world = vec2(axis_phase(v_cam_mod.x, v_c.x, d_coarse),
                      axis_phase(v_cam_mod.y, v_c.y, d_coarse));
    vec2 fw = max(v_fw, vec2(1.0e-7));

    float fade_fine = 1.0 - smoothstep(0.0, 0.90, v_frac);
    float fade_med  = mix(0.80, 0.32, v_frac);

    float g_fine   = metric_grid(world, fw, d_fine,   0.65) * fade_fine * 0.40;
    float g_med    = metric_grid(world, fw, d_med,    0.85) * fade_med;
    float g_coarse = metric_grid(world, fw, d_coarse, 1.25) * 0.95;

    float w = max(max(g_fine, g_med), g_coarse);

    vec3 base_col = vec3(0.055, 0.16, 0.34);
    vec3 line_col = vec3(0.62, 0.82, 0.96);
    out_color = vec4(mix(base_col, line_col, clamp(w, 0.0, 1.0)), 1.0);
}
