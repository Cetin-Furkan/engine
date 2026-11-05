// Shared Slang 2026 physically-inspired surface lighting module.
// Combines a directional sun key light, cool back-rim fill light,
// sky/ground hemisphere irradiance, GGX microfacet specular lobe,
// and Schlick Fresnel emissive boost.

interface IShadedSurface {
    property float3 normal   { get; }
    property float3 albedo   { get; }
    property float3 emissive { get; }
    property float  roughness { get; }
};

struct SurfaceFragment : IShadedSurface {
    float3 nrm;
    float3 alb;
    float3 emi;
    float  rgh;

    property float3 normal    { get { return normalize(nrm); } }
    property float3 albedo    { get { return alb; } }
    property float3 emissive  { get { return emi; } }
    property float  roughness { get { return rgh; } }
};

float ggx_d(float ndh, float alpha) {
    float a2 = alpha * alpha;
    float d  = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1.0e-5);
}

float3 shade_surface<S : IShadedSurface>(S surf) {
    float3 n = surf.normal;
    const float3 to_sun  = normalize(float3(0.28, 0.86, 0.42));
    const float3 to_fill = normalize(float3(-0.62, -0.24, 0.74));
    const float3 to_view = float3(0.0, 0.0, 1.0);
    const float3 half_v  = normalize(to_sun + to_view);

    const float3 sun_col  = float3(1.00, 0.96, 0.88);
    const float3 fill_col = float3(0.20, 0.30, 0.46);
    const float3 sky_top  = float3(0.24, 0.30, 0.40);
    const float3 sky_bot  = float3(0.11, 0.13, 0.18);

    float hemi = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
    float3 ambient = lerp(sky_bot, sky_top, hemi);

    float ndl      = max(dot(n, to_sun), 0.0);
    float ndfill   = max(dot(n, to_fill), 0.0) * 0.22;
    float ndv      = max(abs(n.z), 1.0e-3);
    float ndh      = max(dot(n, half_v), 0.0);

    float alpha    = max(surf.roughness * surf.roughness, 0.08);
    float spec_d   = ggx_d(ndh, alpha);
    float fresnel  = 0.04 + 0.96 * pow(1.0 - clamp(ndv, 0.0, 1.0), 4.0);
    float3 spec    = sun_col * (spec_d * fresnel * ndl * 0.14);

    float rim      = pow(1.0 - clamp(ndl, 0.0, 1.0), 2.0);
    float3 diffuse = surf.albedo * (ambient + sun_col * ndl + fill_col * ndfill);
    float3 emit    = surf.emissive * (0.68 + 0.32 * max(rim, fresnel));

    return min(diffuse + spec + emit, float3(1.0));
}
