// Slang 2026 Indirect Hardware MSAA Meshlet Fragment Shader

#include "lighting.slang"

struct MeshletPSIn {
    [[vk::location(0)]] float3 v_nrm       : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo    : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit      : TEXCOORD2;
    [[vk::location(3)]] float  v_roughness : TEXCOORD3;
};

[shader("fragment")]
float4 main(MeshletPSIn pin) : SV_Target0 {
    SurfaceFragment surf = { pin.v_nrm, pin.v_albedo, pin.v_emit, pin.v_roughness };
    return float4(shade_surface(surf), 1.0);
}
