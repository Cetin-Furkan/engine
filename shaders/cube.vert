#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// The cube is posed here. The mesh is read by address from gl_VertexIndex.
// Nothing on the CPU names a front face. The rasterizer keeps a face only
// when it points at the camera. k_edge_px is pixels, so a larger window
// does not grow the cube. origin_x and origin_y are pixels from the window
// center, with +Y up. Negating Y puts +Y at the top of the window.
// The pipeline keeps counter-clockwise faces, which are the ones whose
// outward normal points at the camera.

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Mesh {
    float f[];
};

layout(location = 0) centroid out vec3 v_nrm;
layout(location = 1) centroid out vec3 v_albedo;

layout(push_constant, scalar) uniform Screen {
    uint64_t verts;
    uint width;
    uint height;
    float yaw_deg;
    float origin_x;
    float origin_y;
    float depth_bias;
} screen;

const float k_edge_px = 240.0;

void main() {
    Mesh mesh = Mesh(screen.verts);
    uint o = uint(gl_VertexIndex) * 9u;
    vec3 in_pos = vec3(mesh.f[o + 0u], mesh.f[o + 1u], mesh.f[o + 2u]);
    vec3 in_nrm = vec3(mesh.f[o + 3u], mesh.f[o + 4u], mesh.f[o + 5u]);
    vec3 in_albedo = vec3(mesh.f[o + 6u], mesh.f[o + 7u], mesh.f[o + 8u]);
    const float ax = radians(28.0);
    float ay = radians(screen.yaw_deg);
    mat3 rx = mat3(vec3(1.0, 0.0, 0.0),
                   vec3(0.0, cos(ax), sin(ax)),
                   vec3(0.0, -sin(ax), cos(ax)));
    mat3 ry = mat3(vec3(cos(ay), 0.0, -sin(ay)),
                   vec3(0.0, 1.0, 0.0),
                   vec3(sin(ay), 0.0, cos(ay)));
    mat3 pose = ry * rx;
    vec3 p = (pose * in_pos) * (k_edge_px * 0.5);
    p.x += screen.origin_x;
    p.y += screen.origin_y;
    float w = max(float(screen.width), 1.0);
    float h = max(float(screen.height), 1.0);
    gl_Position = vec4(p.x / (w * 0.5),
                       -p.y / (h * 0.5),
                       0.5 - p.z / 4096.0 - screen.depth_bias,
                       1.0);
    v_nrm = pose * in_nrm;
    v_albedo = in_albedo;
}
