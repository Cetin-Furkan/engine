// Shared view. q is (x, y, z, w). Identity is (0, 0, 0, 1).
// view = rotate(q, world - cam), then screen NDC (orthographic when persp == 0,
// true 3D perspective with focal distance d_cam = 1/persp when persp > 0).
// +Y is up in view space and negated in the framebuffer.

struct ViewPush {
    uint64_t materials;
    uint mat_index;
    float persp;
    float qx, qy, qz, qw;
    float cam_x, cam_y, cam_z;
    float zoom;

    property float4 q { get { return float4(qx, qy, qz, qw); } }
    property float3 cam { get { return float3(cam_x, cam_y, cam_z); } }
};

static const float k_persp_w_near = 0.08;
static const float k_persp_w_far  = 24.0;
static const float k_persp_z_a    = k_persp_w_far / (k_persp_w_far - k_persp_w_near);
static const float k_persp_z_b    = (k_persp_w_far * k_persp_w_near) / (k_persp_w_far - k_persp_w_near);

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

float4 view_ndc_persp(float3 view, float zoom, float persp, uint width, uint height, float depth_bias) {
    if (persp <= 1.0e-7) {
        return view_ndc(view, zoom, width, height, depth_bias);
    }
    float sx = max(float(width), 1.0);
    float sy = max(float(height), 1.0);
    float w_clip = 1.0 - view.z * persp;
    float z_clip = k_persp_z_a * w_clip - k_persp_z_b - depth_bias * max(w_clip, k_persp_w_near);
    return float4(view.x * zoom / (sx * 0.5),
                  -view.y * zoom / (sy * 0.5),
                  z_clip,
                  w_clip);
}

float3 rotate_pose(float3 v, float ax, float ay) {
    float cx = cos(ax), sx = sin(ax);
    float3 rx = float3(v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx);
    float cy = cos(ay), sy = sin(ay);
    return float3(rx.x * cy + rx.z * sy, rx.y, -rx.x * sy + rx.z * cy);
}

float3 rotate_yaw(float3 v, float ay) {
    float cy = cos(ay), sy = sin(ay);
    return float3(v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy);
}

struct MeshVertex {
    float3 pos;
    float3 nrm;
    float3 albedo;
};

struct SoftVertexState {
    float3 disp;
    float  temp;
    float3 vel;
    float  pad;
};
