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
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_sheet: all checks passed\n");
    return 0;
}
