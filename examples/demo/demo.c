#include "chrome/gpu.h"
#include "engine/app.h"
#include "frame/camera.h"
#include "frame/cube.h"
#include "frame/gimbal.h"
#include "frame/grid.h"
#include "frame/hud.h"
#include "frame/material.h"
#include "frame/mesh.h"
#include "frame/pass.h"
#include "frame/plane.h"
#include "frame/policy.h"
#include "frame/plot.h"
#include "frame/shape.h"
#include "frame/solid.h"
#include "frame/text.h"
#include "frame/world.h"
#include "input/pointer.h"
#include "world/world_int.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

// The hosted program. Boot draws the metric grid and no objects.
// opening.world places each cube line on z = 0. Clicks build faces.
// Keys 1-6 raise solids. M cycles the math plot. L reloads the CSV
// chart and the OBJ. Tab swaps the flat plot workspace and the 3D
// workspace. The engine knows none of these names; it only calls
// the EngineApp hooks below on its io_uring thread.

// ---------------------------------------------------------------- targets

// The engine copies EngineTargets by value (a resize installs a fresh
// slot's handles into the shown slot), so ownership cannot hang off the
// EngineTargets address. Entries are keyed by the depth image handle,
// which survives the copy. Poisoned entries (image zeroed on destroy)
// keep a stale handle from ever matching a recycled one.
enum : uint32_t { k_target_cap = 16 };

typedef struct TargetEntry {
    PassDepth    depth;
    PassCoverage coverage;
    uint32_t     generation;
    bool         used;
} TargetEntry;

static TargetEntry s_targets[k_target_cap];
static uint32_t    s_target_generation = 1;

static TargetEntry *target_find(const EngineTargets *targets) {
    if (targets == nullptr || targets->depth_image == VK_NULL_HANDLE) {
        return nullptr;
    }
    for (uint32_t i = 0; i < k_target_cap; ++i) {
        if (s_targets[i].used && s_targets[i].depth.image == targets->depth_image) {
            return &s_targets[i];
        }
    }
    return nullptr;
}

static int demo_create_targets(VulkanContext *vk, EngineTargets *targets,
                               uint32_t width, uint32_t height) {
    if (vk == nullptr || targets == nullptr) {
        return -1;
    }
    TargetEntry *entry = target_find(targets);
    if (entry == nullptr) {
        for (uint32_t i = 0; i < k_target_cap; ++i) {
            if (!s_targets[i].used) {
                entry = &s_targets[i];
                break;
            }
        }
    }
    if (entry == nullptr) {
        fprintf(stderr, "[Demo] No free render-target slot\n");
        return -1;
    }
    if (pass_depth_create(vk, &entry->depth, width, height) != 0 ||
        pass_coverage_create(vk, &entry->coverage, width, height) != 0) {
        pass_depth_destroy(vk, &entry->depth);
        pass_coverage_destroy(vk, &entry->coverage);
        entry->used = false;
        *targets = (EngineTargets){0};
        return -1;
    }
    entry->generation = s_target_generation++;
    if (s_target_generation == 0) {
        s_target_generation = 1;
    }
    entry->used = true;
    targets->depth_image     = entry->depth.image;
    targets->depth_view      = entry->depth.view;
    targets->coverage_image  = entry->coverage.image;
    targets->coverage_view   = entry->coverage.view;
    targets->depth_format    = pass_depth_format();
    targets->samples         = pass_raster_samples();
    return 0;
}

static void demo_destroy_targets(VulkanContext *vk, EngineTargets *targets) {
    if (targets == nullptr) {
        return;
    }
    TargetEntry *entry = target_find(targets);
    if (entry != nullptr) {
        pass_depth_destroy(vk, &entry->depth);
        pass_coverage_destroy(vk, &entry->coverage);
        entry->used = false;
        // Poison: a copied EngineTargets still naming these handles must
        // not match a future entry that recycles the same image handle
        // with a different generation. Handles are zeroed by destroy.
    }
    *targets = (EngineTargets){0};
}

// ------------------------------------------------------------------- hooks

static int demo_init(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user) {
    policy_boot();
    if (pass_init(vk) != 0 ||
        chrome_gpu_bind_pass(vk, pass_depth_format(), pass_raster_samples()) != 0 ||
        cube_init(vk) != 0 || solid_init(vk) != 0 || shape_init(vk) != 0 ||
        plane_init(vk) != 0 || material_init(vk) != 0 ||
        plot_init(vk, forward_cqe, forward_user) != 0 ||
        mesh_init(vk, forward_cqe, forward_user) != 0 ||
        gimbal_init(vk) != 0 || hud_init(vk) != 0 || text_init(vk) != 0 || grid_init(vk) != 0) {
        return -1;
    }
    return 0;
}

static void demo_shutdown(VulkanContext *vk) {
    grid_shutdown(vk);
    text_shutdown(vk);
    hud_shutdown(vk);
    gimbal_shutdown(vk);
    mesh_shutdown(vk);
    plot_shutdown(vk);
    material_shutdown(vk);
    plane_shutdown(vk);
    shape_shutdown(vk);
    solid_shutdown(vk);
    cube_shutdown(vk);
    pass_shutdown(vk);
}

static void demo_record_compute(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    world_record(cmd, heat_dt, live);
}

static void demo_prepare_draw(VkCommandBuffer cmd, VkImage depth_image) {
    (void)depth_image;
    plot_prepare(cmd);
}

static void demo_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    shape_begin(acquire, signal_point);
    text_begin(acquire, signal_point);
    plot_begin(acquire, signal_point);
    mesh_begin(acquire, signal_point);
}

static void demo_release_frame(uint64_t signal_point) {
    shape_release(signal_point);
    text_release(signal_point);
    plot_release(signal_point);
    mesh_release(signal_point);
}

static void demo_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    world_draw(cmd, width, height, live);
}

static void demo_draw_overlay(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                              VkFormat format, VkFormat depth_format,
                              VkSampleCountFlagBits samples, uint32_t frame_ms,
                              bool have_stamp, uint64_t modifier) {
    hud_prepare(frame_ms, have_stamp, modifier);
    hud_draw(cmd, width, height, format, depth_format, samples);
    text_draw(cmd, width, height, format, depth_format, samples);
}

static bool demo_simulation(uint32_t time_ms, const FrameStep *step, uint32_t *heat_dt) {
    (void)step;  // Heat diffusion is stamp-driven; dt arrives clamped.
    return world_simulation(time_ms, heat_dt);
}

static bool demo_advance(uint32_t time_ms, const FrameStep *step) {
    (void)step;  // Poses are absolute-stamp functions (spin/camera).
    int32_t px = 0;
    int32_t py = 0;
    bool have = pointer_client_xy(&px, &py);
    text_arm_tick(pointer_key_down(KEY_T), have, px, py);
    return world_advance(time_ms);
}

static bool demo_click(float origin_x, float origin_y) {
    if (text_click(origin_x, origin_y, pointer_key_down(KEY_T))) {
        return true;
    }
    return world_click(origin_x, origin_y);
}

static bool demo_text(const char *utf8, uint32_t bytes) {
    return text_insert(utf8, bytes);
}

static bool demo_scroll(float steps) {
    return camera_zoom_steps(steps);
}

static bool demo_shift_down(void) {
    return pointer_key_down(KEY_LEFTSHIFT) || pointer_key_down(KEY_RIGHTSHIFT);
}

static uint8_t demo_route_key(uint32_t key) {
    if (text_editing()) {
        if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
            return ENGINE_KEY_HOLD;
        }
        return ENGINE_KEY_TAP;
    }
    if (key == KEY_T) {
        return ENGINE_KEY_HOLD;
    }
    KeyRoute route = policy_route(policy_active(), (uint16_t)key, demo_shift_down());
    if (route == KEY_ROUTE_HOLD) {
        return ENGINE_KEY_HOLD;
    }
    if (route == KEY_ROUTE_TAP) {
        return ENGINE_KEY_TAP;
    }
    return ENGINE_KEY_IGNORE;
}

static bool demo_key(uint32_t key, EngineKeyContext *ctx) {
    if (text_on_key(key)) {
        return true;
    }
    EngineAction act = policy_lookup(policy_active(), (uint16_t)key, demo_shift_down());
    if (!policy_can(policy_active(), act)) {
        return false;
    }
    switch (act) {
    case ACT_CANCEL:
        return world_cancel();
    case ACT_DELETE:
        return world_delete();
    case ACT_CLOSE_FACE:
        return world_close_face();
    case ACT_RESPIN:
        return world_respin_selected();
    case ACT_TOGGLE_SPIN:
        return world_toggle_spin();
    case ACT_TOGGLE_SMOOTH:
        return world_toggle_smooth();
    case ACT_TOGGLE_GLOW:
        return world_toggle_glow();
    case ACT_PLOT_CYCLE:
        return plot_cycle();
    case ACT_DATA_RELOAD: {
        EngineRelay relay;
        engine_relay_begin(&relay, ctx->signal_armed, ctx->got_signal,
                           ctx->signo, ctx->inotify_armed);
        bool changed = plot_reload(ctx->reload_cqe, nullptr);
        bool mesh_has = false;
        bool mesh_bytes = mesh_reload(ctx->reload_cqe, nullptr, &mesh_has);
        if (world_apply_mesh(mesh_has, mesh_bytes)) {
            changed = true;
        }
        if (engine_relay_end(&relay, ctx->wl, ctx->vk, ctx->pending_serial,
                             ctx->send_in_flight, ctx->resend_wire,
                             ctx->run_rc, ctx->why)) {
            *ctx->stop = true;
        }
        return changed;
    }
    case ACT_SNAP_FRONT:
        return camera_snap_front();
    case ACT_HOME:
        return camera_home();
    case ACT_VIEW_TOP:
        return camera_view_top();
    case ACT_VIEW_BOTTOM:
        return camera_view_bottom();
    case ACT_VIEW_LEFT:
        return camera_view_left();
    case ACT_VIEW_RIGHT:
        return camera_view_right();
    case ACT_VIEW_FRONT:
        return camera_view_front();
    case ACT_TOGGLE_DRAW: {
        WorkspacePolicy *live = policy_active();
        if (policy_allows(PERM_DRAW)) {
            if (!policy_revoke(live, PERM_DRAW)) {
                return false;
            }
            (void)face_cancel_chain();
            printf("[Draw] Points are off.\n");
            fflush(stdout);
            return true;
        }
        if (!policy_grant(live, PERM_DRAW)) {
            return false;
        }
        printf("[Draw] Points are on.\n");
        fflush(stdout);
        return true;
    }
    case ACT_TOGGLE_GIMBAL:
        return gimbal_toggle();
    case ACT_SPAWN_SOLID_1:
    case ACT_SPAWN_SOLID_2:
    case ACT_SPAWN_SOLID_3:
    case ACT_SPAWN_SOLID_4:
    case ACT_SPAWN_SOLID_5:
    case ACT_SPAWN_SOLID_6:
        return world_raise_solid((uint32_t)(act - ACT_SPAWN_SOLID_1));
    case ACT_SWITCH_MODE:
        if (!policy_switch()) {
            return false;
        }
        if (policy_active()->lock_2d_quat) {
            (void)camera_snap_front();
        }
        return true;
    default:
        return false;
    }
}

static bool demo_camera_active(void) {
    return camera_motion_down() || camera_busy();
}

static const EngineApp s_app = {
    .help = "One turn, then io_uring idle. Points stay off until numpad 9. "
            "Empty clicks then place points, and F closes them into a face that stays. "
            "R spins until R again. F5 turns the selection again. A click on a mesh "
            "selects it. A cube click also plants heat for five seconds, and it can be "
            "selected while other meshes are up. Its colors sweep on that clock, then hold. "
            "Keys 1-6 raise a tetrahedron, octahedron, icosahedron, cylinder, cone, and torus. "
            "S smooths edges. G opens or closes glow. Esc clears. Backspace, Delete, or X removes. "
            "M cycles the math plot. L reloads the CSV chart and the OBJ mesh. "
            "Numpad 8, 2, 4, 6, and 5 set top, bottom, left, right, and front. "
            "Numpad 5 again sets the back. Home or numpad 1 restores the opening camera, "
            "including zoom and position. Numpad 7 shows or hides the view ball. "
            "The wheel and the touchpad zoom. Arrows move on the screen axes. Q and E "
            "yaw, C and V pitch. Minus zooms out. Plus is numpad plus, shift+4, or equals. "
            "Tab switches the flat plot workspace and the 3D workspace. "
            "Hold T and click twice to place a text box, then type. Escape leaves it.",
    .init              = demo_init,
    .shutdown          = demo_shutdown,
    .load              = world_load,
    .reset             = world_reset,
    .create_targets    = demo_create_targets,
    .destroy_targets   = demo_destroy_targets,
    .record_compute    = demo_record_compute,
    .prepare_draw      = demo_prepare_draw,
    .claim_frame       = demo_claim_frame,
    .draw              = demo_draw,
    .draw_overlay      = demo_draw_overlay,
    .record_abort      = world_record_abort,
    .record_ok         = world_record_ok,
    .record_published  = world_record_published,
    .record_step_open  = world_record_step_open,
    .record_poke_pending = world_record_poke_pending,
    .release_frame     = demo_release_frame,
    .simulation        = demo_simulation,
    .simulation_hold   = world_simulation_hold,
    .simulation_commit = world_simulation_commit,
    .advance           = demo_advance,
    .busy              = world_busy,
    .consume_spin      = world_consume_spin,
    .commit            = world_commit,
    .note_cqe          = hud_note_cqe,
    .turn_report       = hud_turn_report,
    .click             = demo_click,
    .scroll            = demo_scroll,
    .text              = demo_text,
    .key               = demo_key,
    .camera_active     = demo_camera_active,
    .route_key         = demo_route_key,
};

const EngineApp *engine_app(void) {
    return &s_app;
}
