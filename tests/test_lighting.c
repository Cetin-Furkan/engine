#include "lighting/lighting.h"
#include <math.h>
#include <stdbit.h>
#include <stdio.h>

static int s_failures = 0;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));    \
            s_failures++;                                                          \
        }                                                                          \
    } while (0)

static void test_fp16_and_rgb9e5_codecs(void) {
    // IEEE 754 fp16 exact & subnormal checks
    CHECK(lighting_f32_to_f16(0.0f) == 0x0000u, "fp16 +0.0");
    CHECK(lighting_f32_to_f16(1.0f) == 0x3C00u, "fp16 +1.0");
    CHECK(lighting_f32_to_f16(-2.0f) == 0xC000u, "fp16 -2.0");
    CHECK(fabsf(lighting_f16_to_f32(lighting_f32_to_f16(0.00003f)) - 0.00003f) < 1.0e-5f,
          "fp16 subnormal round-trip");

    // Khronos RGB9E5 HDR codec checks across 6 orders of magnitude
    CHECK(lighting_pack_rgb9e5(0.0f, 0.0f, 0.0f) == 0u, "rgb9e5 zero");
    CHECK(lighting_pack_rgb9e5(-5.0f, -1.0f, 0.0f) == 0u, "rgb9e5 negative clamp");

    const float hdr_cases[4][3] = {
        { 0.004f, 0.002f, 0.001f },
        { 0.85f,  0.42f,  0.18f  },
        { 12.5f,  3.75f,  0.50f  },
        { 420.0f, 180.0f, 64.0f  },
    };
    for (int i = 0; i < 4; ++i) {
        uint32_t packed = lighting_pack_rgb9e5(hdr_cases[i][0], hdr_cases[i][1], hdr_cases[i][2]);
        float dec[3] = {0};
        lighting_unpack_rgb9e5(packed, dec);
        float peak = hdr_cases[i][0];
        for (int c = 0; c < 3; ++c) {
            CHECK(fabsf(dec[c] - hdr_cases[i][c]) <= peak * 0.01f + 1.0e-4f,
                  "rgb9e5 HDR relative precision <= 1%");
        }
    }
}

static void test_octahedral_normal_codecs(void) {
    // Test all 8 octants + cardinal axes for both snorm16 and snorm8 octahedral codecs
    for (int ix = -2; ix <= 2; ++ix) {
        for (int iy = -2; iy <= 2; ++iy) {
            for (int iz = -2; iz <= 2; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) {
                    continue;
                }
                float nx = (float)ix;
                float ny = (float)iy;
                float nz = (float)iz;
                float len = sqrtf(nx * nx + ny * ny + nz * nz);
                nx /= len;
                ny /= len;
                nz /= len;

                float d16[3] = {0};
                float d8[3]  = {0};
                lighting_unpack_oct_snorm16(lighting_pack_oct_snorm16(nx, ny, nz), d16);
                lighting_unpack_oct_snorm8(lighting_pack_oct_snorm8(nx, ny, nz), d8);

                float dot16 = nx * d16[0] + ny * d16[1] + nz * d16[2];
                float dot8  = nx * d8[0]  + ny * d8[1]  + nz * d8[2];
                CHECK(dot16 > 0.9998f, "oct_snorm16 angular fidelity across all 8 octants");
                CHECK(dot8  > 0.994f,  "oct_snorm8 angular fidelity across all 8 octants");
            }
        }
    }
}

static void test_ssilvb_32bit_visibility_bitmask(void) {
    // 1. Exact bitmask span encoding
    CHECK(lighting_ssilvb_sector_mask(0.0f, 0.0f)   == 0x00000000u, "empty span");
    CHECK(lighting_ssilvb_sector_mask(0.0f, 1.0f)   == 0xFFFFFFFFu, "full hemisphere span");
    CHECK(lighting_ssilvb_sector_mask(0.0f, 0.25f)  == 0x000000FFu, "low 8 sectors");
    CHECK(lighting_ssilvb_sector_mask(0.25f, 0.50f) == 0x0000FF00u, "mid-low 8 sectors");
    CHECK(lighting_ssilvb_sector_mask(0.75f, 1.0f)  == 0xFF000000u, "top 8 sectors");

    // 2. Thin foreground occluder pass-through (critical 2026 SSILVB property over legacy HBAO/GTAO):
    // A thin floating pole at (+10, 0, +16) with thickness 3.0 occludes a narrow bit band,
    // leaving the lower horizon bits OPEN so a background lit wall at (+30, 0, +8) STILL
    // transfers indirect bounce light underneath the floating pole!
    const float nrm_up[3] = { 0.0f, 0.0f, 1.0f };
    const LightingSsilvbSample pole_only[1] = {
        {
            .delta_view  = { 10.0f, 0.0f, 16.0f },
            .normal_view = { -1.0f, 0.0f,  0.0f },
            .radiance    = {  0.0f, 0.0f,  0.0f }, // dark foreground pole
        },
    };
    const LightingSsilvbSample pole_plus_bg_wall[2] = {
        pole_only[0],
        {
            .delta_view  = { 30.0f, 0.0f,  8.0f },
            .normal_view = { -1.0f, 0.0f,  0.0f },
            .radiance    = {  4.0f, 2.0f,  1.0f }, // bright sunlit wall behind & below pole!
        },
    };

    LightingSsilvbSliceResult r_pole = lighting_ssilvb_eval_slice(nrm_up, 1.0f, 0.0f, 3.0f, pole_only, 1u);
    LightingSsilvbSliceResult r_both = lighting_ssilvb_eval_slice(nrm_up, 1.0f, 0.0f, 3.0f, pole_plus_bg_wall, 2u);

    CHECK(stdc_count_ones_ui(r_pole.vis_mask) > 0u && stdc_count_ones_ui(r_pole.vis_mask) < 12u,
          "thin foreground pole occupies only a narrow angular sector band");
    CHECK(r_both.visibility < r_pole.visibility,
          "adding background wall monotonically decreases unoccluded visibility");
    CHECK(r_both.indirect_rgb[0] > 0.15f,
          "SSILVB bitmask lets background bounce light pass underneath thin foreground occluder!");
    CHECK(r_both.bent_tilt < -0.05f,
          "bent normal tilts away from +X occluders toward unoccluded -X hemisphere");

    // 3. Duplicate / shadowed sample energy conservation:
    // A second wall directly behind an already-thick occluding wall in the SAME angular bits
    // must contribute ZERO newly_occluded bounce!
    const LightingSsilvbSample thick_front_and_hidden_back[2] = {
        {
            .delta_view  = { 10.0f, 0.0f, 12.0f },
            .normal_view = { -1.0f, 0.0f,  0.0f },
            .radiance    = {  1.0f, 0.0f,  0.0f },
        },
        {
            .delta_view  = { 20.0f, 0.0f, 10.0f },
            .normal_view = { -1.0f, 0.0f,  0.0f },
            .radiance    = {  0.0f, 50.0f, 0.0f }, // completely hidden behind thick front wall!
        },
    };
    LightingSsilvbSliceResult r_occ = lighting_ssilvb_eval_slice(
        nrm_up, 1.0f, 0.0f, 40.0f, thick_front_and_hidden_back, 2u);
    CHECK(r_occ.indirect_rgb[0] > 0.0f && r_occ.indirect_rgb[1] == 0.0f,
          "hidden sample behind thick front wall contributes zero leaked green radiance");
}

static void test_hrc_cascade_merge_and_multibounce(void) {
    // Opaque near interval (beta = 0) completely blocks far cascade radiance (zero light leak!)
    const float near_opaque[3] = { 0.4f, 0.2f, 0.1f };
    const float far_bright[3]  = { 99.0f, 99.0f, 99.0f };
    float out[3] = {0};
    lighting_hrc_merge_ray(near_opaque, 0.0f, far_bright, far_bright, out);
    CHECK(fabsf(out[0] - 0.4f) < 1.0e-6f && fabsf(out[1] - 0.2f) < 1.0e-6f,
          "HRC beta=0 completely blocks far-cascade light leaks");

    // Transparent near interval (beta = 1) preserves exact solid-angle average of child rays
    const float zero_r[3] = { 0.0f, 0.0f, 0.0f };
    const float c_a[3]    = { 1.2f, 0.4f, 0.2f };
    const float c_b[3]    = { 0.8f, 0.6f, 0.6f };
    lighting_hrc_merge_ray(zero_r, 1.0f, c_a, c_b, out);
    CHECK(fabsf(out[0] - 1.0f) < 1.0e-6f &&
          fabsf(out[1] - 0.5f) < 1.0e-6f &&
          fabsf(out[2] - 0.4f) < 1.0e-6f,
          "HRC beta=1 preserves exact average of bifurcated child rays");

    // Jimenez multi-bounce AO preserves boundary endpoints V=0 -> 0, V=1 -> 1, and boosts midtones
    const float red_alb[3] = { 0.9f, 0.1f, 0.1f };
    float mb0[3], mb1[3], mb_half[3];
    lighting_gtao_multibounce(0.0f, red_alb, mb0);
    lighting_gtao_multibounce(1.0f, red_alb, mb1);
    lighting_gtao_multibounce(0.5f, red_alb, mb_half);
    CHECK(mb0[0] == 0.0f && fabsf(mb1[0] - 1.0f) < 1.0e-3f, "GTAO multibounce endpoints");
    CHECK(mb_half[0] > 0.65f && mb_half[0] > mb_half[1], "GTAO multibounce chromatic lift");
}

static void test_rotated_proxy_ray_oracles(void) {
    const float alb[3] = { 0.7f, 0.5f, 0.3f };
    const float emi[3] = { 1.5f, 0.8f, 0.2f };

    // 1. Pitched & yawed OBB intersection + unit outward normal check
    LightingProxy obb = {0};
    const float center[3] = { 40.0f, -25.0f, 120.0f };
    const float ext[3]    = { 60.0f,  60.0f,  60.0f };
    lighting_proxy_fill(&obb, LIGHTING_PROXY_OBB, center, ext, 0.48f, 0.65f, 0.36f, alb, emi);

    const float ro[3] = { 40.0f, -25.0f, 320.0f };
    const float rd[3] = {  0.0f,   0.0f,  -1.0f };
    float t_hit = 0.0f;
    float n_hit[3] = {0};
    bool hit = lighting_ray_proxy_interval(ro, rd, 1.0f, 500.0f, &obb, &t_hit, n_hit);
    CHECK(hit && t_hit > 90.0f && t_hit < 200.0f, "rotated OBB ray hit distance valid");
    float nlen = sqrtf(n_hit[0] * n_hit[0] + n_hit[1] * n_hit[1] + n_hit[2] * n_hit[2]);
    CHECK(fabsf(nlen - 1.0f) < 1.0e-4f && n_hit[2] > 0.2f, "rotated OBB outward unit normal");

    // Interval clipping check: [1.0, t_hit - 5.0] must miss!
    CHECK(!lighting_ray_proxy_interval(ro, rd, 1.0f, t_hit - 5.0f, &obb, nullptr, nullptr),
          "HRC interval [t_min, t_max] clips hits beyond t_max");

    // 2. Squashed ellipsoid intersection check
    LightingProxy ell = {0};
    const float e_rad[3] = { 90.0f, 90.0f, 45.0f }; // 2:1 Z-squashed sphere
    lighting_proxy_fill(&ell, LIGHTING_PROXY_ELLIPSOID, center, e_rad, 0.0f, 0.0f, 0.25f, alb, emi);
    float t_ell = 0.0f;
    float n_ell[3] = {0};
    CHECK(lighting_ray_proxy_interval(ro, rd, 1.0f, 500.0f, &ell, &t_ell, n_ell),
          "squashed ellipsoid hit");
    CHECK(fabsf(t_ell - (320.0f - (120.0f + 45.0f))) < 1.0e-3f && n_ell[2] > 0.999f,
          "squashed ellipsoid exact Z-apex intersection at z = center.z + rz");
}

int main(void) {
    CHECK(lighting_self_check(), "lighting_self_check()");
    test_fp16_and_rgb9e5_codecs();
    test_octahedral_normal_codecs();
    test_ssilvb_32bit_visibility_bitmask();
    test_hrc_cascade_merge_and_multibounce();
    test_rotated_proxy_ray_oracles();

    if (s_failures != 0) {
        fprintf(stderr, "[test_lighting] %d assertion(s) failed!\n", s_failures);
        return 1;
    }
    printf("[test_lighting] All 2026 SSILVB 32-bit bitmask, Split HRC merge, HDR codec & ray-oracle tests passed!\n");
    return 0;
}
