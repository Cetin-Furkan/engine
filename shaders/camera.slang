// Shared view. q is (x, y, z, w). Identity is (0, 0, 0, 1).
// view = rotate(q, world - cam), then the pixel NDC the cube already used.
// +Y is up in the world and negated in the framebuffer.

float3 view_rotate(float4 q, float3 v) {
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float3 view_of(float3 world, float4 q, float3 cam) {
    return view_rotate(q, world - cam);
}

float4 view_ndc(float3 view, float zoom, uint width, uint height, float depth_bias) {
    float sx = max(float(width), 1.0);
    float sy = max(float(height), 1.0);
    return float4(view.x * zoom / (sx * 0.5),
                  -view.y * zoom / (sy * 0.5),
                  0.5 - view.z / 4096.0 - depth_bias,
                  1.0);
}
