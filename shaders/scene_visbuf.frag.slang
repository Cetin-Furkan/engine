// Slang 2026 1x R32_UINT Visibility Buffer Fragment Shader
// Writes 32-bit packed ((instance_id + 1) : 14) | (meshlet_id : 11) | (prim_id : 7).

[shader("fragment")]
uint main([[vk::location(0)]] nointerpolation uint vis_id : TEXCOORD0) : SV_Target0 {
    return vis_id;
}
