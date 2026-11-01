struct PSInput {
    [[vk::location(0)]] float3 v_nrm : TEXCOORD0;
    [[vk::location(1)]] float3 v_albedo : TEXCOORD1;
    [[vk::location(2)]] float3 v_emit : TEXCOORD2;
};

[shader("fragment")]
float4 main(PSInput pin) : SV_Target0 {
    float3 n = normalize(pin.v_nrm);
    const float3 to_sun = normalize(float3(0.28, 0.86, 0.42));
    const float3 sun = float3(1.0, 0.96, 0.88);
    float3 sky = float3(0.18, 0.22, 0.30) * (0.70 + 0.30 * clamp(n.y, 0.0, 1.0));
    float ndl = max(dot(n, to_sun), 0.0);
    float rim = pow(1.0 - clamp(ndl, 0.0, 1.0), 2.0);
    float3 color = pin.v_albedo * (sky + sun * ndl) + pin.v_emit * (0.65 + 0.35 * rim);
    return float4(min(color, float3(1.0)), 1.0);
}
