// Slang 2026 3D Transform Gizmo & Debug-Draw Overlay Vertex Shader
// Pulls 16-byte GizmoVertex (float3 pos + packed RGBA8) directly from transient BDA.

#include "camera.slang"

struct GizmoVertex {
    float x;
    float y;
    float z;
    uint  rgba;
};

struct GizmoPush {
    uint64_t verts_bda;
    uint     width;
    uint     height;
    ViewPush view;
};

[[vk::push_constant]] GizmoPush pc;

struct VSOutput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float4 v_color : TEXCOORD0;
};

float3 gizmo_srgb_to_linear(float3 c) {
    return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);
}

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    VSOutput out;
    GizmoVertex v = ((GizmoVertex *)pc.verts_bda)[vert_id];
    float3 vp     = view_of(float3(v.x, v.y, v.z), pc.view.q, pc.view.cam);
    out.sv_pos    = view_ndc_persp(vp, pc.view.zoom, pc.view.persp, pc.width, pc.height, 0.0);

    float4 srgb = float4(
        float( v.rgba         & 0xFFu),
        float((v.rgba >>  8u) & 0xFFu),
        float((v.rgba >> 16u) & 0xFFu),
        float((v.rgba >> 24u) & 0xFFu)
    ) * (1.0 / 255.0);
    // Linearize for VK_FORMAT_B8G8R8A8_SRGB hardware OETF
    out.v_color = float4(gizmo_srgb_to_linear(srgb.rgb), srgb.a);
    return out;
}
