#ifndef ENGINE_FRAME_POLICY_H
#define ENGINE_FRAME_POLICY_H

#include <stdbool.h>
#include <stdint.h>

// Capability gate for one workspace. A physical evdev key becomes a
// semantic action, then the action runs only when its permission bit
// is set. The engine never sees these names: it asks the hosted
// program whether a press is held, tapped, or ignored.
//
// need_shift on a bind: 0 means shift must be up, 1 means shift must
// be down, 2 means either. An exact 0 or 1 match wins over a loose 2,
// so shift+4 can be zoom while 4 alone stays the cylinder.
//
// sealed rejects grant, revoke, rebind, and a live profile switch.
// lock_2d_quat is a pose constraint on top of the permission mask:
// the camera holds the identity quaternion and z = 0.

enum : uint32_t {
    PERM_NONE         = 0u,
    PERM_CAM_PAN_X    = 1u << 0,
    PERM_CAM_PAN_Y    = 1u << 1,
    PERM_CAM_ZOOM     = 1u << 2,
    PERM_CAM_YAW      = 1u << 3,
    PERM_CAM_PITCH    = 1u << 4,
    PERM_CAM_SNAP     = 1u << 5,
    PERM_OBJ_SELECT   = 1u << 6,
    PERM_OBJ_SPAWN    = 1u << 7,
    PERM_OBJ_DELETE   = 1u << 8,
    PERM_OBJ_SPIN     = 1u << 9,
    PERM_OBJ_MATERIAL = 1u << 10,
    PERM_OBJ_COMPUTE  = 1u << 11,
    PERM_PLOT_CYCLE   = 1u << 12,
    PERM_DATA_RELOAD  = 1u << 13,
    PERM_MODE_SWITCH  = 1u << 14,
    // Empty-ground clicks. Off in both compiled profiles, so a boot
    // places no corners. Numpad 9 grants and revokes it on the live copy.
    PERM_DRAW         = 1u << 15,
};

// Held actions stay a contiguous range so a route can tell a hold
// from a tap by the action's place in this list. ACT_PAN_LEFT through
// ACT_PITCH_DOWN are polled. Everything after them is one press.
typedef enum EngineAction : uint8_t {
    ACT_NONE = 0,
    ACT_PAN_LEFT,
    ACT_PAN_RIGHT,
    ACT_PAN_UP,
    ACT_PAN_DOWN,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_YAW_CCW,
    ACT_YAW_CW,
    ACT_PITCH_UP,
    ACT_PITCH_DOWN,
    ACT_SNAP_FRONT,
    ACT_CANCEL,
    ACT_DELETE,
    ACT_CLOSE_FACE,
    ACT_RESPIN,
    ACT_TOGGLE_SPIN,
    ACT_TOGGLE_SMOOTH,
    ACT_TOGGLE_GLOW,
    ACT_PLOT_CYCLE,
    ACT_DATA_RELOAD,
    ACT_SPAWN_SOLID_1,
    ACT_SPAWN_SOLID_2,
    ACT_SPAWN_SOLID_3,
    ACT_SPAWN_SOLID_4,
    ACT_SPAWN_SOLID_5,
    ACT_SPAWN_SOLID_6,
    ACT_SWITCH_MODE,
    ACT_HOME,
    ACT_VIEW_TOP,
    ACT_VIEW_BOTTOM,
    ACT_VIEW_LEFT,
    ACT_VIEW_RIGHT,
    ACT_VIEW_FRONT,
    ACT_TOGGLE_DRAW,
    ACT_TOGGLE_GIMBAL,
    ACT_COUNT
} EngineAction;

static_assert(ACT_PITCH_DOWN == ACT_PAN_LEFT + 9, "held actions stay contiguous");
static_assert(ACT_SPAWN_SOLID_6 == ACT_SPAWN_SOLID_1 + 5, "solid keys stay contiguous");

enum : uint8_t {
    KEY_SHIFT_UP   = 0,
    KEY_SHIFT_DOWN = 1,
    KEY_SHIFT_ANY  = 2,
};

typedef enum KeyRoute : uint8_t {
    KEY_ROUTE_IGNORE = 0,
    KEY_ROUTE_HOLD   = 1,
    KEY_ROUTE_TAP    = 2,
} KeyRoute;

typedef struct KeyBind {
    uint16_t     evdev_key;
    uint8_t      need_shift;
    EngineAction action;
} KeyBind;

enum { k_max_key_binds = 48 };

typedef struct WorkspacePolicy {
    char     name[24];
    uint32_t active_perms;
    bool     sealed;
    bool     lock_2d_quat;
    uint32_t bind_count;
    KeyBind  binds[k_max_key_binds];
} WorkspacePolicy;

// True while the named evdev key is down. user is the caller's state.
typedef bool (*PolicyKeyDown)(uint16_t evdev_key, void *user);

// False for a null policy, ACT_NONE, an unknown action, or a missing bit.
bool policy_can(const WorkspacePolicy *p, EngineAction act);

// True when every bit of perm is set on the live policy. Zero is false.
bool policy_allows(uint32_t perm);

// Live mutation. False when p is null or sealed; the mask is unchanged then.
bool policy_grant(WorkspacePolicy *p, uint32_t perms);
bool policy_revoke(WorkspacePolicy *p, uint32_t perms);

// Replaces a bind with the same key and need_shift, or appends one.
// False when sealed or the table is full.
bool policy_rebind_key(WorkspacePolicy *p, uint16_t evdev_key,
                       uint8_t need_shift, EngineAction new_action);

// The bound action, ignoring permissions. ACT_NONE when nothing matches.
// An exact shift match wins over a bind that accepts either.
EngineAction policy_lookup(const WorkspacePolicy *p, uint16_t evdev_key, bool shift);

// HOLD for a permitted continuous action, TAP for any other permitted
// action, IGNORE when the key is unbound or the permission is missing.
KeyRoute policy_route(const WorkspacePolicy *p, uint16_t evdev_key, bool shift);

// +1 when a permitted bind of `positive` is down, -1 for `negative`,
// 0 when both are down, neither is down, or the permission is missing.
int policy_held_axis(const WorkspacePolicy *p, EngineAction positive,
                     EngineAction negative, bool shift,
                     PolicyKeyDown down, void *user);

// Compiled profiles. Filling them does not select the live workspace.
const WorkspacePolicy *policy_blender(void);
const WorkspacePolicy *policy_matplotlib(void);

// Boot copies the 3D profile into the live workspace and logs once.
// A second call keeps a live switch and any rebind.
void policy_boot(void);

// The live workspace. Boots on first use so a camera step before
// demo_init still has the 3D keymap.
WorkspacePolicy *policy_active(void);

// Swaps the live workspace between the 3D profile and the flat plot
// profile. A switch drops rebinds on the previous copy. False when sealed.
bool policy_switch(void);

#endif /* ENGINE_FRAME_POLICY_H */
