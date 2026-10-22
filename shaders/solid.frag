#version 450

// Same sun as the cube. Faceted or rounded is already in the vertex normal.

layout(location = 0) centroid in vec3 v_nrm;
layout(location = 1) centroid in vec3 v_albedo;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 n = normalize(v_nrm);
    const vec3 to_sun = normalize(vec3(0.28, 0.86, 0.42));
    const vec3 sun = vec3(1.0, 0.96, 0.88);
    vec3 sky = vec3(0.18, 0.22, 0.30) * (0.70 + 0.30 * clamp(n.y, 0.0, 1.0));
    float ndl = max(dot(n, to_sun), 0.0);
    vec3 color = v_albedo * (sky + sun * ndl);
    out_color = vec4(min(color, vec3(1.0)), 1.0);
}
