#include "gizmo/gizmo.h"
#include <math.h>
#include <stdio.h>

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

static void test_gizmo_pick_translate_and_snap(void) {
    ecs_world_init(&s_world);
    EcsEntity parent = ecs_spawn(&s_world, "Parent", ECS_KIND_CUBE,  ECS_COMP_RENDER, 0);
    EcsEntity child  = ecs_spawn(&s_world, "Child",  ECS_KIND_SOLID, ECS_COMP_RENDER, 1);
    ecs_set_local_pos(&s_world, parent, 0.0f, 0.0f, 0.0f);
    ecs_set_local_pos(&s_world, child,  0.0f, 0.0f, 100.0f);
    CHECK(ecs_set_parent(&s_world, child, parent, false), "Attach child to parent");
    ecs_propagate_transforms(&s_world);

    GizmoState st;
    gizmo_state_init(&st);
    GizmoCamera cam;
    gizmo_camera_fill(&cam, (const float[4]){ 0.0f, 0.0f, 0.0f, 1.0f },
                      (const float[3]){ 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f);

    // Ray pointing directly along -Z at X=+50 (hits X translate handle!)
    GizmoRay r_start = { .origin = { 50.0f, 0.0f, 500.0f }, .dir = { 0.0f, 0.0f, -1.0f } };
    CHECK(gizmo_pick_handle(&st, &s_world, parent, &cam, &r_start) == GIZMO_HANDLE_X,
          "Ray at X=+50 picks X translate shaft");
    CHECK(gizmo_hover(&st, &s_world, parent, &cam, &r_start), "Hover change returns true");
    CHECK(!gizmo_hover(&st, &s_world, parent, &cam, &r_start), "Same hover returns false (0 idle wakeups)");

    // Begin drag and move +63 units with snap ON -> must snap cleanly to +75 units!
    CHECK(gizmo_begin_drag(&st, &s_world, parent, &cam, &r_start), "Begin X drag");
    GizmoRay r_drag = { .origin = { 113.0f, 0.0f, 500.0f }, .dir = { 0.0f, 0.0f, -1.0f } };
    CHECK(gizmo_update_drag(&st, &s_world, &cam, &r_drag, true), "Update X drag with Ctrl snap");
    gizmo_end_drag(&st);

    EcsTrs tp = {0}, tc = {0};
    CHECK(ecs_get_world_trs(&s_world, parent, &tp), "Parent world TRS");
    CHECK(ecs_get_world_trs(&s_world, child,  &tc), "Child world TRS");
    CHECK(near_f(tp.pos[0], 75.0f, 1e-3f), "Parent X snapped to 75.0");
    CHECK(near_f(tc.pos[0], 75.0f, 1e-3f) && near_f(tc.pos[2], 100.0f, 1e-3f),
          "Child inherited parent gizmo translation");
}

static void test_gizmo_rotate_scale_and_geometry(void) {
    ecs_world_reset(&s_world);
    EcsEntity e = ecs_spawn(&s_world, "Hero", ECS_KIND_SOLID, ECS_COMP_RENDER, 0);
    ecs_propagate_transforms(&s_world);

    GizmoState st;
    gizmo_state_init(&st);
    GizmoCamera cam;
    gizmo_camera_fill(&cam, (const float[4]){ 0.0f, 0.0f, 0.0f, 1.0f },
                      (const float[3]){ 0.0f, 0.0f, 0.0f }, 1.0f, 0.001f);

    // Rotate mode: ray hitting Z ring at (ring_r, 0, 0)
    gizmo_set_mode(&st, GIZMO_MODE_ROTATE);
    float sc = gizmo_compute_scale(&cam, (const float[3]){ 0.0f, 0.0f, 0.0f }, 96.0f);
    GizmoRay r0 = { .origin = { sc * 0.92f, 0.0f, 500.0f }, .dir = { 0.0f, 0.0f, -1.0f } };
    CHECK(gizmo_begin_drag(&st, &s_world, e, &cam, &r0), "Begin Z ring rotation");
    // Drag to +45 deg around Z with 15-deg snap
    GizmoRay r1 = { .origin = { sc * 0.65f, sc * 0.65f, 500.0f }, .dir = { 0.0f, 0.0f, -1.0f } };
    CHECK(gizmo_update_drag(&st, &s_world, &cam, &r1, true), "Rotate +45 deg snapped");
    gizmo_end_drag(&st);

    EcsTrs trs = {0};
    CHECK(ecs_get_world_trs(&s_world, e, &trs), "World TRS after rotate");
    float euler[3];
    ecs_quat_to_euler_deg(trs.rot, euler);
    CHECK(near_f(euler[2], 45.0f, 1.0f), "Z rotation snapped cleanly to 45 deg");

    // Scale mode always uses entity local basis (now rotated +45 deg around Z)
    gizmo_set_mode(&st, GIZMO_MODE_SCALE);
    GizmoRay rs0 = { .origin = { sc * 0.35355f, sc * 0.35355f, 500.0f }, .dir = { 0.0f, 0.0f, -1.0f } };
    CHECK(gizmo_begin_drag(&st, &s_world, e, &cam, &rs0), "Begin rotated local-X scale drag");
    GizmoRay rs1 = { .origin = { sc * 1.06066f, sc * 1.06066f, 500.0f }, .dir = { 0.0f, 0.0f, -1.0f } };
    CHECK(gizmo_update_drag(&st, &s_world, &cam, &rs1, true), "Scale local-X snapped to 2.0");
    // Verify vertex builder emits valid triangles for both gizmo and debug overlays
    st.debug_draw = true;
    static GizmoVertex verts[4096];
    uint32_t vn = gizmo_build_vertices(&st, &s_world, e, &cam, verts, 4096u);
    CHECK(vn > 100u && (vn % 3u) == 0u, "Gizmo + debug bounds build clean triangle list");
    gizmo_end_drag(&st);

    CHECK(ecs_get_world_trs(&s_world, e, &trs), "World TRS after scale");
    CHECK(near_f(trs.scale[0], 2.0f, 0.05f) && near_f(trs.scale[1], 1.0f, 0.05f),
          "Non-uniform local X scale exact");
}

int main(void) {
    test_gizmo_pick_translate_and_snap();
    test_gizmo_rotate_scale_and_geometry();

    if (s_failures != 0) {
        fprintf(stderr, "[test_gizmo] %d check(s) failed!\n", s_failures);
        return 1;
    }
    printf("test_gizmo: all checks passed\n");
    return 0;
}
