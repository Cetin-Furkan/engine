// Slang 2026 Indirect Hardware MSAA Meshlet Fragment Shader

#include "lighting.slang"

struct MeshletPSIn {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm       : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo    : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit      : TEXCOORD2;
    [[vk::location(3)]] float  v_roughness : TEXCOORD3;
    [[vk::location(4)]] float  v_view_z    : TEXCOORD4;
    [[vk::location(5)]] nointerpolation uint64_t v_env_bda : TEXCOORD5;
};

[shader("fragment")]
float4 main(MeshletPSIn pin) : SV_Target0 {
    SurfaceFragment surf = { pin.v_nrm, pin.v_albedo, pin.v_emit, pin.v_roughness };
    return float4(shade_surface_gi(surf, pin.sv_pos.xy, pin.v_view_z, pin.v_env_bda), 1.0);
}
