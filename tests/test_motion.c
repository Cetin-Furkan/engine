#include "frame/fillet.h"
#include "world/motion.h"
#include <math.h>
#include <stdio.h>

static int s_failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            s_failures++;                                                           \
        }                                                                           \
    } while (0)

static void settle(MotionBody *bodies, uint32_t count, int steps) {
    for (int i = 0; i < steps; ++i) {
        motion_substep(bodies, count, 0.008f, 0.0f, k_fillet_half);
    }
}

static void test_lift_clears_the_pitch(void) {
    float expect = k_fillet_half * (sinf(k_fillet_pitch) + cosf(k_fillet_pitch));
    float lift = motion_rest_lift(0.0f, k_fillet_half);
    CHECK(fabsf(lift - expect) < 0.05f);
    CHECK(lift > k_fillet_half);
}

static void test_pose_matches_the_draw(void) {
    float x[3];
    float y[3];
    float z[3];
    fillet_axes(0.0f, x, y, z);
    CHECK(fabsf(x[0] - 1.0f) < 1.0e-5f);
    CHECK(fabsf(x[1]) < 1.0e-5f && fabsf(x[2]) < 1.0e-5f);
    CHECK(fabsf(y[1] - cosf(k_fillet_pitch)) < 1.0e-5f);
    CHECK(fabsf(z[2] - cosf(k_fillet_pitch)) < 1.0e-5f);
    float support = k_fillet_half * (fabsf(x[2]) + fabsf(y[2]) + fabsf(z[2]));
    CHECK(fabsf(motion_rest_lift(0.0f, k_fillet_half) - support) < 1.0e-4f);

    float yaw = fillet_rad(38.0f);
    fillet_axes(yaw, x, y, z);
    float local[3] = { 1.0f, 0.0f, 0.0f };
    float world[3];
    fillet_place(local, yaw, k_fillet_half, 10.0f, 20.0f, 30.0f, world);
    CHECK(fabsf(world[0] - (10.0f + x[0] * k_fillet_half)) < 1.0e-4f);
    CHECK(fabsf(world[1] - (20.0f + x[1] * k_fillet_half)) < 1.0e-4f);
    CHECK(fabsf(world[2] - (30.0f + x[2] * k_fillet_half)) < 1.0e-4f);
}

static void test_still_cube_stays(void) {
    MotionBody b = { .x = 100.0f, .y = 0.0f, .z = 0.0f, .yaw = 0.3f };
    settle(&b, 1, 200);
    CHECK(!b.awake);
    CHECK(b.x == 100.0f && b.y == 0.0f && b.z == 0.0f);
    CHECK(b.vx == 0.0f && b.vy == 0.0f && b.vz == 0.0f);
}

static void test_shove_rests_on_the_ground(void) {
    float lift = motion_rest_lift(0.0f, k_fillet_half);
    MotionBody b = { .vx = 640.0f, .awake = true };
    settle(&b, 1, 800);
    CHECK(!b.awake);
    CHECK(fabsf(b.z - lift) < 0.6f);
    CHECK(fabsf(b.vx) < 0.1f && fabsf(b.vy) < 0.1f && fabsf(b.vz) < 0.1f);
    CHECK(b.x > 80.0f);
}

static void test_cubes_separate(void) {
    float lift = motion_rest_lift(0.0f, k_fillet_half);
    MotionBody pair[3] = {
        { .x = 0.0f, .z = lift, .vx = 500.0f, .awake = true },
        { .x = 200.0f, .z = lift },
        { .x = 4000.0f, .z = 0.0f },
    };
    settle(pair, 3, 800);
    CHECK(!pair[0].awake && !pair[1].awake && !pair[2].awake);
    CHECK(fabsf(pair[1].x - pair[0].x) >= 239.0f);
    CHECK(pair[1].x > 200.0f);
    CHECK(pair[2].x == 4000.0f && pair[2].z == 0.0f);
}

// At yaw 38° the pitched faces meet at 240/cos(38°) ≈ 305 px along world X.
// The loose bounds meet near 389 px, so a 340 px gap must stay open and a
// 260 px overlap must finish on the faces.
static void test_pitched_faces_meet(void) {
    float yaw = fillet_rad(38.0f);
    float lift = motion_rest_lift(yaw, k_fillet_half);
    MotionBody gap[2] = {
        { .z = lift, .yaw = yaw, .awake = true },
        { .x = 340.0f, .z = lift, .yaw = yaw, .awake = true },
    };
    settle(gap, 2, 200);
    CHECK(!gap[0].awake && !gap[1].awake);
    CHECK(fabsf(gap[0].x) < 0.05f);
    CHECK(fabsf(gap[1].x - 340.0f) < 0.05f);
    CHECK(fabsf(gap[0].y) < 0.05f && fabsf(gap[1].y) < 0.05f);
    CHECK(fabsf(gap[0].z - lift) < 0.05f);
    CHECK(fabsf(gap[1].z - lift) < 0.05f);

    MotionBody hit[2] = {
        { .z = lift, .yaw = yaw, .awake = true },
        { .x = 260.0f, .z = lift, .yaw = yaw, .awake = true },
    };
    settle(hit, 2, 800);
    float dx = fabsf(hit[1].x - hit[0].x);
    CHECK(!hit[0].awake && !hit[1].awake);
    CHECK(dx > 300.0f);
    CHECK(dx < 310.0f);
    CHECK(fabsf(hit[0].y) < 0.05f && fabsf(hit[1].y) < 0.05f);
    CHECK(fabsf(hit[0].z - lift) < 0.6f);
    CHECK(fabsf(hit[1].z - lift) < 0.6f);
    CHECK(fabsf(hit[0].vx) < 0.1f && fabsf(hit[1].vx) < 0.1f);

    MotionBody shove[2] = {
        { .z = lift, .yaw = yaw, .vx = 640.0f, .awake = true },
        { .x = 500.0f, .z = lift, .yaw = yaw },
    };
    float closest = 1.0e9f;
    for (int i = 0; i < 1500; ++i) {
        motion_substep(shove, 2, 0.008f, 0.0f, k_fillet_half);
        float gap_x = fabsf(shove[1].x - shove[0].x);
        if (gap_x < closest) {
            closest = gap_x;
        }
    }
    CHECK(closest > 300.0f);
    CHECK(closest < 310.0f);
    CHECK(fabsf(shove[1].x - shove[0].x) > 300.0f);
    CHECK(fabsf(shove[1].x - shove[0].x) < 310.0f);
    CHECK(!shove[0].awake && !shove[1].awake);
}

static void test_elastic_bounce_and_solids(void) {
    // Compare inelastic vs elastic non-cube solid dropped from z = 500
    MotionBody dull = {
        .z = 500.0f,
        .restitution = 0.10f,
        .radius = 120.0f,
        .spherical = true,
        .awake = true,
    };
    MotionBody bouncy = {
        .z = 500.0f,
        .restitution = 0.88f,
        .radius = 120.0f,
        .spherical = true,
        .awake = true,
    };
    float dull_rebound_vz = 0.0f;
    float bouncy_rebound_vz = 0.0f;
    float bouncy_max_impact = 0.0f;
    for (int i = 0; i < 120; ++i) {
        motion_substep(&dull, 1, 0.008f, 0.0f, k_fillet_half);
        motion_substep(&bouncy, 1, 0.008f, 0.0f, k_fillet_half);
        if (dull.vz > dull_rebound_vz) {
            dull_rebound_vz = dull.vz;
        }
        if (bouncy.vz > bouncy_rebound_vz) {
            bouncy_rebound_vz = bouncy.vz;
        }
        if (bouncy.impact > bouncy_max_impact) {
            bouncy_max_impact = bouncy.impact;
        }
    }
    CHECK(bouncy_rebound_vz > 450.0f);
    CHECK(bouncy_rebound_vz > dull_rebound_vz * 3.0f);
    CHECK(bouncy_max_impact > 100.0f);

    // Spherical solid colliding with a cube transfers momentum and separates
    float lift = motion_rest_lift(0.0f, k_fillet_half);
    MotionBody mix[2] = {
        { .x = 0.0f, .z = 120.0f, .vx = 700.0f, .restitution = 0.85f, .radius = 120.0f, .spherical = true, .awake = true },
        { .x = 220.0f, .z = lift, .restitution = 0.85f },
    };
    for (int i = 0; i < 40; ++i) {
        motion_substep(mix, 2, 0.008f, 0.0f, k_fillet_half);
    }
    CHECK(mix[1].x > 250.0f);
}

static void test_single_owner_gpu_pose_contract(void) {
    // Verify that when GPU AVBD owns the pose:
    // 1. motion_prepare_avbd_step leaves MotionBody positions 100% untouched on CPU;
    // 2. harvesting a GPU-solved AvbdVertex buffer produces the GPU pose and FAILS
    //    if the resulting pose still matches the CPU integrator.
    float lift = motion_rest_lift(0.0f, k_fillet_half);
    MotionBody cpu_body = {
        .x           = 10.0f,
        .y           = -20.0f,
        .z           = lift + 120.0f,
        .vx          = 500.0f,
        .vy          = -150.0f,
        .vz          = 0.0f,
        .restitution = 0.75f,
        .awake       = true,
    };
    MotionBody gpu_body = cpu_body;

    // Reference CPU-only integrator step
    motion_reset_warm_cache();
    motion_substep(&cpu_body, 1u, 0.008f, 0.0f, k_fillet_half);

    // GPU-owned path: prepare step on CPU without integrating positions
    motion_reset_warm_cache();
    AvbdVertex     gpu_verts[K_AVBD_MAX_VERTS];
    AvbdConstraint gpu_cons[K_AVBD_MAX_CONS];
    uint32_t       vn = 0u;
    uint32_t       cn = 0u;
    CHECK(motion_prepare_avbd_step(&gpu_body, 1u, 0.008f, 0.0f, k_fillet_half,
                                   gpu_verts, &vn, gpu_cons, &cn));
    CHECK(vn == 1u);
    CHECK(cn == 1u);

    // Pre-harvest drawn pose MUST NOT have moved on the CPU
    CHECK(gpu_body.x == 10.0f);
    CHECK(gpu_body.y == -20.0f);
    CHECK(gpu_body.z == lift + 120.0f);
    CHECK(fabsf(gpu_body.x - cpu_body.x) > 1.0f);

    // Simulate a distinct GPU compute solve in the mapped BDA vertex buffer
    gpu_verts[0].pos[0] = 77.5f;
    gpu_verts[0].pos[1] = -88.25f;
    gpu_verts[0].pos[2] = lift + 64.0f;

    // Harvest the retired GPU vertex buffer
    motion_apply_solved_avbd(&gpu_body, 1u, 0.008f, 0.0f, k_fillet_half,
                             gpu_verts, vn, gpu_cons, cn);

    // Drawn pose MUST match the GPU buffer and MUST NOT match the CPU integrator
    CHECK(fabsf(gpu_body.x - 77.5f) < 1.0e-4f);
    CHECK(fabsf(gpu_body.y - (-88.25f)) < 1.0e-4f);
    CHECK(fabsf(gpu_body.z - (lift + 64.0f)) < 1.0e-4f);
    CHECK(fabsf(gpu_body.x - cpu_body.x) > 50.0f);
    CHECK(fabsf(gpu_body.y - cpu_body.y) > 50.0f);
    CHECK(fabsf(gpu_body.z - cpu_body.z) > 40.0f);
}

int main(void) {
    test_lift_clears_the_pitch();
    test_pose_matches_the_draw();
    test_still_cube_stays();
    test_shove_rests_on_the_ground();
    test_cubes_separate();
    test_pitched_faces_meet();
    test_elastic_bounce_and_solids();
    test_single_owner_gpu_pose_contract();
    if (s_failures != 0) {
        fprintf(stderr, "test_motion: %d failed\n", s_failures);
        return 1;
    }
    printf("test_motion: all checks passed\n");
    return 0;
}
