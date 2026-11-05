#include "chrome/gpu.h"
#include "engine/app.h"
#include "vulkan/bda.h"
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
#include "frame/sheet.h"
#include "frame/solid.h"
#include "frame/terrain.h"
#include "frame/text.h"
#include "frame/world.h"
#include "input/pointer.h"
#include "world/world_int.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The hosted program. Boot draws the metric grid and no objects.
// opening.world places each cube line on z = 0. Clicks build faces.
// Keys 1-6 raise solids. M cycles the math plot. L reloads the CSV
// chart and the OBJ, and in 2D or Hybrid the sheet document too.
// Tab cycles 3D, 2D, and Hybrid. The engine knows none of these names;
// it only calls the EngineApp hooks below on its io_uring thread.

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

static void print_worlds_usage(const char *prog) {
    printf("Usage: %s [--world=<0..8> | -w <0..8>] [--list-worlds] [--help]\n\n"
           "Selectable Worlds & Capability Masks:\n"
           "  --world=0  Default Sandbox (3D / 2D / Hybrid switchable via Tab)\n"
           "  --world=1  [3D-Only | PERM_DRAW=on  | Tab=locked] Click-to-Draw Studio (Points -> Lines -> Faces)\n"
           "  --world=2  [3D-Only | Gravity+Bounce| Tab=locked] Non-Cube Solids Squash-and-Stretch Drop\n"
           "  --world=3  [2D-Only | Gravity+Soft  | Tab=locked] Deformable Soft Patches & Elastic Polygons\n"
           "  --world=4  [2D-Only | PERM_DRAW=on  | Tab=locked] 2D Click-to-Draw Polygon Workshop & Sheet\n"
           "  --world=5  [Hybrid  | 3D+2D+Draw    | Tab=open  ] Combined 3D+2D Gravity, Soft Bodies & Draw\n"
           "  --world=6  [3D+2D   | Multi-Bounce  | Tab=open  ] Restitution Staircase (0.08..0.94) & Bumper Lab\n"
           "  --world=7  [Hybrid  | C23 Builder   | Tab=open  ] Procedural Kinetic Cathedral & Polyring Cascade\n"
           "  --world=8  [3D/Map  | RelPtr2D+SSD  | Tab=open  ] Baklan & Beşparmak Dağı Real DEM + Satellite Map\n",
           prog != nullptr ? prog : "engine");
    fflush(stdout);
}

static bool parse_world_arg(const char *arg, uint32_t *out_id) {
    if (arg == nullptr || arg[0] == '\0' || out_id == nullptr) {
        return false;
    }
    if (strcmp(arg, "default") == 0) {
        *out_id = 0u;
        return true;
    }
    if (strcmp(arg, "draw3d") == 0) {
        *out_id = 1u;
        return true;
    }
    if (strcmp(arg, "gravity3d") == 0 || strcmp(arg, "solids") == 0) {
        *out_id = 2u;
        return true;
    }
    if (strcmp(arg, "soft2d") == 0 || strcmp(arg, "2d") == 0) {
        *out_id = 3u;
        return true;
    }
    if (strcmp(arg, "draw2d") == 0) {
        *out_id = 4u;
        return true;
    }
    if (strcmp(arg, "hybrid") == 0) {
        *out_id = 5u;
        return true;
    }
    if (strcmp(arg, "bounce") == 0 || strcmp(arg, "elastic") == 0) {
        *out_id = 6u;
        return true;
    }
    if (strcmp(arg, "procedural") == 0 || strcmp(arg, "cathedral") == 0) {
        *out_id = 7u;
        return true;
    }
    if (strcmp(arg, "map") == 0 || strcmp(arg, "besparmak") == 0 || strcmp(arg, "baklan") == 0) {
        *out_id = 8u;
        return true;
    }
    char *end = nullptr;
    unsigned long v = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' || v > (unsigned long)k_world_preset_max) {
        return false;
    }
    *out_id = (uint32_t)v;
    return true;
}

static int demo_configure(int argc, char **argv) {
    uint32_t chosen = 0u;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a == nullptr) {
            continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "--list-worlds") == 0) {
            print_worlds_usage(argv[0]);
            return 1;
        }
        const char *val = nullptr;
        if (strncmp(a, "--world=", 8) == 0) {
            val = a + 8;
        } else if (strncmp(a, "-w=", 3) == 0) {
            val = a + 3;
        } else if (strcmp(a, "--world") == 0 || strcmp(a, "-w") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "[Engine] Missing world number after '%s' (expected 0..%u)\n",
                        a, k_world_preset_max);
                print_worlds_usage(argv[0]);
                return -1;
            }
            val = argv[++i];
        } else if (strncmp(a, "-w", 2) == 0 && a[2] != '\0') {
            val = a + 2;
        } else {
            fprintf(stderr, "[Engine] Unknown option '%s'\n", a);
            print_worlds_usage(argv[0]);
            return -1;
        }
        if (!parse_world_arg(val, &chosen)) {
            fprintf(stderr, "[Engine] Invalid world '%s' (expected 0..%u)\n",
                    val, k_world_preset_max);
            print_worlds_usage(argv[0]);
            return -1;
        }
    }
    world_select(chosen);
    return 0;
}

static int demo_init(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user) {
    policy_boot();
    world_apply_policy();
    if (pass_init(vk) != 0 ||
        chrome_gpu_bind_pass(vk, pass_depth_format(), pass_raster_samples()) != 0 ||
        cube_init(vk) != 0 || solid_init(vk) != 0 || shape_init(vk) != 0 ||
        plane_init(vk) != 0 || material_init(vk) != 0 ||
        plot_init(vk, forward_cqe, forward_user) != 0 ||
        mesh_init(vk, forward_cqe, forward_user) != 0 ||
        terrain_init(vk) != 0 ||
        gimbal_init(vk) != 0 || hud_init(vk) != 0 || text_init(vk) != 0 || grid_init(vk) != 0) {
        return -1;
    }
    return 0;
}

static void demo_shutdown(VulkanContext *vk) {
    (void)world_save();
    (void)sheet_save();
    grid_shutdown(vk);
    text_shutdown(vk);
    hud_shutdown(vk);
    gimbal_shutdown(vk);
    terrain_shutdown(vk);
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
    // This slot's previous acquire has already retired. Rewind only its
    // half of the transient bump. The other slot may still be reading.
    bda_transient_begin_frame(signal_point);
    cube_bind_acquire(acquire);
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
    uint32_t sim_dt = step != nullptr ? step->sim_dt_ms : 0u;
    return world_simulation(time_ms, sim_dt, heat_dt);
}

static bool demo_advance(uint32_t time_ms, const FrameStep *step) {
    (void)step;  // time_ms is the sim cursor issued with step->sim_dt_ms.
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
    case ACT_RESTORE:
        return world_restore();
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
        if (policy_mode() != MODE_2D) {
            bool mesh_has = false;
            bool mesh_bytes = mesh_reload(ctx->reload_cqe, nullptr, &mesh_has);
            if (world_apply_mesh(mesh_has, mesh_bytes)) {
                changed = true;
            }
            if (terrain_reload()) {
                changed = true;
            }
        }
        if (policy_mode() != MODE_3D && sheet_reload()) {
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

static bool demo_drag(float dx, float dy, uint32_t buttons) {
    return camera_pointer_drag(dx, dy, buttons);
}

static const EngineApp s_app = {
    .help = "One turn, then io_uring idle. Points stay off until numpad 9. "
            "Empty clicks then place points, and F closes them into a face that stays. "
            "R spins until R again. F5 turns the selection again. A click on a mesh "
            "selects it. A cube click plants heat for five seconds and shoves the cube. "
            "It stops on the ground and against other cubes, and that place is saved. "
            "It can be selected while other meshes are up. Its colors sweep on that clock, then hold. "
            "Keys 1-6 raise a tetrahedron, octahedron, icosahedron, cylinder, cone, and torus. "
            "S smooths edges. G opens or closes glow. Esc clears. Backspace, Delete, or X removes. "
            "Z puts the last removed cube, solid, or mesh back. "
            "M cycles the math plot. L reloads the CSV chart and the OBJ mesh. "
            "In 2D and Hybrid, L also reads sheet.world again. "
            "Numpad 8, 2, 4, 6, and 5 set top, bottom, left, right, and front. "
            "Numpad 5 again sets the back. Home or numpad 1 restores the opening camera, "
            "including zoom and position. Numpad 7 shows or hides the view ball. "
            "The wheel and the touchpad zoom. Right-drag orbits. Middle-drag pans. "
            "Arrows move on the screen axes. Q and E "
            "yaw, C and V pitch. Minus zooms out. Plus is numpad plus, shift+4, or equals. "
            "Tab cycles 3D, 2D, and Hybrid. 2D is a flat sheet of rigid boxes, "
            "a soft patch, and shapes from sheet.world. It does not step the fillets "
            "or the lattice. Hybrid draws the 3D scene and that sheet. "
            "Hold T and click twice to place a text box, then type. Escape leaves it. "
            "Edits are written to examples/demo/world/live.world. Faces and heat stay unsaved.",
    .configure         = demo_configure,
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
    .drag              = demo_drag,
    .scroll            = demo_scroll,
    .text              = demo_text,
    .key               = demo_key,
    .camera_active     = demo_camera_active,
    .route_key         = demo_route_key,
};

const EngineApp *engine_app(void) {
    return &s_app;
}
