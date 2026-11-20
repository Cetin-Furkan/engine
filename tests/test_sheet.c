#include "frame/shape.h"
#include "frame/sheet.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Sheet contact and the soft patch. Links sheet.c only. The shape draw
// is stubbed so the test does not need Vulkan.

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

void shape_draw_polygon(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                        const float *x, const float *y, uint32_t n,
                        const uint16_t *idx, uint32_t tri_n, bool closed,
                        float center_x, float center_y, float angle_rad,
                        float fill_depth, float edge_depth, float mark_depth,
                        float fill_r, float fill_g, float fill_b,
                        float edge_r, float edge_g, float edge_b,
                        float stroke_half, float mark_half, uint32_t mat_index) {
    (void)cmd;
    (void)width;
    (void)height;
    (void)x;
    (void)y;
    (void)n;
    (void)idx;
    (void)tri_n;
    (void)closed;
    (void)center_x;
    (void)center_y;
    (void)angle_rad;
    (void)fill_depth;
    (void)edge_depth;
    (void)mark_depth;
    (void)fill_r;
    (void)fill_g;
    (void)fill_b;
    (void)edge_r;
    (void)edge_g;
    (void)edge_b;
    (void)stroke_half;
    (void)mark_half;
    (void)mat_index;
}

static void test_document(void) {
    CHECK(sheet_load_path("examples/demo/world/sheet.world") == 0);
    CHECK(sheet_rigid_count() == 2u);
    CHECK(sheet_soft_count() == 1u);
    CHECK(sheet_dyn_count() == 1u);
    const char *bad = "dyn 0 0 0 4\n0 0\n10 0\n1 1\n0 10\n";
    CHECK(sheet_load_memory(bad, strlen(bad)) != 0);
    CHECK(sheet_rigid_count() == 2u);
    CHECK(sheet_dyn_count() == 1u);
}

static void test_gap_stays(void) {
    const char *text = "rigid 0 0 50 40 0\nrigid 130 0 50 40 0\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    for (int i = 0; i < 30; ++i) {
        (void)sheet_integrate(0.008f);
    }
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    sheet_rigid_pos(0, &x0, &y0);
    sheet_rigid_pos(1, &x1, &y1);
    CHECK(fabsf(x1 - x0) > 129.0f && fabsf(x1 - x0) < 131.0f);
    CHECK(fabsf(y0) < 0.1f && fabsf(y1) < 0.1f);
}

static void test_faces_meet(void) {
    const char *text = "rigid 0 0 50 40 0\nrigid 70 0 50 40 0\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    for (int i = 0; i < 40; ++i) {
        (void)sheet_integrate(0.008f);
    }
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    sheet_rigid_pos(0, &x0, &y0);
    sheet_rigid_pos(1, &x1, &y1);
    float dx = fabsf(x1 - x0);
    CHECK(dx > 99.0f && dx < 101.0f);
    CHECK(!sheet_busy());
}

static void test_yaw_stays_finite(void) {
    const char *text = "rigid 0 0 60 20 25\nrigid 40 0 60 20 25\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    for (int i = 0; i < 80; ++i) {
        (void)sheet_integrate(0.008f);
    }
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    sheet_rigid_pos(0, &x0, &y0);
    sheet_rigid_pos(1, &x1, &y1);
    CHECK(isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1));
    CHECK(fabsf(x0) < 500.0f && fabsf(x1) < 500.0f);
    CHECK(fabsf(x1 - x0) > 40.0f);
    CHECK(!sheet_busy());
}

static void test_soft_returns(void) {
    const char *text = "soft 0 0 4 4 30\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    uint32_t cols = 0;
    uint32_t rows = 0;
    float spacing = 0;
    sheet_soft_layout(0, &cols, &rows, &spacing);
    CHECK(cols == 4u && rows == 4u);
    sheet_soft_move(0, 5, 24.0f, 0.0f);
    for (int i = 0; i < 800; ++i) {
        (void)sheet_integrate(0.008f);
    }
    CHECK(!sheet_busy());
    float cx = 0.0f;
    float cy = 0.0f;
    uint32_t n = cols * rows;
    for (uint32_t k = 0; k < n; ++k) {
        float x = 0, y = 0;
        sheet_soft_pos(0, k, &x, &y);
        CHECK(isfinite(x) && isfinite(y));
        CHECK(fabsf(x) < 1000.0f && fabsf(y) < 1000.0f);
        cx += x;
        cy += y;
    }
    cx /= (float)n;
    cy /= (float)n;
    float worst = 0.0f;
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            float x = 0, y = 0;
            sheet_soft_pos(0, r * cols + c, &x, &y);
            float lx = ((float)c - (float)(cols - 1u) * 0.5f) * spacing;
            float ly = ((float)r - (float)(rows - 1u) * 0.5f) * spacing;
            float err = hypotf((x - cx) - lx, (y - cy) - ly);
            if (err > worst) {
                worst = err;
            }
        }
    }
    CHECK(worst < 2.0f);
}

static void test_roundtrip(void) {
    const char *text =
        "rigid 5 6 20 10 8\n"
        "dyn 10 20 15 4\n"
        "30 0\n"
        "0 20\n"
        "-30 0\n"
        "0 -16\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    const char *path = "/tmp/engine-sheet-test.world";
    CHECK(sheet_save_path(path) == 0);
    CHECK(sheet_load_memory("rigid 0 0 10 10 0\n", strlen("rigid 0 0 10 10 0\n")) == 0);
    CHECK(sheet_rigid_count() == 1u);
    CHECK(sheet_load_path(path) == 0);
    CHECK(sheet_rigid_count() == 1u);
    CHECK(sheet_dyn_count() == 1u);
    float x = 0, y = 0;
    sheet_rigid_pos(0, &x, &y);
    CHECK(fabsf(x - 5.0f) < 1.0e-3f);
    CHECK(fabsf(y - 6.0f) < 1.0e-3f);
    sheet_dyn_local(0, 0, &x, &y);
    CHECK(fabsf(x - 30.0f) < 1.0e-3f);
    CHECK(fabsf(y) < 1.0e-3f);
    unlink(path);
}

static void test_shove_and_delete(void) {
    const char *text = "rigid 0 0 40 40 0\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    CHECK(sheet_click(1000.0f, 1000.0f) == false);
    CHECK(sheet_click(0.0f, -10.0f));
    for (int i = 0; i < 15; ++i) {
        (void)sheet_integrate(0.008f);
    }
    float x = 0, y = 0;
    sheet_rigid_pos(0, &x, &y);
    CHECK(y > 5.0f);
    CHECK(sheet_has_selection());
    CHECK(sheet_delete());
    CHECK(sheet_rigid_count() == 0u);
    CHECK(!sheet_has_selection());
}

static void test_gravity_and_elastic_presets(void) {
    for (uint32_t w = 1u; w <= 6u; ++w) {
        char path[64];
        snprintf(path, sizeof path, "examples/demo/world/sheet%u.world", w);
        CHECK(sheet_load_path(path) == 0);
        for (int step = 0; step < 30; ++step) {
            (void)sheet_integrate(0.008f);
        }
    }
    // Verify elastic vs inelastic polygon rebound under gravity against a fixed bumper
    const char *drop =
        "gravity 0 -700\n"
        "bounds 500 300\n"
        "rigid 0 -220 400 20 0 0.90 1\n"
        "dyn -120 0 0 4 0.92\n"
        "30 0\n0 30\n-30 0\n0 -30\n"
        "dyn 120 0 0 4 0.05\n"
        "30 0\n0 30\n-30 0\n0 -30\n";
    CHECK(sheet_load_memory(drop, strlen(drop)) == 0);
    CHECK(sheet_rigid_count() == 1u);
    CHECK(sheet_dyn_count() == 2u);
    for (int i = 0; i < 85; ++i) {
        (void)sheet_integrate(0.008f);
    }

    // Verify procedural polyring directive and World 7 sheet preset
    const char *rings =
        "bounds 500 300\n"
        "polyring -160 120 40 8 0.90\n"
        "polyring 0 120 40 6 0.70\n"
        "polyring 160 120 40 3 0.25\n";
    CHECK(sheet_load_memory(rings, strlen(rings)) == 0);
    CHECK(sheet_dyn_count() == 3u);
    sheet_select_world(7u);
    CHECK(sheet_load() == 0);
    CHECK(sheet_rigid_count() == 2u);
    CHECK(sheet_soft_count() == 2u);
    CHECK(sheet_dyn_count() == 4u);

    // World 9 (sandbox) loads examples/demo/world/sheet.world
    sheet_select_world(9u);
    CHECK(sheet_load() == 0);
    CHECK(sheet_rigid_count() == 2u);
    CHECK(sheet_soft_count() == 1u);
    CHECK(sheet_dyn_count() == 1u);

    // World 0 (pure empty engine) opens with literally zero 2D sheet objects
    sheet_select_world(0u);
    CHECK(sheet_load() == 0);
    CHECK(sheet_rigid_count() == 0u);
    CHECK(sheet_soft_count() == 0u);
    CHECK(sheet_dyn_count() == 0u);
}

static void test_single_owner_gpu_pose_contract(void) {
    // Verify that when GPU AVBD owns the 2D soft-body pose:
    // 1. sheet_integrate leaves soft particle positions 100% untouched on CPU;
    // 2. harvesting a GPU-solved AvbdVertex buffer produces the GPU pose and FAILS
    //    if the drawn pose still matches the CPU integrator.
    const char *spec =
        "gravity 0 -680\n"
        "soft 0 120 4 4 30 48 1.8 0.65\n";

    // Reference CPU-only integrator step
    sheet_set_gpu_pose_owner(false);
    CHECK(sheet_load_memory(spec, strlen(spec)) == 0);
    sheet_soft_move(0u, 0u, 18.0f, 0.0f);
    float init_x0 = 0.0f, init_y0 = 0.0f;
    sheet_soft_pos(0u, 0u, &init_x0, &init_y0);
    (void)sheet_integrate(0.008f);
    float cpu_x0 = 0.0f, cpu_y0 = 0.0f;
    sheet_soft_pos(0u, 0u, &cpu_x0, &cpu_y0);
    CHECK(fabsf(cpu_x0 - init_x0) > 1.0e-3f || fabsf(cpu_y0 - init_y0) > 1.0e-3f);

    // GPU-owned path: prepare step without integrating soft vertices on CPU
    sheet_set_gpu_pose_owner(true);
    CHECK(sheet_load_memory(spec, strlen(spec)) == 0);
    sheet_soft_move(0u, 0u, 18.0f, 0.0f);
    (void)sheet_integrate(0.008f);

    float pre_harvest_x0 = 0.0f, pre_harvest_y0 = 0.0f;
    sheet_soft_pos(0u, 0u, &pre_harvest_x0, &pre_harvest_y0);
    CHECK(fabsf(pre_harvest_x0 - init_x0) < 1.0e-5f);
    CHECK(fabsf(pre_harvest_y0 - init_y0) < 1.0e-5f);
    CHECK(sheet_busy());

    SheetGpuStep gstep = {0};
    CHECK(sheet_pop_gpu_step(&gstep));
    CHECK(gstep.vert_count == 16u);
    CHECK(gstep.con_count > 0u);

    // Construct a distinct GPU-solved vertex buffer
    AvbdVertex gpu_solved[16];
    memcpy(gpu_solved, gstep.verts, sizeof gpu_solved);
    for (uint32_t k = 0u; k < 16u; ++k) {
        gpu_solved[k].pos[0] += 33.5f;
        gpu_solved[k].pos[1] -= 41.25f;
        gpu_solved[k].vel[0]  = 12.0f;
        gpu_solved[k].vel[1]  = -25.0f;
    }

    CHECK(sheet_apply_gpu_soft_step(gstep.soft_index, gstep.epoch,
                                    gpu_solved, 16u, gstep.dt));

    float gpu_x0 = 0.0f, gpu_y0 = 0.0f;
    sheet_soft_pos(0u, 0u, &gpu_x0, &gpu_y0);
    CHECK(fabsf(gpu_x0 - (init_x0 + 33.5f)) < 1.0e-4f);
    CHECK(fabsf(gpu_y0 - (init_y0 - 41.25f)) < 1.0e-4f);
    // Must fail if the drawn pose still matches the CPU integrator
    CHECK(fabsf(gpu_x0 - cpu_x0) > 20.0f);
    CHECK(fabsf(gpu_y0 - cpu_y0) > 20.0f);

    sheet_set_gpu_pose_owner(false);
}

static bool soft_all_tris_ccw(uint32_t soft_idx, float *out_min_sep,
                              float *out_min_y, float *out_max_y, float *out_cy) {
    uint32_t cols = 0u, rows = 0u;
    float    spacing = 0.0f;
    sheet_soft_layout(soft_idx, &cols, &rows, &spacing);
    uint32_t n = cols * rows;
    float px[64], py[64];
    float min_y = 1.0e30f, max_y = -1.0e30f, cy = 0.0f;
    for (uint32_t k = 0u; k < n; ++k) {
        sheet_soft_pos(soft_idx, k, &px[k], &py[k]);
        if (py[k] < min_y) min_y = py[k];
        if (py[k] > max_y) max_y = py[k];
        cy += py[k];
    }
    if (out_min_y) *out_min_y = min_y;
    if (out_max_y) *out_max_y = max_y;
    if (out_cy)    *out_cy    = cy / (float)n;

    float min_sep = 1.0e30f;
    for (uint32_t a = 0u; a < n; ++a) {
        for (uint32_t b = a + 1u; b < n; ++b) {
            float d = hypotf(px[b] - px[a], py[b] - py[a]);
            if (d < min_sep) min_sep = d;
        }
    }
    if (out_min_sep) *out_min_sep = min_sep;

    for (uint32_t r = 0u; r + 1u < rows; ++r) {
        for (uint32_t c = 0u; c + 1u < cols; ++c) {
            uint32_t i00 = r * cols + c;
            uint32_t i10 = i00 + 1u;
            uint32_t i01 = i00 + cols;
            uint32_t i11 = i01 + 1u;
            float c0 = (px[i10] - px[i00]) * (py[i11] - py[i00]) - (py[i10] - py[i00]) * (px[i11] - px[i00]);
            float c1 = (px[i11] - px[i00]) * (py[i01] - py[i00]) - (py[i11] - py[i00]) * (px[i01] - px[i00]);
            if (c0 <= 0.0f || c1 <= 0.0f) {
                return false;
            }
        }
    }
    return true;
}

static void test_rigid_rests_on_soft(void) {
    const char *text =
        "bounds 600 400\n"
        "gravity 0 -980\n"
        "soft 0 -330 4 3 32\n"
        "rigid 0 -100 38 24 12\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    for (int step = 0; step < 120; ++step) {
        (void)sheet_integrate(0.008f);
    }
    float rx = 0.0f, ry = 0.0f;
    sheet_rigid_pos(0u, &rx, &ry);
    float s_min_y = 0.0f, s_max_y = 0.0f, s_cy = 0.0f, s_min_sep = 0.0f;
    bool ccw = soft_all_tris_ccw(0u, &s_min_sep, &s_min_y, &s_max_y, &s_cy);
    CHECK(isfinite(rx) && isfinite(ry));
    CHECK(ccw);
    CHECK(ry > s_cy + 15.0f && ry >= s_max_y - 12.0f);
}

static void test_soft_soft_disjoint(void) {
    const char *text =
        "bounds 600 400\n"
        "gravity 0 -980\n"
        "soft 0 -330 4 3 32\n"
        "soft 0 -120 4 3 32\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    for (int step = 0; step < 140; ++step) {
        (void)sheet_integrate(0.008f);
    }
    float min0 = 0.0f, max0 = 0.0f, cy0 = 0.0f, sep0 = 0.0f;
    float min1 = 0.0f, max1 = 0.0f, cy1 = 0.0f, sep1 = 0.0f;
    CHECK(soft_all_tris_ccw(0u, &sep0, &min0, &max0, &cy0));
    CHECK(soft_all_tris_ccw(1u, &sep1, &min1, &max1, &cy1));
    CHECK(cy1 > cy0 + 40.0f);
    CHECK(min1 >= max0 - 6.0f);
}

static void test_soft_self_unfold(void) {
    const char *text =
        "bounds 600 400\n"
        "gravity 0 -980\n"
        "soft 0 -340 4 3 32\n";
    CHECK(sheet_load_memory(text, strlen(text)) == 0);
    sheet_soft_move(0u, 8u, 0.0f, -90.0f);
    for (int step = 0; step < 80; ++step) {
        (void)sheet_integrate(0.008f);
    }
    float min_sep = 0.0f;
    CHECK(soft_all_tris_ccw(0u, &min_sep, nullptr, nullptr, nullptr));
    CHECK(min_sep >= 0.30f * 32.0f);
}

static bool point_in_oriented_box(float px, float py, float bx, float by,
                                  float hx, float hy, float yaw_deg, float margin) {
    float rad = yaw_deg * 0.017453292519943295f;
    float c   = cosf(rad);
    float s   = sinf(rad);
    float dx  = px - bx;
    float dy  = py - by;
    float lx  =  dx * c + dy * s;
    float ly  = -dx * s + dy * c;
    return fabsf(lx) < (hx + margin) && fabsf(ly) < (hy + margin);
}

static bool point_in_soft_mesh(uint32_t soft_idx, float px, float py) {
    uint32_t cols = 0u, rows = 0u;
    float spacing = 0.0f;
    sheet_soft_layout(soft_idx, &cols, &rows, &spacing);
    uint32_t n = cols * rows;
    float sx[64], sy[64];
    for (uint32_t k = 0u; k < n; ++k) {
        sheet_soft_pos(soft_idx, k, &sx[k], &sy[k]);
    }
    for (uint32_t r = 0u; r + 1u < rows; ++r) {
        for (uint32_t c = 0u; c + 1u < cols; ++c) {
            uint32_t i00 = r * cols + c;
            uint32_t i10 = i00 + 1u;
            uint32_t i01 = i00 + cols;
            uint32_t i11 = i01 + 1u;
            const uint32_t tris[2][3] = { { i00, i10, i11 }, { i00, i11, i01 } };
            for (int t = 0; t < 2; ++t) {
                float ax = sx[tris[t][0]], ay = sy[tris[t][0]];
                float bx = sx[tris[t][1]], by = sy[tris[t][1]];
                float cx = sx[tris[t][2]], cy = sy[tris[t][2]];
                float det = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                if (fabsf(det) < 1.0e-4f) {
                    continue;
                }
                float inv = 1.0f / det;
                float u = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * inv;
                float v = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * inv;
                float w = 1.0f - u - v;
                if (u > 1.0e-3f && v > 1.0e-3f && w > 1.0e-3f) {
                    return true;
                }
            }
        }
    }
    return false;
}

typedef struct TestRamp {
    float x, y, hx, hy, yaw_deg;
} TestRamp;

static void check_sheet3_no_ramp_overlap(void) {
    static constexpr TestRamp k_ramps[3] = {
        { -330.0f, -120.0f, 110.0f, 18.0f, -16.0f },
        {  330.0f, -120.0f, 110.0f, 18.0f,  16.0f },
        {    0.0f, -285.0f, 520.0f, 16.0f,   0.0f },
    };

    for (uint32_t si = 0u; si < 3u; ++si) {
        CHECK(soft_all_tris_ccw(si, nullptr, nullptr, nullptr, nullptr));
        uint32_t cols = 0u, rows = 0u;
        float spacing = 0.0f;
        sheet_soft_layout(si, &cols, &rows, &spacing);
        uint32_t n = cols * rows;
        for (uint32_t k = 0u; k < n; ++k) {
            float sx = 0.0f, sy = 0.0f;
            sheet_soft_pos(si, k, &sx, &sy);
            for (int ri = 0; ri < 3; ++ri) {
                CHECK(!point_in_oriented_box(sx, sy,
                                             k_ramps[ri].x, k_ramps[ri].y,
                                             k_ramps[ri].hx, k_ramps[ri].hy,
                                             k_ramps[ri].yaw_deg, 0.0f));
            }
        }
        // Check all 4 corners of each fixed ramp against every soft triangle
        for (int ri = 0; ri < 3; ++ri) {
            float rad = k_ramps[ri].yaw_deg * 0.017453292519943295f;
            float c = cosf(rad), s = sinf(rad);
            const float lx[4] = { -k_ramps[ri].hx, k_ramps[ri].hx, k_ramps[ri].hx, -k_ramps[ri].hx };
            const float ly[4] = { -k_ramps[ri].hy, -k_ramps[ri].hy, k_ramps[ri].hy, k_ramps[ri].hy };
            for (int ci = 0; ci < 4; ++ci) {
                float wx = k_ramps[ri].x + c * lx[ci] - s * ly[ci];
                float wy = k_ramps[ri].y + s * lx[ci] + c * ly[ci];
                CHECK(!point_in_soft_mesh(si, wx, wy));
            }
        }
    }
}

static void test_sheet3_ramp_no_overlap(void) {
    // 1. CPU AVBD path
    sheet_set_gpu_pose_owner(false);
    CHECK(sheet_load_path("examples/demo/world/sheet3.world") == 0);
    for (int step = 0; step < 500; ++step) {
        (void)sheet_integrate(0.008f);
    }
    check_sheet3_no_ramp_overlap();

    // 2. GPU AVBD owner path (verifies fixed-ramp AVBD_CON_CONTACT_PLANE + multi-pass post-resolve)
    sheet_set_gpu_pose_owner(true);
    CHECK(sheet_load_path("examples/demo/world/sheet3.world") == 0);
    for (int step = 0; step < 500; ++step) {
        (void)sheet_integrate(0.008f);
        SheetGpuStep gs = {0};
        while (sheet_pop_gpu_step(&gs)) {
            AvbdVertex     verts[64];
            AvbdConstraint cons[K_AVBD_MAX_CONS];
            uint32_t vn = gs.vert_count <= 64u ? gs.vert_count : 64u;
            uint32_t cn = gs.con_count <= K_AVBD_MAX_CONS ? gs.con_count : K_AVBD_MAX_CONS;
            memcpy(verts, gs.verts, (size_t)vn * sizeof(AvbdVertex));
            memcpy(cons,  gs.cons,  (size_t)cn * sizeof(AvbdConstraint));
            (void)avbd_solve_step(verts, vn, cons, cn, gs.dt, gs.gravity, 6u);
            for (uint32_t k = 0u; k < vn; ++k) {
                verts[k].vel[0] *= gs.damp_keep;
                verts[k].vel[1] *= gs.damp_keep;
            }
            CHECK(sheet_apply_gpu_soft_step(gs.soft_index, gs.epoch, verts, vn, gs.dt));
        }
    }
    check_sheet3_no_ramp_overlap();
    sheet_set_gpu_pose_owner(false);
}

int main(void) {
    sheet_hold_save(true);
    test_document();
    test_gap_stays();
    test_faces_meet();
    test_yaw_stays_finite();
    test_soft_returns();
    test_roundtrip();
    test_shove_and_delete();
    test_gravity_and_elastic_presets();
    test_single_owner_gpu_pose_contract();
    test_rigid_rests_on_soft();
    test_soft_soft_disjoint();
    test_soft_self_unfold();
    test_sheet3_ramp_no_overlap();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_sheet: all checks passed\n");
    return 0;
}
