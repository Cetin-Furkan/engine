struct FullPush {
    uint64_t pad0;
    uint64_t pad1;
    uint pad2;
    uint pad3;
    uint pad4;
    float pad5;
    float pad6;
    float pad7;
    float pad8;
    float pad9;
    uint pad10;
    uint pad11;
    float r;
    float g;
    float b;
    float a;
};
[[vk::push_constant]] FullPush pc;

[shader("fragment")]
float4 main([[vk::location(0)]] nointerpolation float3 v_emit : TEXCOORD0) : SV_Target0 {
    return float4(min(float3(pc.r, pc.g, pc.b) + v_emit, float3(1.0)), pc.a);
}
