float axis_phase(float wrapped_cam, float c, float coarse, float span_steps, float span_mod) {
    float turns = c * span_steps;
    float frac_turns = turns - floor(turns);
    float phase = frac_turns * coarse + c * span_mod;
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

struct PSInput {
    [[vk::location(0)]] float2 v_c : TEXCOORD0;
    [[vk::location(1)]] nointerpolation float v_cell_med : TEXCOORD1;
    [[vk::location(2)]] nointerpolation float v_frac : TEXCOORD2;
    [[vk::location(3)]] nointerpolation float2 v_cam_mod : TEXCOORD3;
    [[vk::location(4)]] nointerpolation float v_span_steps : TEXCOORD4;
    [[vk::location(5)]] nointerpolation float v_span_mod : TEXCOORD5;
    [[vk::location(6)]] nointerpolation float2 v_fw : TEXCOORD6;
};

[shader("fragment")]
float4 main(PSInput pin) : SV_Target0 {
    float d_fine   = pin.v_cell_med * 0.1;
    float d_med    = pin.v_cell_med;
    float d_coarse = max(pin.v_cell_med * 10.0, 1.0e-6);
    float2 world = float2(axis_phase(pin.v_cam_mod.x, pin.v_c.x, d_coarse, pin.v_span_steps, pin.v_span_mod),
                         axis_phase(pin.v_cam_mod.y, pin.v_c.y, d_coarse, pin.v_span_steps, pin.v_span_mod));
    float2 fw = max(pin.v_fw, float2(1.0e-7));

    float fade_fine = 1.0 - smoothstep(0.0, 0.90, pin.v_frac);
    float fade_med  = lerp(0.80, 0.32, pin.v_frac);

    float g_fine   = metric_grid(world, fw, d_fine,   0.65) * fade_fine * 0.40;
    float g_med    = metric_grid(world, fw, d_med,    0.85) * fade_med;
    float g_coarse = metric_grid(world, fw, d_coarse, 1.25) * 0.95;

    float w = max(max(g_fine, g_med), g_coarse);

    float3 base_col = float3(0.055, 0.16, 0.34);
    float3 line_col = float3(0.62, 0.82, 0.96);
    return float4(lerp(base_col, line_col, clamp(w, 0.0, 1.0)), 1.0);
}
