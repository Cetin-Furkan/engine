// Shared view. q is (x, y, z, w). Identity is (0, 0, 0, 1).
// view = rotate(q, world - cam), then the pixel NDC the cube already used.
// +Y is up in the world and negated in the framebuffer.

vec3 view_rotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec3 view_of(vec3 world, vec4 q, vec3 cam) {
    return view_rotate(q, world - cam);
}

vec4 view_ndc(vec3 view, float zoom, uint width, uint height, float depth_bias) {
    float sx = max(float(width), 1.0);
    float sy = max(float(height), 1.0);
    return vec4(view.x * zoom / (sx * 0.5),
                -view.y * zoom / (sy * 0.5),
                0.5 - view.z / 4096.0 - depth_bias,
                1.0);
}
