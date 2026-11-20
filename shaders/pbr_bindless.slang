// Shared Slang 2026 Vulkan 1.4 Bindless PBR Texture & Material Module.
// Matches PbrMaterialGpu (128B) in include/texture/texture.h byte-for-byte.

static const uint k_pbr_flag_double_sided = 1u << 0u;
static const uint k_pbr_flag_alpha_mask   = 1u << 1u;
static const uint k_pbr_flag_alpha_blend  = 1u << 2u;
static const uint k_pbr_flag_unlit        = 1u << 3u;
static const uint k_pbr_flag_has_tangents = 1u << 4u;
static const uint k_pbr_flag_has_normal   = 1u << 5u;
static const uint k_pbr_flag_has_occ      = 1u << 6u;
static const uint k_pbr_flag_has_base_tex = 1u << 7u;
static const uint k_pbr_flag_has_mr_tex   = 1u << 8u;
static const uint k_pbr_flag_has_emi_tex  = 1u << 9u;

struct PbrMaterialGpu {
    float4 base_color_factor;
    float3 emissive_factor;
    float  metallic_factor;
    float  roughness_factor;
    float  normal_scale;
    float  occlusion_strength;
    float  alpha_cutoff;
    float  ior;
    float  uv_cos;
    float  uv_sin;
    uint   flags;
    float2 uv_offset;
    float2 uv_scale;
    uint   tex_base;
    uint   samp_base;
    uint   tex_mr;
    uint   samp_mr;
    uint   tex_normal;
    uint   samp_normal;
    uint   tex_occ;
    uint   samp_occ;
    uint   tex_emissive;
    uint   samp_emissive;
    uint   pad0;
    uint   pad1;
};

[[vk::binding(0, 1)]] Texture2D    g_bindless_tex[1024];
[[vk::binding(1, 1)]] SamplerState g_bindless_samp[18];

float2 pbr_transform_uv(float2 uv, PbrMaterialGpu mat) {
    float2 s = uv * mat.uv_scale;
    float2 r = float2(s.x * mat.uv_cos + s.y * mat.uv_sin,
                     -s.x * mat.uv_sin + s.y * mat.uv_cos);
    return r + mat.uv_offset;
}

float2 pbr_transform_duv(float2 duv, PbrMaterialGpu mat) {
    float2 s = duv * mat.uv_scale;
    return float2(s.x * mat.uv_cos + s.y * mat.uv_sin,
                 -s.x * mat.uv_sin + s.y * mat.uv_cos);
}

float4 pbr_sample_tex(uint tex_id, uint samp_id, float2 uv) {
    uint t = min(tex_id, 1023u);
    uint s = min(samp_id, 17u);
    return g_bindless_tex[NonUniformResourceIndex(t)].Sample(
        g_bindless_samp[NonUniformResourceIndex(s)], uv);
}

float4 pbr_sample_tex_grad(uint tex_id, uint samp_id, float2 uv, float2 dux, float2 duy) {
    uint t = min(tex_id, 1023u);
    uint s = min(samp_id, 17u);
    return g_bindless_tex[NonUniformResourceIndex(t)].SampleGrad(
        g_bindless_samp[NonUniformResourceIndex(s)], uv, dux, duy);
}

float3 pbr_perturb_normal(
    float3 view_nrm,
    float3 view_tan,
    float  bitan_sign,
    bool   has_tan,
    float2 ts_rg,
    float  normal_scale,
    float3 dp_dx,
    float3 dp_dy,
    float2 duv_dx,
    float2 duv_dy)
{
    float3 N = normalize(view_nrm);
    float2 xy = (ts_rg * 2.0 - float2(1.0)) * normal_scale;
    float  z  = sqrt(max(1.0 - dot(xy, xy), 0.0));

    float3 T;
    float3 B;
    if (has_tan && dot(view_tan, view_tan) > 1.0e-8) {
        T = view_tan - N * dot(N, view_tan);
        float tl2 = dot(T, T);
        if (tl2 > 1.0e-8) {
            T *= rsqrt(tl2);
            B = cross(N, T) * bitan_sign;
            return normalize(T * xy.x + B * xy.y + N * z);
        }
    }

    // Cotangent-frame fallback when mesh lacks explicit vertex tangents
    float3 dp2perp = cross(dp_dy, N);
    float3 dp1perp = cross(N, dp_dx);
    T = dp2perp * duv_dx.x + dp1perp * duv_dy.x;
    B = dp2perp * duv_dx.y + dp1perp * duv_dy.y;
    float invmax = max(dot(T, T), dot(B, B));
    if (invmax <= 1.0e-12) {
        return N;
    }
    float s = rsqrt(invmax);
    return normalize(T * (xy.x * s) + B * (xy.y * s) + N * z);
}
