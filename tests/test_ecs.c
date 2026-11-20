#include "ecs/ecs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));     \
            s_failures++;                                                           \
        }                                                                           \
    } while (0)

static bool near_f(float a, float b, float eps) {
    return fabsf(a - b) <= eps;
}

static EcsWorld s_world;

static void test_generational_handles_and_soa_swap_remove(void) {
    ecs_world_init(&s_world);

    EcsEntity e0 = ecs_spawn(&s_world, "Root", ECS_KIND_CUBE, ECS_COMP_RENDER | ECS_COMP_PHYSICS, 0);
    EcsEntity e1 = ecs_spawn(&s_world, "Mid",  ECS_KIND_SOLID, ECS_COMP_RENDER, 1);
    EcsEntity e2 = ecs_spawn(&s_world, "Leaf", ECS_KIND_MESH,  ECS_COMP_RENDER, 2);

    CHECK(ecs_alive(&s_world, e0) && ecs_alive(&s_world, e1) && ecs_alive(&s_world, e2), "All spawned alive");
    CHECK(ecs_entity_count(&s_world) == 3u, "Dense count is 3");
    CHECK(strcmp(ecs_get_name(&s_world, e1), "Mid") == 0, "Name preserved");

    // Destroy middle entity -> triggers O(1) swap-remove of e2 into slot 1
    ecs_destroy(&s_world, e1);
    CHECK(!ecs_alive(&s_world, e1), "Destroyed handle rejected");
    CHECK(ecs_alive(&s_world, e2),  "Swapped tail entity still alive");
    CHECK(ecs_entity_count(&s_world) == 2u, "Dense count is 2");
    CHECK(ecs_get_user_slot(&s_world, e2) == 2, "Swapped entity user_slot intact");

    // Re-spawning reuses the freed sparse slot with a bumped generation
    EcsEntity e3 = ecs_spawn(&s_world, "Reused", ECS_KIND_SOLID, ECS_COMP_RENDER, 3);
    CHECK(e3 != e1, "Reused slot must have bumped generation");
    CHECK(!ecs_alive(&s_world, e1), "Old stale handle still rejected after reuse");
    CHECK(ecs_alive(&s_world, e3),  "New handle alive");

    // Bitmask query filters physics vs render
    EcsEntity phys_out[8];
    uint32_t np = ecs_query_mask(&s_world, ECS_COMP_PHYSICS, phys_out, 8u);
    CHECK(np == 1u && phys_out[0] == e0, "Bitmask query finds exact physics entity");
}

static void test_deep_hierarchy_and_dirty_caching(void) {
    ecs_world_reset(&s_world);

    EcsEntity r = ecs_spawn(&s_world, "Root", ECS_KIND_CUBE,  ECS_COMP_RENDER, 0);
    EcsEntity c = ecs_spawn(&s_world, "Mid",  ECS_KIND_SOLID, ECS_COMP_RENDER, 1);
    EcsEntity g = ecs_spawn(&s_world, "Tip",  ECS_KIND_SOLID, ECS_COMP_RENDER, 2);

    // Build 3-level chain: r -> c -> g
    CHECK(ecs_set_parent(&s_world, c, r, false), "Attach c to r");
    CHECK(ecs_set_parent(&s_world, g, c, false), "Attach g to c");
    // Cycle rejection: r cannot become child of g or itself
    CHECK(!ecs_set_parent(&s_world, r, g, false), "Reject ancestor-to-descendant cycle");
    CHECK(!ecs_set_parent(&s_world, r, r, false), "Reject self-parent cycle");

    // Root at (100, 0, 0) rotated +90 deg around Z, scaled 2x
    float qz90[4];
    ecs_quat_from_axis_angle((const float[3]){ 0.0f, 0.0f, 1.0f }, 1.57079632679f, qz90);
    ecs_set_local_trs(&s_world, r, (const float[3]){ 100.0f, 0.0f, 0.0f }, qz90,
                      (const float[3]){ 2.0f, 2.0f, 2.0f });
    // Mid at local (10, 0, 0) -> rotated +90Z & scaled 2x -> world (100, 20, 0)
    ecs_set_local_pos(&s_world, c, 10.0f, 0.0f, 0.0f);
    // Tip at local (0, 5, 0) -> rotated +90Z -> (-10, 0, 0) from Mid -> world (90, 20, 0)
    ecs_set_local_pos(&s_world, g, 0.0f, 5.0f, 0.0f);

    ecs_propagate_transforms(&s_world);
    CHECK(s_world.propagate_writes == 3u, "All 3 dirty transforms propagated");
    CHECK(ecs_get_depth(&s_world, r) == 0u && ecs_get_depth(&s_world, c) == 1u &&
          ecs_get_depth(&s_world, g) == 2u, "Depth buckets exact");

    EcsTrs trs_c = {0}, trs_g = {0};
    CHECK(ecs_get_world_trs(&s_world, c, &trs_c), "Get Mid world TRS");
    CHECK(ecs_get_world_trs(&s_world, g, &trs_g), "Get Tip world TRS");
    CHECK(near_f(trs_c.pos[0], 100.0f, 1e-3f) && near_f(trs_c.pos[1], 20.0f, 1e-3f) && near_f(trs_c.pos[2], 0.0f, 1e-3f),
          "Mid world pos exact");
    CHECK(near_f(trs_g.pos[0], 90.0f, 1e-3f) && near_f(trs_g.pos[1], 20.0f, 1e-3f) && near_f(trs_g.pos[2], 0.0f, 1e-3f),
          "Tip world pos exact");

    // Second propagation with nothing dirty must skip all work (0 writes)
    ecs_propagate_transforms(&s_world);
    CHECK(s_world.propagate_writes == 0u, "Clean hierarchy skips 100% of matrix math");

    // World-pose-preserving detach: detaching Tip from Mid must NOT change Tip's world position
    CHECK(ecs_set_parent(&s_world, g, ECS_NULL_ENTITY, true), "Detach Tip preserving world pose");
    ecs_propagate_transforms(&s_world);
    EcsTrs trs_g_after = {0};
    CHECK(ecs_get_world_trs(&s_world, g, &trs_g_after), "Get Tip world TRS after detach");
    CHECK(near_f(trs_g_after.pos[0], 90.0f, 1e-2f) && near_f(trs_g_after.pos[1], 20.0f, 1e-2f) &&
          near_f(trs_g_after.pos[2], 0.0f, 1e-2f), "Keep-world-pose detach prevents teleport");
    CHECK(near_f(trs_g_after.scale[0], 2.0f, 1e-2f), "Keep-world-pose preserves inherited scale");
}

static void test_nonuniform_cofactor_normals_and_world_setter(void) {
    ecs_world_reset(&s_world);

    EcsEntity p = ecs_spawn(&s_world, "Parent", ECS_KIND_CUBE,  ECS_COMP_RENDER, 0);
    EcsEntity c = ecs_spawn(&s_world, "Child",  ECS_KIND_SOLID, ECS_COMP_RENDER, 1);
    ecs_set_local_trs(&s_world, p, (const float[3]){ 50.0f, -20.0f, 30.0f },
                      (const float[4]){ 0.0f, 0.0f, 0.0f, 1.0f },
                      (const float[3]){ 2.0f, 0.5f, 1.0f });
    ecs_set_local_bounds(&s_world, p, (const float[3]){ 10.0f, 10.0f, 10.0f }, 17.32f);
    CHECK(ecs_set_parent(&s_world, c, p, false), "Parent child");
    ecs_propagate_transforms(&s_world);

    // Check cofactor normal matrix preserves perpendicularity to transformed tangent
    // Tangent along (1, 1, 0) scaled by (2, 0.5, 1) -> world tangent (2, 0.5, 0)
    // Original normal (-1, 1, 0) -> transformed by cofactor must be orthogonal to (2, 0.5, 0)!
    uint32_t pd = ecs_dense_index(&s_world, p);
    const float *nm = s_world.world_nrm[pd];
    float wn[3] = {
        nm[0] * (-1.0f) + nm[1] * 1.0f,
        nm[3] * (-1.0f) + nm[4] * 1.0f,
        nm[6] * (-1.0f) + nm[7] * 1.0f,
    };
    float dot_tn = wn[0] * 2.0f + wn[1] * 0.5f;
    CHECK(near_f(dot_tn, 0.0f, 1e-4f), "Cofactor normal matrix stays orthogonal under non-uniform scale");

    // Arvo AABB check: half-ext (10,10,10) scaled by (2, 0.5, 1) -> (20, 5, 10)
    EcsAabb aabb = {0};
    CHECK(ecs_get_world_aabb(&s_world, p, &aabb), "Get AABB");
    CHECK(near_f(aabb.max_v[0] - aabb.min_v[0], 40.0f, 1e-3f) &&
          near_f(aabb.max_v[1] - aabb.min_v[1], 10.0f, 1e-3f) &&
          near_f(aabb.max_v[2] - aabb.min_v[2], 20.0f, 1e-3f), "Arvo AABB extents exact");

    // World-space setter on nested child converts cleanly through inv(M_parent)
    ecs_set_world_pos(&s_world, c, 70.0f, -15.0f, 40.0f);
    ecs_propagate_transforms(&s_world);
    EcsTrs c_world = {0};
    CHECK(ecs_get_world_trs(&s_world, c, &c_world), "Child world TRS");
    CHECK(near_f(c_world.pos[0], 70.0f, 1e-3f) && near_f(c_world.pos[1], -15.0f, 1e-3f) &&
          near_f(c_world.pos[2], 40.0f, 1e-3f), "World pos setter accurate on non-uniformly scaled child");
}

int main(void) {
    test_generational_handles_and_soa_swap_remove();
    test_deep_hierarchy_and_dirty_caching();
    test_nonuniform_cofactor_normals_and_world_setter();

    if (s_failures != 0) {
        fprintf(stderr, "[test_ecs] %d check(s) failed!\n", s_failures);
        return 1;
    }
    printf("test_ecs: all checks passed\n");
    return 0;
}
