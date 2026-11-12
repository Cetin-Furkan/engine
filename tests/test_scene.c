#include "scene/scene.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// Unit & mathematical invariance tests for Engine v0.2 Milestone 3:
// 1. Octahedral snorm16 normal codec (< 0.02 deg error across 4096-dir Fibonacci sphere)
// 2. 16-bit local AABB position quantization & RGBA8 packing
// 3. 32-bit Visibility Buffer (14:11:7) bitpacking & sentinel invariants
// 4. Analytical perspective-correct barycentrics & partial derivatives vs finite differences
// 5. 64v/124t Morton+greedy meshlet clusterizer, vertex welding, bounding sphere & normal cone bounds

static int s_failures = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            s_failures++;                                                       \
        }                                                                       \
    } while (0)

static void test_octahedral_snorm16(void) {
    // Cardinal axes exactness
    const float axes[6][3] = {
        {  1.0f,  0.0f,  0.0f },
        { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f },
        {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f },
        {  0.0f,  0.0f, -1.0f },
    };
    for (int i = 0; i < 6; ++i) {
        int16_t oct[2] = {0};
        float dec[3] = {0};
        scene_oct_encode_snorm16(axes[i][0], axes[i][1], axes[i][2], oct);
        scene_oct_decode_snorm16(oct, dec);
        float d = axes[i][0] * dec[0] + axes[i][1] * dec[1] + axes[i][2] * dec[2];
        CHECK(d > 0.99999f);
    }

    // Dense 4096-point Fibonacci sphere angular precision (< 0.02 degrees)
    const uint32_t k_samples = 4096u;
    const float k_golden = 2.39996322972865332f;
    float max_deg = 0.0f;
    for (uint32_t i = 0u; i < k_samples; ++i) {
        float z = 1.0f - (2.0f * ((float)i + 0.5f)) / (float)k_samples;
        float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
        float phi = k_golden * (float)i;
        float nx = r * cosf(phi);
        float ny = r * sinf(phi);
        float nz = z;

        int16_t oct[2] = {0};
        float dec[3] = {0};
        scene_oct_encode_snorm16(nx, ny, nz, oct);
        scene_oct_decode_snorm16(oct, dec);

        // Measure exact angle via cross-product norm to avoid float32 acosf(1 - eps) ULP loss
        double dx = (double)ny * (double)dec[2] - (double)nz * (double)dec[1];
        double dy = (double)nz * (double)dec[0] - (double)nx * (double)dec[2];
        double dz = (double)nx * (double)dec[1] - (double)ny * (double)dec[0];
        double sin_a = sqrt(dx * dx + dy * dy + dz * dz);
        double cos_a = (double)nx * (double)dec[0] + (double)ny * (double)dec[1] + (double)nz * (double)dec[2];
        float deg = (float)(atan2(sin_a, cos_a) * (180.0 / 3.14159265358979323846));
        if (deg > max_deg) {
            max_deg = deg;
        }
    }
    CHECK(max_deg < 0.02f);
}

static void test_pos16_and_rgba8(void) {
    const float origin[3] = { -125.5f, 40.0f, -8.25f };
    const float scale[3]  = {  251.0f, 92.5f,  16.5f };

    for (uint32_t i = 0u; i <= 256u; ++i) {
        float t = (float)i / 256.0f;
        float pos[3] = {
            origin[0] + scale[0] * t,
            origin[1] + scale[1] * (1.0f - t),
            origin[2] + scale[2] * (0.5f + 0.5f * sinf(t * 6.2831853f)),
        };
        uint16_t q[3] = {0};
        float rec[3] = {0};
        scene_quantize_pos16(pos, origin, scale, q);
        scene_dequantize_pos16(q, origin, scale, rec);

        for (int axis = 0; axis < 3; ++axis) {
            float max_step = scale[axis] / 65535.0f + 1.0e-5f;
            CHECK(fabsf(rec[axis] - pos[axis]) <= max_step);
        }
    }

    uint32_t packed = scene_pack_rgba8(1.0f, 0.5f, 0.0f, 0.30f);
    CHECK((packed & 0xFFu) == 255u);
    CHECK(((packed >> 8u) & 0xFFu) >= 127u && ((packed >> 8u) & 0xFFu) <= 128u);
    CHECK(((packed >> 16u) & 0xFFu) == 0u);
    CHECK(((packed >> 24u) & 0xFFu) >= 76u && ((packed >> 24u) & 0xFFu) <= 77u);
}

static void test_visbuf_bitpacking(void) {
    uint32_t inst = 999u, ml = 999u, prim = 999u;
    // 0u is strictly the background sky sentinel
    CHECK(!scene_visbuf_unpack(0u, &inst, &ml, &prim));

    // Instance 0, meshlet 0, prim 0 must NOT collide with 0u!
    uint32_t id0 = scene_visbuf_pack(0u, 0u, 0u);
    CHECK(id0 != 0u);
    CHECK(scene_visbuf_unpack(id0, &inst, &ml, &prim));
    CHECK(inst == 0u && ml == 0u && prim == 0u);

    // Upper range limits: instance 16382 ((16382+1)=16383=0x3FFF), meshlet 2047 (0x7FF), prim 123
    uint32_t id_max = scene_visbuf_pack(16382u, 2047u, 123u);
    CHECK(scene_visbuf_unpack(id_max, &inst, &ml, &prim));
    CHECK(inst == 16382u && ml == 2047u && prim == 123u);
}

static void test_analytical_barycentrics(void) {
    // Perspective triangle with distinct W clip coordinates
    const float c0[4] = { -0.45f * 1.3f, -0.35f * 1.3f, 0.25f * 1.3f, 1.3f };
    const float c1[4] = {  0.60f * 2.1f, -0.15f * 2.1f, 0.55f * 2.1f, 2.1f };
    const float c2[4] = {  0.05f * 1.7f,  0.70f * 1.7f, 0.40f * 1.7f, 1.7f };
    const float vw = 1280.0f;
    const float vh = 720.0f;
    const float px = 672.5f;
    const float py = 348.5f;

    SceneBarycentrics2D b = scene_eval_barycentrics_2d(c0, c1, c2, px, py, vw, vh);
    CHECK(b.valid);

    float sum = b.lambda[0] + b.lambda[1] + b.lambda[2];
    CHECK(fabsf(sum - 1.0f) < 1.0e-5f);
    CHECK(b.lambda[0] > 0.0f && b.lambda[1] > 0.0f && b.lambda[2] > 0.0f);
    CHECK(b.z_ndc > 0.20f && b.z_ndc < 0.60f);

    // Central finite-difference verification of d(lambda)/dx and d(lambda)/dy
    const float eps = 0.25f;
    SceneBarycentrics2D bx_p = scene_eval_barycentrics_2d(c0, c1, c2, px + eps, py, vw, vh);
    SceneBarycentrics2D bx_m = scene_eval_barycentrics_2d(c0, c1, c2, px - eps, py, vw, vh);
    SceneBarycentrics2D by_p = scene_eval_barycentrics_2d(c0, c1, c2, px, py + eps, vw, vh);
    SceneBarycentrics2D by_m = scene_eval_barycentrics_2d(c0, c1, c2, px, py - eps, vw, vh);

    for (int k = 0; k < 3; ++k) {
        float fd_x = (bx_p.lambda[k] - bx_m.lambda[k]) / (2.0f * eps);
        float fd_y = (by_p.lambda[k] - by_m.lambda[k]) / (2.0f * eps);
        CHECK(fabsf(b.dlambda_dx[k] - fd_x) < 1.0e-4f);
        CHECK(fabsf(b.dlambda_dy[k] - fd_y) < 1.0e-4f);
    }

    // Degenerate collinear triangle must report valid == false
    const float d0[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
    const float d1[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    const float d2[4] = { 1.0f, 1.0f, 0.5f, 1.0f };
    SceneBarycentrics2D deg = scene_eval_barycentrics_2d(d0, d1, d2, 640.5f, 360.5f, vw, vh);
    CHECK(!deg.valid);
}

static void emit_torus_point(float u, float v, float major_r, float minor_r, SceneCorner *out) {
    float cu = cosf(u), su = sinf(u);
    float cv = cosf(v), sv = sinf(v);
    float ring = major_r + minor_r * cv;
    out->x  = ring * cu;
    out->y  = minor_r * sv;
    out->z  = ring * su;
    out->nx = cv * cu;
    out->ny = sv;
    out->nz = cv * su;
    out->r  = 0.85f;
    out->g  = 0.45f;
    out->b  = 0.25f;
}

static void test_meshlet_builder_invariants(void) {
    // Generate a 32x24 smooth procedural torus = 1536 triangles (4608 raw corners)
    const uint32_t nu = 32u;
    const uint32_t nv = 24u;
    const uint32_t tri_total = nu * nv * 2u;
    const uint32_t corner_total = tri_total * 3u;
    const float two_pi = 6.2831853071795864769f;

    SceneCorner *corners = calloc(corner_total, sizeof(*corners));
    CHECK(corners != nullptr);
    if (corners == nullptr) {
        return;
    }

    uint32_t w = 0u;
    for (uint32_t iu = 0u; iu < nu; ++iu) {
        float u0 = two_pi * (float)iu / (float)nu;
        float u1 = two_pi * (float)(iu + 1u) / (float)nu;
        for (uint32_t iv = 0u; iv < nv; ++iv) {
            float v0 = two_pi * (float)iv / (float)nv;
            float v1 = two_pi * (float)(iv + 1u) / (float)nv;
            SceneCorner p00, p10, p11, p01;
            emit_torus_point(u0, v0, 60.0f, 22.0f, &p00);
            emit_torus_point(u1, v0, 60.0f, 22.0f, &p10);
            emit_torus_point(u1, v1, 60.0f, 22.0f, &p11);
            emit_torus_point(u0, v1, 60.0f, 22.0f, &p01);

            corners[w++] = p00;
            corners[w++] = p10;
            corners[w++] = p11;

            corners[w++] = p00;
            corners[w++] = p11;
            corners[w++] = p01;
        }
    }
    CHECK(w == corner_total);

    SceneMeshletBuild build = {0};
    CHECK(scene_build_meshlets(corners, corner_total, 0.30f, &build) == 0);
    CHECK(build.meshlet_count >= 13u && build.meshlet_count <= 40u);
    CHECK(build.triangle_count == tri_total);
    // Smooth torus welding should compress 4608 corners down to well under 1600 unique meshlet verts!
    CHECK(build.vertex_count < 1600u);

    uint32_t sum_tris = 0u;
    uint32_t cone_enabled_count = 0u;
    for (uint32_t m = 0u; m < build.meshlet_count; ++m) {
        const SceneMeshlet *ml = &build.meshlets[m];
        CHECK(ml->vert_count >= 3u && ml->vert_count <= K_SCENE_MESHLET_MAX_VERTS);
        CHECK(ml->tri_count >= 1u && ml->tri_count <= K_SCENE_MESHLET_MAX_TRIS);
        CHECK(ml->radius > 0.0f);
        sum_tris += ml->tri_count;

        float axis[3] = {0};
        scene_oct_decode_snorm16(ml->cone_axis_oct, axis);
        float cutoff = (float)ml->cone_cutoff_s16 / 32767.0f;
        if (ml->cone_cutoff_s16 > -32767) {
            cone_enabled_count++;
        }

        for (uint32_t t = 0u; t < ml->tri_count; ++t) {
            uint32_t packed_tri = build.triangles[ml->tri_offset + t];
            uint32_t i0 = packed_tri & 0xFFu;
            uint32_t i1 = (packed_tri >> 8u) & 0xFFu;
            uint32_t i2 = (packed_tri >> 16u) & 0xFFu;
            CHECK(i0 < ml->vert_count && i1 < ml->vert_count && i2 < ml->vert_count);

            const ScenePackedVertex *v0 = &build.vertices[ml->vert_offset + i0];
            const ScenePackedVertex *v1 = &build.vertices[ml->vert_offset + i1];
            const ScenePackedVertex *v2 = &build.vertices[ml->vert_offset + i2];

            uint16_t q0[3] = { v0->px, v0->py, v0->pz };
            uint16_t q1[3] = { v1->px, v1->py, v1->pz };
            uint16_t q2[3] = { v2->px, v2->py, v2->pz };
            float p0[3], p1[3], p2[3];
            scene_dequantize_pos16(q0, ml->origin, ml->scale, p0);
            scene_dequantize_pos16(q1, ml->origin, ml->scale, p1);
            scene_dequantize_pos16(q2, ml->origin, ml->scale, p2);

            // Conservative post-quantization bounding sphere check
            const float *pts[3] = { p0, p1, p2 };
            for (int k = 0; k < 3; ++k) {
                float dx = pts[k][0] - ml->center[0];
                float dy = pts[k][1] - ml->center[1];
                float dz = pts[k][2] - ml->center[2];
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                CHECK(dist <= ml->radius + 1.0e-3f);
            }

            // Conservative normal cone check: if a triangle is front-facing to view vector w
            // (dot(n_tri, w) > 0), then dot(axis, w) MUST NOT be < cutoff!
            // Equivalently, half-angle theta between axis and n_tri satisfies sin(theta) <= -cutoff
            // whenever cutoff < 0 (i.e. dot(axis, n_tri) >= sqrt(1 - cutoff^2) - slack).
            if (ml->cone_cutoff_s16 > -32767) {
                float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
                float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
                float cx = e1y * e2z - e1z * e2y;
                float cy = e1z * e2x - e1x * e2z;
                float cz = e1x * e2y - e1y * e2x;
                float len = sqrtf(cx * cx + cy * cy + cz * cz);
                if (len > 1.0e-6f) {
                    cx /= len; cy /= len; cz /= len;
                    float cos_theta = cx * axis[0] + cy * axis[1] + cz * axis[2];
                    float sin_theta = sqrtf(fmaxf(0.0f, 1.0f - cos_theta * cos_theta));
                    CHECK(cos_theta > 0.0f);
                    CHECK(-sin_theta >= cutoff - 1.0e-4f);
                }
            }
        }
    }
    CHECK(sum_tris == tri_total);
    CHECK(cone_enabled_count > 0u);

    scene_meshlet_build_free(&build);
    free(corners);
}

int main(void) {
    CHECK(scene_self_check());
    test_octahedral_snorm16();
    test_pos16_and_rgba8();
    test_visbuf_bitpacking();
    test_analytical_barycentrics();
    test_meshlet_builder_invariants();

    if (s_failures != 0) {
        fprintf(stderr, "[test_scene] FAILED (%d failures)\n", s_failures);
        return 1;
    }
    printf("[test_scene] All Milestone 3 meshlet, octahedral snorm16, VisBuf & barycentric tests passed.\n");
    return 0;
}
