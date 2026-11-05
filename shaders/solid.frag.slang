#include "lighting.slang"

struct PSInput {
    [[vk::location(0)]] float3 v_nrm : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit : TEXCOORD2;
};

[shader("fragment")]
float4 main(PSInput pin) : SV_Target0 {
    SurfaceFragment surf = { pin.v_nrm, pin.v_albedo, pin.v_emit, 0.30 };
    return float4(shade_surface(surf), 1.0);
}
