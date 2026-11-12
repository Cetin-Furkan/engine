[shader("fragment")]
float4 main(float4 sv_pos : SV_Position,
            [[vk::location(0)]] float4 color : COLOR0) : SV_Target0 {
    return color;
}
