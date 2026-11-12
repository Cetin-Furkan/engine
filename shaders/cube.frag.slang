#include "lighting.slang"

struct PSInput {
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_nrm : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit : TEXCOORD2;
    [[vk::location(3)]] float  v_view_z : TEXCOORD3;
    [[vk::location(4)]] nointerpolation uint64_t v_env_bda : TEXCOORD4;
};

[shader("fragment")]
float4 main(PSInput pin) : SV_Target0 {
    SurfaceFragment surf = { pin.v_nrm, pin.v_albedo, pin.v_emit, 0.36 };
    return float4(shade_surface_gi(surf, pin.sv_pos.xy, pin.v_view_z, pin.v_env_bda), 1.0);
}
