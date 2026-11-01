[shader("fragment")]
float4 main([[vk::location(0)]] nointerpolation float3 v_color : TEXCOORD0) : SV_Target0 {
    return float4(v_color, 1.0);
}
