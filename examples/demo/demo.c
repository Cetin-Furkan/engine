#include "engine/app.h"
#include "frame/camera.h"
#include "frame/cube.h"
#include "frame/gimbal.h"
#include "frame/grid.h"
#include "frame/hud.h"
#include "frame/material.h"
#include "frame/mesh.h"
#include "frame/plane.h"
#include "frame/plot.h"
#include "frame/shape.h"
#include "frame/solid.h"
#include "frame/world.h"
#include "world/world_int.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

// The hosted program: one bevelled heat cube, click-built faces, six
// raised solids, a math plot, a CSV chart, and an OBJ mesh, all placed
// by world/opening.world. The engine knows none of these names; it only
// calls the EngineApp hooks below on its io_uring thread.

// ---------------------------------------------------------------- targets

// The engine copies EngineTargets by value (a resize installs a fresh
// slot's handles into the shown slot), so ownership cannot hang off the
// EngineTargets address. Entries are keyed by the depth image handle,
// which survives the copy. Poisoned entries (image zeroed on destroy)
// keep a stale handle from ever matching a recycled one.
enum : uint32_t { k_target_cap = 16 };

typedef struct TargetEntry {
    CubeDepth    depth;
    CubeCoverage coverage;
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
    if (cube_depth_create(vk, &entry->depth, width, height) != 0 ||
        cube_coverage_create(vk, &entry->coverage, width, height) != 0) {
        cube_depth_destroy(vk, &entry->depth);
        cube_coverage_destroy(vk, &entry->coverage);
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
    return 0;
}

static void demo_destroy_targets(VulkanContext *vk, EngineTargets *targets) {
    if (targets == nullptr) {
        return;
    }
    TargetEntry *entry = target_find(targets);
    if (entry != nullptr) {
        cube_depth_destroy(vk, &entry->depth);
        cube_coverage_destroy(vk, &entry->coverage);
        entry->used = false;
        // Poison: a copied EngineTargets still naming these handles must
        // not match a future entry that recycles the same image handle
        // with a different generation. Handles are zeroed by destroy.
    }
    *targets = (EngineTargets){0};
}

// ------------------------------------------------------------------- hooks

static int demo_init(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user) {
    if (cube_init(vk) != 0 || solid_init(vk) != 0 || shape_init(vk) != 0 ||
        plane_init(vk) != 0 || material_init(vk) != 0 ||
        plot_init(vk, forward_cqe, forward_user) != 0 ||
        mesh_init(vk, forward_cqe, forward_user) != 0 ||
        gimbal_init(vk) != 0 || hud_init(vk) != 0 || grid_init(vk) != 0) {
        return -1;
    }
    return 0;
}

static void demo_shutdown(VulkanContext *vk) {
    grid_shutdown(vk);
    hud_shutdown(vk);
    gimbal_shutdown(vk);
    mesh_shutdown(vk);
    plot_shutdown(vk);
    material_shutdown(vk);
    plane_shutdown(vk);
    shape_shutdown(vk);
    solid_shutdown(vk);
    cube_shutdown(vk);
}

static void demo_record_compute(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    world_record(cmd, heat_dt, live);
}

static void demo_prepare_draw(VkCommandBuffer cmd, VkImage depth_image) {
    cube_prepare_draw(cmd, depth_image);
    plot_prepare(cmd);
}

static void demo_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    shape_begin(acquire, signal_point);
}

static void demo_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    world_draw(cmd, width, height, live);
}

static void demo_draw_overlay(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                              VkFormat format, uint32_t frame_ms, bool have_stamp,
                              uint64_t modifier) {
    hud_prepare(frame_ms, have_stamp, modifier);
    hud_draw(cmd, width, height, format);
}

static bool demo_simulation(uint32_t time_ms, const FrameStep *step, uint32_t *heat_dt) {
    (void)step;  // Heat diffusion is stamp-driven; dt arrives clamped.
    return world_simulation(time_ms, heat_dt);
}

static bool demo_advance(uint32_t time_ms, const FrameStep *step) {
    (void)step;  // Poses are absolute-stamp functions (spin/camera).
    return world_advance(time_ms);
}

static bool demo_click(float origin_x, float origin_y) {
    return world_click(origin_x, origin_y);
}

static bool demo_key(uint32_t key, EngineKeyContext *ctx) {
    if (key == KEY_ESC) {
        return world_cancel();
    }
    if (key == KEY_BACKSPACE) {
        return world_delete();
    }
    if (key == KEY_F) {
        return world_close_face();
    }
    if (key == KEY_F5) {
        return world_respin_selected();
    }
    if (key == KEY_R) {
        return world_toggle_spin();
    }
    if (key == KEY_S) {
        return world_toggle_smooth();
    }
    if (key == KEY_G) {
        return world_toggle_glow();
    }
    if (key == KEY_M) {
        return plot_cycle();
    }
    if (key == KEY_L) {
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
    if (key == KEY_HOME || key == KEY_KP1) {
        return camera_snap_front();
    }
    if (key >= KEY_1 && key <= KEY_6) {
        return world_raise_solid(key - KEY_1);
    }
    return false;
}

static bool demo_camera_active(void) {
    return camera_motion_down() || camera_busy();
}

static const EngineApp s_app = {
    .help = "One turn, then io_uring idle. Clicks on empty space place points. "
            "F closes those points into a face. A click on a mesh selects it. "
            "The cube click also plants heat for five seconds, and it can be "
            "selected while other meshes are up. Its colors sweep on that "
            "clock, then hold. Keys 1-6 raise a tetrahedron, octahedron, "
            "icosahedron, cylinder, cone, and torus. R spins until R again. "
            "S smooths edges. G opens or closes glow. Esc clears, Backspace "
            "removes, F5 turns the selection again. M cycles the math plot. "
            "L reloads the CSV chart and the OBJ mesh. Home or numpad 1 "
            "snaps the front view. Arrows move on the screen axes. Q and E "
            "yaw, C and V pitch. Minus zooms out. Plus is numpad plus, "
            "shift+4, or equals.",
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
    .release_frame     = shape_release,
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
    .key               = demo_key,
    .camera_active     = demo_camera_active,
};

const EngineApp *engine_app(void) {
    return &s_app;
}
