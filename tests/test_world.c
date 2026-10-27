#include "frame/grid_math.h"
#include "world/world_int.h"
#include <math.h>
#include <stdio.h>

// Focused regression tests for (1) the universal metric-grid math and
// (2) WorldOps-dispatched stats/clicks on the world table: vertices
// count only live objects through their ops, on_click runs only for
// the picked object's own ops. Exercises the real table.c/spin.c.

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

// ------------------------------------------------------------- grid math

static void test_grid_math(void) {
    PlaneGrid g = plane_grid_for(1280, 720, 1.0f);
    // Medium decade near 45% of 720 px = 324 world px -> 10^2.
    CHECK(g.cell_med == 100.0f);
    CHECK(g.level_frac >= 0.0f && g.level_frac < 1.0f);
    // Span must cover half-diagonal at zoom 1 plus the depth range.
    CHECK(g.span >= 0.5f * sqrtf(1280.0f * 1280.0f + 720.0f * 720.0f) + 4096.0f - 1.0f);

    // Zooming in 10x subdivides exactly one decade finer.
    PlaneGrid in = plane_grid_for(1280, 720, 10.0f);
    CHECK(in.cell_med == 10.0f);
    // Zooming out 100x coarsens and the span grows to cover the plane.
    PlaneGrid out = plane_grid_for(1280, 720, 0.01f);
    CHECK(out.cell_med == 10000.0f);
    CHECK(out.span > g.span);
    // Degenerate inputs stay finite.
    PlaneGrid zero = plane_grid_for(0, 0, 0.0f);
    CHECK(isfinite(zero.cell_med) && isfinite(zero.span));
}

// ------------------------------------------------------------ fake objects

static int s_clicks_a = 0;
static bool fake_click(WorldObj *o, Coord hit) {
    (void)o;
    (void)hit;
    s_clicks_a++;
    return true;
}
static uint32_t fake_vertices_a(const WorldObj *o) {
    (void)o;
    return 111u;
}
static uint32_t fake_vertices_b(const WorldObj *o) {
    (void)o;
    return 222u;
}
static const WorldOps k_ops_a = { .on_click = fake_click, .vertices = fake_vertices_a };
static const WorldOps k_ops_b = { .vertices = fake_vertices_b };

static void test_table_stats(void) {
    table_reset();
    CHECK(table_alive_vertices() == 0);
    CHECK(table_add(&k_ops_a, nullptr, (Coord){0, 0, 0}, nullptr) >= 0);
    CHECK(table_add(&k_ops_b, nullptr, (Coord){0, 0, 0}, nullptr) >= 0);
    CHECK(table_alive_vertices() == 333u);

    // Killing one object drops only its own count.
    WorldObj *b = table_at(1);
    CHECK(b != nullptr);
    b->alive = false;
    CHECK(table_alive_vertices() == 111u);

    // on_click belongs to the picked object's ops alone.
    WorldObj *a = table_at(0);
    CHECK(a != nullptr && a->ops->on_click != nullptr);
    CHECK(a->ops->on_click(a, (Coord){0, 0, 0}));
    CHECK(s_clicks_a == 1);
    CHECK(b->ops->on_click == nullptr);
    table_reset();
    CHECK(table_alive_vertices() == 0);
}

int main(void) {
    test_grid_math();
    test_table_stats();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_world: all checks passed\n");
    return 0;
}
