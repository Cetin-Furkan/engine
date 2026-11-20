// Shared Slang 2026 Post-Processing Math:
// - BDA Auto-Exposure State (64-byte aligned, matches include/post/post.h)
// - Karis Average & Soft Knee Bloom Prefilter
// - Troy Sobotka AgX ("Punchy" Look) Filmic Tone Mapper (linearized for B8G8R8A8_SRGB)
// - Triangular-PDF Interleaved Gradient Noise (IGN) 8-bit Anti-Banding Dither

struct PostExposureState {
    float ev100;
    float exposure_scale;
    float avg_luma;
    float target_ev100;
    uint  valid;
    uint  frame_index;
    uint  pad0;
    uint  pad1;
    uint  reserved[8];
};

static const float k_post_log_luma_min  = -6.0;
static const float k_post_log_luma_max  = 10.0;
static const float k_post_log_luma_span = 16.0;

float post_luma(float3 c) {
    return dot(max(c, float3(0.0)), float3(0.2126, 0.7152, 0.0722));
}

float post_karis_weight(float3 c) {
    return 1.0 / (1.0 + max(post_luma(c), 0.0));
}

float3 post_soft_threshold(float3 c, float threshold, float knee) {
    float3 x    = max(c, float3(0.0));
    float  br   = max(x.r, max(x.g, x.b));
    float  k    = max(knee, 1.0e-4);
    float  soft = clamp(br - threshold + k, 0.0, 2.0 * k);
    soft        = (soft * soft) / (4.0 * k);
    float  ctrb = max(soft, br - threshold) / max(br, 1.0e-5);
    return x * max(ctrb, 0.0);
}

// Troy Sobotka AgX 6th-order polynomial contrast curve
float3 post_agx_contrast_approx(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}

// Full AgX with Rec.709 inset/outset + subtle "Punchy" CDL look -> linearized for sRGB target
float3 post_agx_punchy_linear(float3 hdr_linear) {
    float3 v = max(hdr_linear, float3(0.0));

    // AgX inset matrix (column-normalized Rec.709 -> AgX working primaries)
    const float3x3 agx_in = float3x3(
        0.842479062253094, 0.0784335999999992, 0.0792237451477643,
        0.0423282422610123, 0.878468636469772, 0.0791661274605434,
        0.0423756549057051, 0.0784336,         0.879142973793104
    );
    const float3x3 agx_out = float3x3(
         1.19687900512017,   -0.0980208811401368, -0.0990297440797205,
        -0.0528968517574562,  1.15190312990417,   -0.0989611768448433,
        -0.0529716355144438, -0.0980434501171241,  1.15107367264116
    );

    v = mul(agx_in, v);
    v = clamp(log2(max(v, float3(1.0e-6))), float3(-12.47393), float3(4.026069));
    v = (v - float3(-12.47393)) / (4.026069 - (-12.47393));
    v = post_agx_contrast_approx(v);
    v = clamp(mul(agx_out, v), float3(0.0), float3(1.0));

    // Subtle AgX "Punchy" look in display-encoded space (crisp blacks, vivid hue-linear mids)
    float  lw = dot(v, float3(0.2126, 0.7152, 0.0722));
    v = pow(max(v, float3(0.0)), float3(1.12));
    v = clamp(lerp(float3(lw), v, 1.14), float3(0.0), float3(1.0));

    // Return linear value so hardware VK_FORMAT_B8G8R8A8_SRGB OETF encodes exact sRGB
    return pow(v, float3(2.2));
}

float3 post_linear_to_srgb(float3 c) {
    float3 x = max(c, float3(0.0));
    float3 lo = x * 12.92;
    float3 hi = 1.055 * pow(x, float3(1.0 / 2.4)) - 0.055;
    return select(x <= float3(0.0031308), lo, hi);
}

float3 post_srgb_to_linear(float3 c) {
    float3 x = clamp(c, float3(0.0), float3(1.0));
    float3 lo = x * (1.0 / 12.92);
    float3 hi = pow((x + 0.055) * (1.0 / 1.055), float3(2.4));
    return select(x <= float3(0.04045), lo, hi);
}

float post_ign(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Triangular-PDF 1-LSB sRGB anti-banding dither (eliminates sky & soft-shadow banding)
float3 post_dither_for_srgb_8bit(float3 linear_rgb, float2 pix, uint frame_idx) {
    float2 jitter = float2(float(frame_idx & 63u) * 5.588238, float((frame_idx >> 2u) & 63u) * 3.141592);
    float  r1     = post_ign(pix + jitter);
    float  r2     = post_ign(pix + jitter + float2(17.0, 29.0));
    float  tri    = (r1 - r2) * (1.0 / 255.0);
    float3 srgb   = clamp(post_linear_to_srgb(linear_rgb) + float3(tri), float3(0.0), float3(1.0));
    return post_srgb_to_linear(srgb);
}
