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
    CHECK(zero.span_mod >= 0.0f && zero.span_mod < zero.coarse + 1.0e-6f);

    // span splits back into whole coarse cells plus a remainder.
    PlaneGrid far = plane_grid_for(1280, 720, 10000.0f);
    CHECK(far.coarse == far.cell_med * 10.0f);
    CHECK(far.span_steps >= 1.0f);
    float rebuilt = far.span_steps * far.coarse + far.span_mod;
    CHECK(fabsf(rebuilt - far.span) <= far.coarse * 1.0e-4f);

    // Near the origin the reduced phase matches the real modulo on every
    // corner of the quad, including the negative ones.
    PlaneGrid near = plane_grid_for(1280, 720, 1.0f);
    const float near_cam = 12.5f;
    const float near_c[] = { -1.0f, -0.25f, 0.0f, 0.5f, 1.0f };
    float near_wrap = plane_grid_wrap(near_cam, near.coarse);
    for (uint32_t i = 0; i < sizeof near_c / sizeof near_c[0]; ++i) {
        float phase = plane_grid_phase(near_wrap, near_c[i], &near);
        CHECK(phase >= 0.0f && phase < near.coarse);
        double truth = fmod((double)near_cam + (double)near_c[i] * (double)near.span,
                            (double)near.coarse);
        if (truth < 0.0) {
            truth += (double)near.coarse;
        }
        double err = fabs((double)phase - truth);
        if (err > (double)near.coarse * 0.5) {
            err = (double)near.coarse - err;
        }
        CHECK(err < 1.0e-3);
    }

    // Far pan. Adding the camera to a corner in float32 is what jitters
    // the 0.001 lines. The center of the quad keeps the wrapped phase,
    // and a small corner stays within a fraction of the fine decade.
    float far_wrap = plane_grid_wrap(50000.0f, far.coarse);
    CHECK(far_wrap >= 0.0f && far_wrap < far.coarse);
    float center = plane_grid_phase(far_wrap, 0.0f, &far);
    CHECK(fabsf(center - far_wrap) < 1.0e-5f);
    float nudged = plane_grid_phase(far_wrap, 0.001f, &far);
    double expect = fmod((double)50000.0f + 0.001 * (double)far.span, (double)far.coarse);
    if (expect < 0.0) {
        expect += (double)far.coarse;
    }
    double far_err = fabs((double)nudged - expect);
    if (far_err > (double)far.coarse * 0.5) {
        far_err = (double)far.coarse - far_err;
    }
    CHECK(far_err < (double)far.cell_med * 0.001);
    float neg = plane_grid_wrap(-50000.0f, far.coarse);
    CHECK(neg >= 0.0f && neg < far.coarse);
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

// ----------------------------------------------------------- compute dispatch

static int s_records = 0;
static int s_oks = 0;
static int s_aborts = 0;
static int s_holds = 0;
static int s_commits = 0;

static void fake_record(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    (void)o;
    (void)cmd;
    (void)heat_dt;
    (void)live;
    s_records++;
}
static void fake_ok(WorldObj *o, uint64_t signal_point) {
    (void)o;
    if (signal_point == 9u) {
        s_oks++;
    }
}
static void fake_abort(WorldObj *o) {
    (void)o;
    s_aborts++;
}
static uint64_t fake_published(const WorldObj *o) {
    (void)o;
    return 4u;
}
static uint64_t fake_published_low(const WorldObj *o) {
    (void)o;
    return 2u;
}
static bool fake_step_open(const WorldObj *o) {
    (void)o;
    return true;
}
static bool fake_poke(const WorldObj *o) {
    (void)o;
    return true;
}
static bool fake_sim(WorldObj *o, uint32_t time_ms, uint32_t *heat_dt) {
    (void)o;
    (void)time_ms;
    if (heat_dt != nullptr) {
        *heat_dt = 7u;
    }
    return true;
}
static void fake_hold(WorldObj *o) {
    (void)o;
    s_holds++;
}
static void fake_commit(WorldObj *o, uint32_t stamp, uint32_t heat_dt) {
    (void)o;
    (void)stamp;
    if (heat_dt == 7u) {
        s_commits++;
    }
}

static const WorldOps k_ops_compute = {
    .record_compute      = fake_record,
    .record_ok           = fake_ok,
    .record_abort        = fake_abort,
    .record_published    = fake_published,
    .record_step_open    = fake_step_open,
    .record_poke_pending = fake_poke,
    .simulate            = fake_sim,
    .simulate_hold       = fake_hold,
    .simulate_commit     = fake_commit,
    .fallback            = true,
};
static const WorldOps k_ops_low = { .record_published = fake_published_low };

static void test_table_compute(void) {
    table_reset();
    uint32_t dt = 99u;
    CHECK(!table_simulate(1u, &dt));
    CHECK(dt == 0u);
    CHECK(table_record_published() == 0u);
    CHECK(!table_record_step_open());
    CHECK(!table_record_poke_pending());
    CHECK(table_fallback() == nullptr);

    CHECK(table_add(&k_ops_a, nullptr, (Coord){0, 0, 0}, nullptr) >= 0);
    CHECK(table_add(&k_ops_low, nullptr, (Coord){0, 0, 0}, nullptr) >= 0);
    CHECK(table_add(&k_ops_compute, nullptr, (Coord){0, 0, 0}, nullptr) >= 0);
    table_record(VK_NULL_HANDLE, 3u, true);
    CHECK(s_records == 1);
    // Newest signal wins. The lower point stays ordered behind it.
    CHECK(table_record_published() == 4u);
    CHECK(table_record_step_open());
    CHECK(table_record_poke_pending());
    table_record_ok(9u);
    CHECK(s_oks == 1);
    table_record_abort();
    CHECK(s_aborts == 1);
    dt = 0u;
    CHECK(table_simulate(10u, &dt));
    CHECK(dt == 7u);
    table_simulate_hold();
    CHECK(s_holds == 1);
    table_simulate_commit(10u, dt);
    CHECK(s_commits == 1);
    WorldObj *fb = table_fallback();
    CHECK(fb != nullptr && fb->ops == &k_ops_compute);

    table_at(2)->alive = false;
    table_record(VK_NULL_HANDLE, 3u, true);
    CHECK(s_records == 1);
    CHECK(table_record_published() == 2u);
    CHECK(table_fallback() == nullptr);
    table_reset();
}

static int s_kind_resets = 0;
static int s_kind_checks = 0;
static void kind_reset(void) { s_kind_resets++; }
static void kind_check(void) { s_kind_checks++; }

static void test_kind_listen(void) {
    table_kind_listen(kind_reset, kind_check);
    table_kinds_reset();
    table_kinds_check();
    CHECK(s_kind_resets == 1);
    CHECK(s_kind_checks == 1);
}

int main(void) {
    test_grid_math();
    test_table_stats();
    test_table_compute();
    test_kind_listen();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_world: all checks passed\n");
    return 0;
}
