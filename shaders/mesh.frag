#version 450

// Same sun as the solids. The OBJ normal is already in the vertex.

layout(location = 0) centroid in vec3 v_nrm;
layout(location = 1) centroid in vec3 v_albedo;
layout(location = 2) centroid in vec3 v_emit;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 n = normalize(v_nrm);
    const vec3 to_sun = normalize(vec3(0.28, 0.86, 0.42));
    const vec3 sun = vec3(1.0, 0.96, 0.88);
    vec3 sky = vec3(0.18, 0.22, 0.30) * (0.70 + 0.30 * clamp(n.y, 0.0, 1.0));
    float ndl = max(dot(n, to_sun), 0.0);
    float rim = pow(1.0 - clamp(ndl, 0.0, 1.0), 2.0);
    vec3 color = v_albedo * (sky + sun * ndl) + v_emit * (0.65 + 0.35 * rim);
    out_color = vec4(min(color, vec3(1.0)), 1.0);
}
