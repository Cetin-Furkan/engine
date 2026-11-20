// Slang 2026 Physically-Based Energy-Conserving Jimenez 3x3 Tent Bloom Upsample
// Reads downsampled base mip[i] and coarser bloom up[i+1] via hardware bilinear samplers,
// and blends them with an energy-conserving scatter weight (never blows out to white).

[[vk::binding(0, 0)]] Sampler2D           g_base_tex;
[[vk::binding(1, 0)]] Sampler2D           g_small_tex;
[[vk::binding(2, 0)]] RWTexture2D<float4> g_out_tex;

struct PostUpPush {
    uint  dst_w;
    uint  dst_h;
    float radius_u;
    float radius_v;
    float scatter;
    uint  pad0;
};

[[vk::push_constant]] PostUpPush pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= pc.dst_w || dtid.y >= pc.dst_h) {
        return;
    }

    float2 uv = (float2(dtid.xy) + 0.5) / float2(float(pc.dst_w), float(pc.dst_h));
    float  rx = pc.radius_u;
    float  ry = pc.radius_v;

    // 9-tap Jimenez 3x3 tent filter: (1 2 1 / 2 4 2 / 1 2 1) / 16
    float3 s  = g_small_tex.SampleLevel(uv + float2(-rx, -ry), 0.0).rgb * 1.0;
    s        += g_small_tex.SampleLevel(uv + float2(0.0, -ry), 0.0).rgb * 2.0;
    s        += g_small_tex.SampleLevel(uv + float2( rx, -ry), 0.0).rgb * 1.0;
    s        += g_small_tex.SampleLevel(uv + float2(-rx, 0.0), 0.0).rgb * 2.0;
    s        += g_small_tex.SampleLevel(uv,                    0.0).rgb * 4.0;
    s        += g_small_tex.SampleLevel(uv + float2( rx, 0.0), 0.0).rgb * 2.0;
    s        += g_small_tex.SampleLevel(uv + float2(-rx,  ry), 0.0).rgb * 1.0;
    s        += g_small_tex.SampleLevel(uv + float2(0.0,  ry), 0.0).rgb * 2.0;
    s        += g_small_tex.SampleLevel(uv + float2( rx,  ry), 0.0).rgb * 1.0;
    float3 tent = max(s * (1.0 / 16.0), float3(0.0));

    float3 base = max(g_base_tex.SampleLevel(uv, 0.0).rgb, float3(0.0));
    float3 out_rgb = lerp(base, tent, pc.scatter);
    g_out_tex[dtid.xy] = float4(out_rgb, 1.0);
}
