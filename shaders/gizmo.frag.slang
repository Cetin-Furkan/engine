// Slang 2026 3D Transform Gizmo & Debug-Draw Overlay Fragment Shader

[shader("fragment")]
float4 main([[vk::location(0)]] float4 v_color : TEXCOORD0) : SV_Target0 {
    return v_color;
}
