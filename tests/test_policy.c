#include "frame/policy.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

// Capability gate: permissions, shift-exact binds, seal, and the live
// switch between the flat plot workspace and the 3D workspace.
// Links policy.c only. No Vulkan, no pointer, no window.

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

static uint16_t s_down[8];
static uint32_t s_down_n = 0;

static void hold(uint16_t key) {
    if (s_down_n < 8u) {
        s_down[s_down_n++] = key;
    }
}

static void release_all(void) {
    s_down_n = 0;
}

static bool fake_down(uint16_t key, void *user) {
    (void)user;
    for (uint32_t i = 0; i < s_down_n; ++i) {
        if (s_down[i] == key) {
            return true;
        }
    }
    return false;
}

static void check_profiles(void) {
    const WorkspacePolicy *model = policy_blender();
    const WorkspacePolicy *plot = policy_matplotlib();

    CHECK(strcmp(model->name, "Blender-3D") == 0);
    CHECK(!model->sealed);
    CHECK(!model->lock_2d_quat);
    CHECK(model->bind_count == 33u);

    CHECK(strcmp(plot->name, "Matplotlib-2D") == 0);
    CHECK(!plot->sealed);
    CHECK(plot->lock_2d_quat);
    CHECK(plot->bind_count == 14u);

    CHECK(policy_can(model, ACT_YAW_CW));
    CHECK(policy_can(model, ACT_PITCH_UP));
    CHECK(policy_can(model, ACT_SPAWN_SOLID_4));
    CHECK(policy_can(model, ACT_DELETE));
    CHECK(policy_can(model, ACT_PLOT_CYCLE));
    CHECK(policy_can(model, ACT_DATA_RELOAD));
    CHECK((model->active_perms & PERM_OBJ_COMPUTE) == PERM_OBJ_COMPUTE);
    CHECK((model->active_perms & PERM_OBJ_SELECT) == PERM_OBJ_SELECT);

    CHECK(policy_can(plot, ACT_PAN_LEFT));
    CHECK(policy_can(plot, ACT_ZOOM_IN));
    CHECK(policy_can(plot, ACT_SNAP_FRONT));
    CHECK(policy_can(plot, ACT_PLOT_CYCLE));
    CHECK(policy_can(plot, ACT_DATA_RELOAD));
    CHECK(policy_can(plot, ACT_SWITCH_MODE));
    CHECK(!policy_can(plot, ACT_YAW_CCW));
    CHECK(!policy_can(plot, ACT_YAW_CW));
    CHECK(!policy_can(plot, ACT_PITCH_UP));
    CHECK(!policy_can(plot, ACT_PITCH_DOWN));
    CHECK(!policy_can(plot, ACT_SPAWN_SOLID_1));
    CHECK(!policy_can(plot, ACT_DELETE));
    CHECK(!policy_can(plot, ACT_CLOSE_FACE));
    CHECK(!policy_can(plot, ACT_TOGGLE_SPIN));
    CHECK((plot->active_perms & PERM_OBJ_SELECT) == 0u);
    CHECK((plot->active_perms & PERM_OBJ_COMPUTE) == 0u);

    CHECK(policy_can(nullptr, ACT_PAN_LEFT) == false);
    CHECK(policy_can(model, ACT_NONE) == false);
    CHECK(policy_can(model, ACT_COUNT) == false);

    CHECK(policy_lookup(model, KEY_Q, false) == ACT_YAW_CCW);
    CHECK(policy_lookup(model, KEY_Q, true) == ACT_YAW_CCW);
    CHECK(policy_lookup(model, KEY_E, false) == ACT_YAW_CW);
    CHECK(policy_lookup(model, KEY_C, false) == ACT_PITCH_UP);
    CHECK(policy_lookup(model, KEY_V, false) == ACT_PITCH_DOWN);
    CHECK(policy_lookup(plot, KEY_Q, false) == ACT_NONE);
    CHECK(policy_lookup(plot, KEY_E, true) == ACT_NONE);
    CHECK(policy_lookup(plot, KEY_X, false) == ACT_NONE);
    CHECK(policy_lookup(plot, KEY_1, false) == ACT_NONE);

    CHECK(policy_lookup(model, KEY_4, false) == ACT_SPAWN_SOLID_4);
    CHECK(policy_lookup(model, KEY_4, true) == ACT_ZOOM_IN);
    CHECK(policy_lookup(plot, KEY_4, false) == ACT_NONE);
    CHECK(policy_lookup(plot, KEY_4, true) == ACT_ZOOM_IN);
    CHECK(policy_lookup(model, KEY_EQUAL, true) == ACT_ZOOM_IN);
    CHECK(policy_lookup(model, KEY_X, false) == ACT_DELETE);
    CHECK(policy_lookup(model, KEY_BACKSPACE, false) == ACT_DELETE);
    CHECK(policy_lookup(model, KEY_DELETE, false) == ACT_DELETE);
    CHECK(policy_lookup(model, KEY_TAB, false) == ACT_SWITCH_MODE);
    CHECK(policy_lookup(plot, KEY_TAB, false) == ACT_SWITCH_MODE);
    CHECK(policy_lookup(nullptr, KEY_Q, false) == ACT_NONE);

    CHECK(policy_route(model, KEY_Q, false) == KEY_ROUTE_HOLD);
    CHECK(policy_route(model, KEY_EQUAL, false) == KEY_ROUTE_HOLD);
    CHECK(policy_route(model, KEY_4, true) == KEY_ROUTE_HOLD);
    CHECK(policy_route(model, KEY_4, false) == KEY_ROUTE_TAP);
    CHECK(policy_route(model, KEY_ESC, false) == KEY_ROUTE_TAP);
    CHECK(policy_route(model, KEY_X, false) == KEY_ROUTE_TAP);
    CHECK(policy_route(plot, KEY_Q, false) == KEY_ROUTE_IGNORE);
    CHECK(policy_route(plot, KEY_C, true) == KEY_ROUTE_IGNORE);
    CHECK(policy_route(plot, KEY_4, false) == KEY_ROUTE_IGNORE);
    CHECK(policy_route(plot, KEY_4, true) == KEY_ROUTE_HOLD);
    CHECK(policy_route(plot, KEY_M, false) == KEY_ROUTE_TAP);
    CHECK(policy_route(plot, KEY_1, false) == KEY_ROUTE_IGNORE);
}

static void check_mutation(void) {
    WorkspacePolicy sealed = *policy_blender();
    sealed.sealed = true;
    uint32_t before = sealed.active_perms;
    CHECK(!policy_grant(&sealed, PERM_CAM_YAW));
    CHECK(!policy_revoke(&sealed, PERM_CAM_YAW));
    CHECK(!policy_rebind_key(&sealed, KEY_Q, KEY_SHIFT_ANY, ACT_DELETE));
    CHECK(sealed.active_perms == before);
    CHECK(policy_lookup(&sealed, KEY_Q, false) == ACT_YAW_CCW);

    WorkspacePolicy open = *policy_matplotlib();
    CHECK(policy_grant(&open, PERM_CAM_YAW));
    CHECK(policy_can(&open, ACT_YAW_CW));
    CHECK(policy_lookup(&open, KEY_Q, false) == ACT_NONE);
    CHECK(policy_revoke(&open, PERM_CAM_YAW));
    CHECK(!policy_can(&open, ACT_YAW_CW));

    WorkspacePolicy rebound = *policy_blender();
    CHECK(policy_rebind_key(&rebound, KEY_Q, KEY_SHIFT_ANY, ACT_DELETE));
    CHECK(policy_lookup(&rebound, KEY_Q, false) == ACT_DELETE);
    CHECK(policy_lookup(&rebound, KEY_Q, true) == ACT_DELETE);
    CHECK(policy_route(&rebound, KEY_Q, false) == KEY_ROUTE_TAP);

    rebound.bind_count = k_max_key_binds;
    CHECK(!policy_rebind_key(&rebound, KEY_Z, KEY_SHIFT_ANY, ACT_DELETE));
    CHECK(policy_rebind_key(&rebound, KEY_E, KEY_SHIFT_ANY, ACT_CANCEL));
    CHECK(policy_lookup(&rebound, KEY_E, false) == ACT_CANCEL);

    CHECK(!policy_grant(nullptr, PERM_CAM_PAN_X));
    CHECK(!policy_revoke(nullptr, PERM_CAM_PAN_X));
    CHECK(!policy_rebind_key(nullptr, KEY_Q, KEY_SHIFT_ANY, ACT_DELETE));
}

static void check_axis(void) {
    WorkspacePolicy model = *policy_blender();
    release_all();
    hold(KEY_E);
    hold(KEY_Q);
    CHECK(policy_held_axis(&model, ACT_YAW_CW, ACT_YAW_CCW, false, fake_down, nullptr) == 0);
    release_all();
    hold(KEY_E);
    CHECK(policy_held_axis(&model, ACT_YAW_CW, ACT_YAW_CCW, false, fake_down, nullptr) == 1);
    release_all();
    hold(KEY_Q);
    CHECK(policy_held_axis(&model, ACT_YAW_CW, ACT_YAW_CCW, false, fake_down, nullptr) == -1);
    release_all();
    hold(KEY_EQUAL);
    hold(KEY_MINUS);
    CHECK(policy_held_axis(&model, ACT_ZOOM_IN, ACT_ZOOM_OUT, false, fake_down, nullptr) == 0);
    release_all();
    hold(KEY_4);
    CHECK(policy_held_axis(&model, ACT_ZOOM_IN, ACT_ZOOM_OUT, false, fake_down, nullptr) == 0);
    CHECK(policy_held_axis(&model, ACT_ZOOM_IN, ACT_ZOOM_OUT, true, fake_down, nullptr) == 1);
    CHECK(policy_held_axis(nullptr, ACT_YAW_CW, ACT_YAW_CCW, false, fake_down, nullptr) == 0);
    CHECK(policy_held_axis(&model, ACT_YAW_CW, ACT_YAW_CCW, false, nullptr, nullptr) == 0);

    CHECK(policy_revoke(&model, PERM_CAM_YAW));
    release_all();
    hold(KEY_E);
    CHECK(policy_held_axis(&model, ACT_YAW_CW, ACT_YAW_CCW, false, fake_down, nullptr) == 0);
    CHECK(policy_lookup(&model, KEY_E, false) == ACT_YAW_CW);
    CHECK(policy_route(&model, KEY_E, false) == KEY_ROUTE_IGNORE);
}

static void check_live(void) {
    policy_boot();
    policy_boot();
    CHECK(strcmp(policy_active()->name, "Blender-3D") == 0);
    CHECK(policy_allows(PERM_OBJ_SELECT));
    CHECK(policy_allows(PERM_OBJ_SPAWN));
    CHECK(!policy_allows(PERM_NONE));

    CHECK(policy_rebind_key(policy_active(), KEY_Q, KEY_SHIFT_ANY, ACT_DELETE));
    CHECK(policy_lookup(policy_active(), KEY_Q, false) == ACT_DELETE);
    CHECK(policy_switch());
    CHECK(strcmp(policy_active()->name, "Matplotlib-2D") == 0);
    CHECK(policy_active()->lock_2d_quat);
    CHECK(!policy_allows(PERM_OBJ_SELECT));
    CHECK(!policy_allows(PERM_CAM_YAW));
    CHECK(policy_allows(PERM_CAM_ZOOM));
    CHECK(policy_allows(PERM_MODE_SWITCH));
    CHECK(policy_lookup(policy_active(), KEY_Q, false) == ACT_NONE);

    CHECK(policy_switch());
    CHECK(strcmp(policy_active()->name, "Blender-3D") == 0);
    CHECK(!policy_active()->lock_2d_quat);
    CHECK(policy_lookup(policy_active(), KEY_Q, false) == ACT_YAW_CCW);
    CHECK(policy_allows(PERM_OBJ_SELECT));

    policy_active()->sealed = true;
    CHECK(!policy_switch());
    CHECK(!policy_grant(policy_active(), PERM_CAM_YAW));
    CHECK(strcmp(policy_active()->name, "Blender-3D") == 0);
    policy_active()->sealed = false;
    CHECK(policy_switch());
    CHECK(strcmp(policy_active()->name, "Matplotlib-2D") == 0);
    CHECK(policy_switch());
    CHECK(strcmp(policy_active()->name, "Blender-3D") == 0);
}

int main(void) {
    check_profiles();
    check_mutation();
    check_axis();
    check_live();

    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_policy: all checks passed\n");
    return 0;
}
