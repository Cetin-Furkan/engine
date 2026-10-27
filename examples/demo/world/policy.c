#include "frame/policy.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

// Two compiled workspaces. The live copy starts as the 3D keymap so
// Q/E yaw and C/V pitch behave as they did before the gate existed.
// sealed true on a profile locks that copy for the life of the process:
// grant, revoke, rebind, and Tab all fail. Both ship unsealed so Tab
// can move between them.
//
// Shift+4 is plus on a Turkish Q number row. That chord is zoom.
// 4 with shift up places the cylinder. The two binds share KEY_4 and
// the exact shift match picks one.

static const bool k_blender_sealed = false;
static const bool k_plot_sealed    = false;

static const uint32_t k_perm_plot =
    PERM_CAM_PAN_X | PERM_CAM_PAN_Y | PERM_CAM_ZOOM | PERM_CAM_SNAP |
    PERM_PLOT_CYCLE | PERM_DATA_RELOAD | PERM_MODE_SWITCH;

static const uint32_t k_perm_blender =
    PERM_CAM_PAN_X | PERM_CAM_PAN_Y | PERM_CAM_ZOOM | PERM_CAM_YAW |
    PERM_CAM_PITCH | PERM_CAM_SNAP | PERM_OBJ_SELECT | PERM_OBJ_SPAWN |
    PERM_OBJ_DELETE | PERM_OBJ_SPIN | PERM_OBJ_MATERIAL | PERM_OBJ_COMPUTE |
    PERM_PLOT_CYCLE | PERM_DATA_RELOAD | PERM_MODE_SWITCH;

static const uint32_t k_action_perm[ACT_COUNT] = {
    [ACT_NONE]          = PERM_NONE,
    [ACT_PAN_LEFT]      = PERM_CAM_PAN_X,
    [ACT_PAN_RIGHT]     = PERM_CAM_PAN_X,
    [ACT_PAN_UP]        = PERM_CAM_PAN_Y,
    [ACT_PAN_DOWN]      = PERM_CAM_PAN_Y,
    [ACT_ZOOM_IN]       = PERM_CAM_ZOOM,
    [ACT_ZOOM_OUT]      = PERM_CAM_ZOOM,
    [ACT_YAW_CCW]       = PERM_CAM_YAW,
    [ACT_YAW_CW]        = PERM_CAM_YAW,
    [ACT_PITCH_UP]      = PERM_CAM_PITCH,
    [ACT_PITCH_DOWN]    = PERM_CAM_PITCH,
    [ACT_SNAP_FRONT]    = PERM_CAM_SNAP,
    [ACT_CANCEL]        = PERM_OBJ_SELECT,
    [ACT_DELETE]        = PERM_OBJ_DELETE,
    [ACT_CLOSE_FACE]    = PERM_OBJ_SPAWN,
    [ACT_RESPIN]        = PERM_OBJ_SPIN,
    [ACT_TOGGLE_SPIN]   = PERM_OBJ_SPIN,
    [ACT_TOGGLE_SMOOTH] = PERM_OBJ_MATERIAL,
    [ACT_TOGGLE_GLOW]   = PERM_OBJ_MATERIAL,
    [ACT_PLOT_CYCLE]    = PERM_PLOT_CYCLE,
    [ACT_DATA_RELOAD]   = PERM_DATA_RELOAD,
    [ACT_SPAWN_SOLID_1] = PERM_OBJ_SPAWN,
    [ACT_SPAWN_SOLID_2] = PERM_OBJ_SPAWN,
    [ACT_SPAWN_SOLID_3] = PERM_OBJ_SPAWN,
    [ACT_SPAWN_SOLID_4] = PERM_OBJ_SPAWN,
    [ACT_SPAWN_SOLID_5] = PERM_OBJ_SPAWN,
    [ACT_SPAWN_SOLID_6] = PERM_OBJ_SPAWN,
    [ACT_SWITCH_MODE]   = PERM_MODE_SWITCH,
};

static const KeyBind k_blender_binds[] = {
    { .evdev_key = KEY_LEFT,      .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_LEFT },
    { .evdev_key = KEY_RIGHT,     .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_RIGHT },
    { .evdev_key = KEY_UP,        .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_UP },
    { .evdev_key = KEY_DOWN,      .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_DOWN },
    { .evdev_key = KEY_Q,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_YAW_CCW },
    { .evdev_key = KEY_E,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_YAW_CW },
    { .evdev_key = KEY_C,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_PITCH_UP },
    { .evdev_key = KEY_V,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_PITCH_DOWN },
    { .evdev_key = KEY_EQUAL,     .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_IN },
    { .evdev_key = KEY_KPPLUS,    .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_IN },
    { .evdev_key = KEY_4,         .need_shift = KEY_SHIFT_DOWN, .action = ACT_ZOOM_IN },
    { .evdev_key = KEY_MINUS,     .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_OUT },
    { .evdev_key = KEY_KPMINUS,   .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_OUT },
    { .evdev_key = KEY_HOME,      .need_shift = KEY_SHIFT_ANY,  .action = ACT_SNAP_FRONT },
    { .evdev_key = KEY_KP1,       .need_shift = KEY_SHIFT_ANY,  .action = ACT_SNAP_FRONT },
    { .evdev_key = KEY_ESC,       .need_shift = KEY_SHIFT_ANY,  .action = ACT_CANCEL },
    { .evdev_key = KEY_BACKSPACE, .need_shift = KEY_SHIFT_ANY,  .action = ACT_DELETE },
    { .evdev_key = KEY_DELETE,    .need_shift = KEY_SHIFT_ANY,  .action = ACT_DELETE },
    { .evdev_key = KEY_X,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_DELETE },
    { .evdev_key = KEY_F,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_CLOSE_FACE },
    { .evdev_key = KEY_F5,        .need_shift = KEY_SHIFT_ANY,  .action = ACT_RESPIN },
    { .evdev_key = KEY_R,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_TOGGLE_SPIN },
    { .evdev_key = KEY_S,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_TOGGLE_SMOOTH },
    { .evdev_key = KEY_G,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_TOGGLE_GLOW },
    { .evdev_key = KEY_M,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_PLOT_CYCLE },
    { .evdev_key = KEY_L,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_DATA_RELOAD },
    { .evdev_key = KEY_TAB,       .need_shift = KEY_SHIFT_ANY,  .action = ACT_SWITCH_MODE },
    { .evdev_key = KEY_1,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_SPAWN_SOLID_1 },
    { .evdev_key = KEY_2,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_SPAWN_SOLID_2 },
    { .evdev_key = KEY_3,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_SPAWN_SOLID_3 },
    { .evdev_key = KEY_4,         .need_shift = KEY_SHIFT_UP,   .action = ACT_SPAWN_SOLID_4 },
    { .evdev_key = KEY_5,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_SPAWN_SOLID_5 },
    { .evdev_key = KEY_6,         .need_shift = KEY_SHIFT_ANY,  .action = ACT_SPAWN_SOLID_6 },
};

static const KeyBind k_plot_binds[] = {
    { .evdev_key = KEY_LEFT,    .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_LEFT },
    { .evdev_key = KEY_RIGHT,   .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_RIGHT },
    { .evdev_key = KEY_UP,      .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_UP },
    { .evdev_key = KEY_DOWN,    .need_shift = KEY_SHIFT_ANY,  .action = ACT_PAN_DOWN },
    { .evdev_key = KEY_EQUAL,   .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_IN },
    { .evdev_key = KEY_KPPLUS,  .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_IN },
    { .evdev_key = KEY_4,       .need_shift = KEY_SHIFT_DOWN, .action = ACT_ZOOM_IN },
    { .evdev_key = KEY_MINUS,   .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_OUT },
    { .evdev_key = KEY_KPMINUS, .need_shift = KEY_SHIFT_ANY,  .action = ACT_ZOOM_OUT },
    { .evdev_key = KEY_HOME,    .need_shift = KEY_SHIFT_ANY,  .action = ACT_SNAP_FRONT },
    { .evdev_key = KEY_KP1,     .need_shift = KEY_SHIFT_ANY,  .action = ACT_SNAP_FRONT },
    { .evdev_key = KEY_M,       .need_shift = KEY_SHIFT_ANY,  .action = ACT_PLOT_CYCLE },
    { .evdev_key = KEY_L,       .need_shift = KEY_SHIFT_ANY,  .action = ACT_DATA_RELOAD },
    { .evdev_key = KEY_TAB,     .need_shift = KEY_SHIFT_ANY,  .action = ACT_SWITCH_MODE },
};

static_assert(sizeof k_blender_binds / sizeof k_blender_binds[0] == 33,
              "3D workspace bind count");
static_assert(sizeof k_plot_binds / sizeof k_plot_binds[0] == 14,
              "plot workspace bind count");
static_assert(sizeof k_blender_binds / sizeof k_blender_binds[0] <= k_max_key_binds,
              "3D binds fit the table");
static_assert(sizeof k_plot_binds / sizeof k_plot_binds[0] <= k_max_key_binds,
              "plot binds fit the table");

static WorkspacePolicy s_blender;
static WorkspacePolicy s_plot;
static WorkspacePolicy s_live;
static bool            s_profiles_ready = false;
static bool            s_booted = false;
static uint32_t        s_which = 0;

static uint32_t bind_limit(const WorkspacePolicy *p) {
    if (p->bind_count > k_max_key_binds) {
        return k_max_key_binds;
    }
    return p->bind_count;
}

static bool bind_shift_ok(uint8_t need, bool shift) {
    if (need == KEY_SHIFT_ANY) {
        return true;
    }
    if (need == KEY_SHIFT_DOWN) {
        return shift;
    }
    return !shift;
}

static void fill_one(WorkspacePolicy *dst, const char *name, uint32_t perms,
                     bool sealed, bool lock, const KeyBind *binds, uint32_t count) {
    memset(dst, 0, sizeof *dst);
    snprintf(dst->name, sizeof dst->name, "%s", name);
    dst->active_perms = perms;
    dst->sealed = sealed;
    dst->lock_2d_quat = lock;
    dst->bind_count = count;
    memcpy(dst->binds, binds, (size_t)count * sizeof(KeyBind));
}

static void fill_profiles(void) {
    if (s_profiles_ready) {
        return;
    }
    fill_one(&s_blender, "Blender-3D", k_perm_blender, k_blender_sealed, false,
             k_blender_binds,
             (uint32_t)(sizeof k_blender_binds / sizeof k_blender_binds[0]));
    fill_one(&s_plot, "Matplotlib-2D", k_perm_plot, k_plot_sealed, true,
             k_plot_binds,
             (uint32_t)(sizeof k_plot_binds / sizeof k_plot_binds[0]));
    s_profiles_ready = true;
}

bool policy_can(const WorkspacePolicy *p, EngineAction act) {
    if (p == nullptr || act <= ACT_NONE || act >= ACT_COUNT) {
        return false;
    }
    uint32_t req = k_action_perm[act];
    if (req == PERM_NONE) {
        return false;
    }
    return (p->active_perms & req) == req;
}

bool policy_allows(uint32_t perm) {
    const WorkspacePolicy *p = policy_active();
    if (perm == PERM_NONE) {
        return false;
    }
    return (p->active_perms & perm) == perm;
}

bool policy_grant(WorkspacePolicy *p, uint32_t perms) {
    if (p == nullptr || p->sealed) {
        return false;
    }
    p->active_perms |= perms;
    return true;
}

bool policy_revoke(WorkspacePolicy *p, uint32_t perms) {
    if (p == nullptr || p->sealed) {
        return false;
    }
    p->active_perms &= ~perms;
    return true;
}

bool policy_rebind_key(WorkspacePolicy *p, uint16_t evdev_key,
                       uint8_t need_shift, EngineAction new_action) {
    if (p == nullptr || p->sealed) {
        return false;
    }
    uint32_t n = bind_limit(p);
    for (uint32_t i = 0; i < n; ++i) {
        if (p->binds[i].evdev_key == evdev_key && p->binds[i].need_shift == need_shift) {
            p->binds[i].action = new_action;
            return true;
        }
    }
    if (p->bind_count >= k_max_key_binds) {
        return false;
    }
    p->binds[p->bind_count++] = (KeyBind){
        .evdev_key  = evdev_key,
        .need_shift = need_shift,
        .action     = new_action,
    };
    return true;
}

EngineAction policy_lookup(const WorkspacePolicy *p, uint16_t evdev_key, bool shift) {
    if (p == nullptr) {
        return ACT_NONE;
    }
    EngineAction loose = ACT_NONE;
    bool saw_loose = false;
    uint32_t n = bind_limit(p);
    for (uint32_t i = 0; i < n; ++i) {
        const KeyBind *b = &p->binds[i];
        if (b->evdev_key != evdev_key || !bind_shift_ok(b->need_shift, shift)) {
            continue;
        }
        if (b->need_shift == KEY_SHIFT_ANY) {
            if (!saw_loose) {
                loose = b->action;
                saw_loose = true;
            }
            continue;
        }
        return b->action;
    }
    return loose;
}

KeyRoute policy_route(const WorkspacePolicy *p, uint16_t evdev_key, bool shift) {
    EngineAction act = policy_lookup(p, evdev_key, shift);
    if (!policy_can(p, act)) {
        return KEY_ROUTE_IGNORE;
    }
    if (act >= ACT_PAN_LEFT && act <= ACT_PITCH_DOWN) {
        return KEY_ROUTE_HOLD;
    }
    return KEY_ROUTE_TAP;
}

int policy_held_axis(const WorkspacePolicy *p, EngineAction positive,
                     EngineAction negative, bool shift,
                     PolicyKeyDown down, void *user) {
    if (p == nullptr || down == nullptr) {
        return 0;
    }
    int pos = 0;
    int neg = 0;
    uint32_t n = bind_limit(p);
    for (uint32_t i = 0; i < n; ++i) {
        const KeyBind *b = &p->binds[i];
        if (!bind_shift_ok(b->need_shift, shift)) {
            continue;
        }
        if (!down(b->evdev_key, user)) {
            continue;
        }
        EngineAction act = policy_lookup(p, b->evdev_key, shift);
        if (!policy_can(p, act)) {
            continue;
        }
        if (act == positive) {
            pos = 1;
        } else if (act == negative) {
            neg = 1;
        }
    }
    return pos - neg;
}

const WorkspacePolicy *policy_blender(void) {
    fill_profiles();
    return &s_blender;
}

const WorkspacePolicy *policy_matplotlib(void) {
    fill_profiles();
    return &s_plot;
}

void policy_boot(void) {
    if (s_booted) {
        return;
    }
    fill_profiles();
    s_live = s_blender;
    s_which = 0;
    s_booted = true;
    printf("[Policy] %s.\n", s_live.name);
    fflush(stdout);
}

WorkspacePolicy *policy_active(void) {
    if (!s_booted) {
        policy_boot();
    }
    return &s_live;
}

bool policy_switch(void) {
    if (!s_booted) {
        policy_boot();
    }
    if (s_live.sealed) {
        return false;
    }
    s_which ^= 1u;
    s_live = (s_which == 0u) ? s_blender : s_plot;
    printf("[Policy] %s.\n", s_live.name);
    fflush(stdout);
    return true;
}
