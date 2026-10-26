#version 450

// Analytic grid. fwidth keeps the line about one pixel at any zoom,
// so the mesh does not carry line geometry. Major lines are every
// fourth cell of this clipmap level.

layout(location = 0) in vec2 v_world;
layout(location = 1) flat in float v_cell;
layout(location = 0) out vec4 out_color;

float grid_aa(vec2 world, float cell) {
    vec2 coord = world / max(cell, 1.0);
    vec2 fw = max(fwidth(coord), vec2(1e-4));
    vec2 g = abs(fract(coord - 0.5) - 0.5);
    vec2 a = smoothstep(vec2(0.0), fw * 1.15, g);
    return 1.0 - min(a.x, a.y);
}

void main() {
    float minor = grid_aa(v_world, v_cell);
    float major = grid_aa(v_world, v_cell * 4.0);
    vec3 base = vec3(0.055, 0.16, 0.34);
    vec3 line = vec3(0.62, 0.82, 0.96);
    float w = max(minor * 0.55, major * 0.90);
    out_color = vec4(mix(base, line, clamp(w, 0.0, 1.0)), 1.0);
}
