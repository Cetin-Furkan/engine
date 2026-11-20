#include "world/world_int.h"
#include "backend/backend.h"
#include "frame/camera.h"
#include "frame/gimbal.h"
#include "frame/hud.h"
#include "frame/plot.h"
#include "frame/policy.h"
#include "frame/sheet.h"
#include "frame/terrain.h"
#include "frame/text.h"
#include "graph/graph.h"
#include "lighting/lighting.h"
#include "physics/physics.h"
#include "post/post.h"
#include "scene/scene.h"
#include <stdio.h>
#include <string.h>

// World lifecycle & per-frame orchestration.
// Presets live in presets.c; pointer/gizmo/ECS interaction lives in interact.c;
// object kinds live in kind/; faces in face/; scene parser in load.c.

enum : uint32_t { k_removed_cap = 24u };

static WorldRemoved s_removed[k_removed_cap];
static uint32_t     s_removed_n = 0u;

void world_removed_clear(void) {
    s_removed_n = 0u;
}

void world_removed_push(const WorldRemoved *item) {
    if (item == nullptr) {
        return;
    }
    if (s_removed_n == k_removed_cap) {
        memmove(s_removed, s_removed + 1, (k_removed_cap - 1u) * sizeof s_removed[0]);
        s_removed_n = k_removed_cap - 1u;
    }
    s_removed[s_removed_n++] = *item;
}

uint32_t world_removed_count(void) {
    return s_removed_n;
}

const WorldRemoved *world_removed_at(uint32_t index) {
    if (index >= s_removed_n) {
        return nullptr;
    }
    return &s_removed[index];
}

static bool snap_removed(const WorldObj *o, WorldRemoved *out) {
    if (o == nullptr || o->ops == nullptr || out == nullptr) {
        return false;
    }
    WorldRemoved g = { .at = o->at };
    if (o->spin != nullptr) {
        g.yaw_milli = o->spin->live_milli;
        g.spinning  = o->spin->spinning;
        g.infinite  = o->spin->infinite;
        g.glow      = o->spin->glow;
    }
    if (o->ops == cube_ops()) {
        g.kind        = k_removed_cube;
        g.restitution = cube_kind_restitution(o);
    } else if (o->ops == solid_ops()) {
        const char *name = o->ops->name != nullptr ? o->ops->name(o) : nullptr;
        int kind = solid_kind_index(name);
        if (kind < 0) {
            return false;
        }
        g.kind        = k_removed_solid;
        g.solid       = kind;
        g.restitution = solid_kind_restitution(o);
    } else if (o->ops == mesh_ops()) {
        g.kind = k_removed_mesh;
    } else {
        return false;
    }
    *out = g;
    return true;
}

void world_reset(void) {
    world_interact_reset();
    world_removed_clear();
    table_reset();
    ground_reset();
    sheet_reset();
    face_reset();
    table_kinds_reset();
    solid_kind_reset();
    mesh_kind_reset();
    terrain_kind_reset();
    model_kind_reset();
    terrain_set_contours(false);
    scene_invalidate_hiz();
    lighting_reset();
    post_reset_exposure();
    physics_reset();
    spin_clear();
    camera_reset();
    if (world_selected_id() == 0u) {
        ground_set_visible(false);
        gimbal_set_visible(false);
        hud_set_visible(false);
        plot_set_visible(false);
        text_set_visible(false);
        world_set_clear_color(0.015f, 0.018f, 0.024f, 1.0f);
    } else {
        ground_set_visible(true);
        gimbal_set_visible(true);
        hud_set_visible(true);
        plot_set_visible(world_selected_id() == 9u);
        text_set_visible(true);
        world_set_clear_color(0.08f, 0.24f, 0.48f, 1.0f);
    }
    face_self_check();
    table_kinds_check();
}

void world_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    physics_record_compute(cmd);
    if (policy_mode() == MODE_2D) {
        return;
    }
    table_record(cmd, heat_dt, live);
}

void world_record_ok(uint64_t signal_point) {
    table_record_ok(signal_point);
}

void world_record_abort(void) {
    physics_abort_frame(0u);
    table_record_abort();
}

uint64_t world_record_published(void) {
    return table_record_published();
}

bool world_record_step_open(void) {
    return table_record_step_open();
}

bool world_record_poke_pending(void) {
    if (policy_mode() == MODE_2D) {
        return false;
    }
    return table_record_poke_pending();
}

static void world_harvest_gpu_physics(void) {
    PhysicsSolvedStep step;
    while (physics_poll_solved_step(&step)) {
        if (step.channel_id == PHYSICS_CHAN_3D_BODIES) {
            (void)world_apply_gpu_motion(&step);
        } else if (step.channel_id >= PHYSICS_CHAN_2D_SOFT_BASE) {
            uint32_t soft_idx = step.channel_id - PHYSICS_CHAN_2D_SOFT_BASE;
            (void)sheet_apply_gpu_soft_step(soft_idx, step.epoch,
                                            step.verts, step.vert_count, step.dt);
        }
    }
}

bool world_simulation(uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt) {
    world_harvest_gpu_physics();
    bool pulse = false;
    if (policy_mode() == MODE_2D) {
        if (heat_dt != nullptr) {
            *heat_dt = 0u;
        }
    } else {
        pulse = table_simulate(time_ms, sim_dt_ms, heat_dt);
    }
    if (policy_mode() != MODE_3D) {
        sheet_set_avbd_iterations(physics_recommended_iterations());
        sheet_set_gpu_pose_owner(physics_gpu_pose_owner());
        if (sheet_simulate(sim_dt_ms)) {
            pulse = true;
        }
        SheetGpuStep gstep;
        while (sheet_pop_gpu_step(&gstep)) {
            if (!physics_submit_step(PHYSICS_CHAN_2D_SOFT_BASE + gstep.soft_index,
                                     gstep.epoch,
                                     gstep.verts, gstep.vert_count,
                                     gstep.cons, gstep.con_count,
                                     gstep.dt, gstep.gravity, gstep.damp_keep)) {
                sheet_cancel_gpu_soft_step(gstep.soft_index, gstep.epoch);
            }
        }
    }
    if (physics_any_inflight()) {
        pulse = true;
    }
    return pulse;
}

void world_simulation_hold(void) {
    if (policy_mode() != MODE_2D) {
        table_simulate_hold();
    }
}

void world_simulation_commit(uint32_t time_ms, uint32_t heat_dt) {
    if (policy_mode() != MODE_2D) {
        table_simulate_commit(time_ms, heat_dt);
    }
}

static void world_store(void) {
    (void)world_save();
}

void world_adopt_mesh(void) {
    mesh_kind_set_wanted(true);
    mesh_kind_raise();
    world_store();
}

bool world_apply_mesh(bool has, bool bytes) {
    bool was     = table_find(mesh_ops()) != nullptr;
    bool changed = mesh_kind_apply(has, bytes);
    bool now     = table_find(mesh_ops()) != nullptr;
    if (was != now) {
        world_store();
    }
    return changed;
}

bool world_close_face(void) {
    return face_close();
}

bool world_cancel(void) {
    (void)world_interact_release();
    bool sheet    = policy_mode() != MODE_3D && sheet_cancel();
    bool corners  = face_chain_count() > 0u;
    bool selected = table_selected() != nullptr;
    face_cancel_chain();
    table_clear_selection();
    if (!corners && !selected && !sheet) {
        return false;
    }
    if (sheet && !corners && !selected) {
        printf("[Sheet] Selection cleared.\n");
        fflush(stdout);
        return true;
    }
    if (corners && selected) {
        printf("[Engine] Edit cancelled.\n");
    } else if (corners) {
        printf("[Engine] Corners cleared.\n");
    } else {
        printf("[Engine] Selection cleared.\n");
    }
    fflush(stdout);
    return true;
}

static bool delete_obj(WorldObj *o) {
    if (o == nullptr || o->ops == nullptr || o->ops->remove == nullptr) {
        return false;
    }
    WorldRemoved snap = {0};
    bool keep = snap_removed(o, &snap);
    if (!o->ops->remove(o)) {
        return false;
    }
    if (keep) {
        if (snap.kind == k_removed_mesh) {
            mesh_kind_abandon();
        }
        world_removed_push(&snap);
    }
    table_clear_selection();
    world_store();
    return true;
}

bool world_delete(void) {
    if (policy_mode() != MODE_3D && sheet_has_selection()) {
        return sheet_delete();
    }
    if (policy_mode() == MODE_2D) {
        WorldObj *sel = table_selected();
        if (sel != nullptr && sel->ops == face_ops() && delete_obj(sel)) {
            return true;
        }
        return face_pop_corner();
    }
    if (delete_obj(table_selected())) {
        return true;
    }
    if (face_pop_corner()) {
        return true;
    }
    return delete_obj(table_fallback());
}

bool world_restore(void) {
    if (s_removed_n == 0u) {
        printf("[Engine] Nothing removed to put back.\n");
        fflush(stdout);
        return false;
    }
    WorldRemoved g = s_removed[--s_removed_n];
    int  before = table_count();
    bool ok     = false;
    if (g.kind == k_removed_cube) {
        ok = cube_kind_spawn_elastic(g.at, g.yaw_milli, g.spinning, g.infinite, g.glow, g.restitution);
    } else if (g.kind == k_removed_solid) {
        ok = solid_kind_place_elastic((uint32_t)g.solid, g.at, g.yaw_milli, g.spinning, g.infinite, g.glow, g.restitution);
    } else if (g.kind == k_removed_mesh) {
        ok = mesh_kind_pose(g.yaw_milli, g.spinning, g.infinite, g.glow);
    }
    if (!ok || table_count() == before) {
        if (ok && g.kind == k_removed_mesh) {
            mesh_kind_abandon();
        }
        s_removed[s_removed_n++] = g;
        printf("[Engine] That object did not fit back.\n");
        fflush(stdout);
        return false;
    }
    table_select_quiet(table_count() - 1);
    const char *name = "Object";
    if (g.kind == k_removed_cube) {
        name = "Cube";
    } else if (g.kind == k_removed_solid) {
        const char *solid = solid_kind_name((uint32_t)g.solid);
        if (solid != nullptr) {
            name = solid;
        }
    } else if (g.kind == k_removed_mesh) {
        name = "Mesh";
    }
    printf("[Engine] %s back.\n", name);
    fflush(stdout);
    world_store();
    return true;
}

bool world_respin_selected(void) {
    WorldObj *o = table_selected();
    if (o == nullptr || o->ops == nullptr || o->ops->respin == nullptr) {
        return false;
    }
    bool changed = o->ops->respin(o);
    if (changed) {
        world_store();
    }
    return changed;
}

bool world_raise_solid(uint32_t kind) {
    bool changed = solid_kind_raise(kind);
    if (changed) {
        world_store();
    }
    return changed;
}

static bool invoke_selected_or_fallback(bool (*fn)(WorldObj *)) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && fn != nullptr) {
        bool changed = fn(o);
        if (changed) {
            world_store();
        }
        return changed;
    }
    return false;
}

bool world_toggle_spin(void) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && o->ops->toggle_spin != nullptr) {
        return invoke_selected_or_fallback(o->ops->toggle_spin);
    }
    o = table_fallback();
    if (o == nullptr || o->ops == nullptr || o->ops->toggle_spin == nullptr) {
        return false;
    }
    bool changed = o->ops->toggle_spin(o);
    if (changed) {
        world_store();
    }
    return changed;
}

bool world_toggle_glow(void) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && o->ops->toggle_glow != nullptr) {
        return invoke_selected_or_fallback(o->ops->toggle_glow);
    }
    o = table_fallback();
    if (o == nullptr || o->ops == nullptr || o->ops->toggle_glow == nullptr) {
        return false;
    }
    bool changed = o->ops->toggle_glow(o);
    if (changed) {
        world_store();
    }
    return changed;
}

bool world_toggle_smooth(void) {
    bool changed = solid_kind_toggle_smooth();
    world_store();
    return changed;
}

bool world_busy(void) {
    bool busy = camera_busy() || text_busy() || physics_any_inflight() || world_interact_busy();
    if (policy_mode() == MODE_2D) {
        busy = busy || plot_wants_frame() || sheet_busy() || sheet_has_pending_gpu_steps() ||
               face_chain_busy() || table_busy_ops(face_ops());
    } else {
        busy = busy || overlay_busy() || face_chain_busy() || solid_kind_smooth_dirty() || table_busy();
        if (policy_mode() == MODE_HYBRID) {
            busy = busy || sheet_busy() || sheet_has_pending_gpu_steps();
        }
    }
    return busy;
}

bool world_advance(uint32_t time_ms) {
    world_harvest_gpu_physics();
    bool changed = text_busy() || physics_has_unrecorded_steps();
    if (policy_mode() == MODE_2D) {
        if (plot_wants_frame() || face_chain_busy() || table_advance_ops(time_ms, face_ops())) {
            changed = true;
        }
    } else if (table_advance(time_ms) || face_chain_busy() || solid_kind_smooth_dirty() || overlay_busy()) {
        changed = true;
    }
    if (policy_mode() != MODE_3D && sheet_take_changed()) {
        changed = true;
    }
    if (camera_advance(time_ms)) {
        changed = true;
    }
    return changed;
}

void world_commit(void) {
    table_commit();
    face_reap();
    face_commit_chain();
    solid_kind_commit_smooth();
    overlay_commit();
    camera_commit();
    text_commit();
}

bool world_consume_spin(void) {
    if (policy_mode() == MODE_2D && (table_busy() || table_record_poke_pending())) {
        return false;
    }
    if (world_busy() || !table_spun_pending()) {
        return false;
    }
    table_clear_spun();
    return true;
}

typedef struct WorldPrepCtx {
    uint32_t width;
    uint32_t height;
    bool     live;
} WorldPrepCtx;

static WorldPrepCtx s_prep_ctx = {0};

static void exec_world_scene_cull(VkCommandBuffer cmd, void *user) {
    const WorldPrepCtx *ctx = (const WorldPrepCtx *)user;
    scene_prepare_draw(cmd, ctx->width, ctx->height);
}

static void exec_world_shadow_map(VkCommandBuffer cmd, void *user) {
    const WorldPrepCtx *ctx = (const WorldPrepCtx *)user;
    float light_clip_vp[16];
    if (lighting_shadow_begin(cmd, light_clip_vp)) {
        table_draw_shadow(cmd, light_clip_vp, ctx->live);
        scene_draw_shadow(cmd, light_clip_vp);
        lighting_shadow_end(cmd);
    }
}

void world_prepare_draw(RgGraph *g, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    if (g == nullptr || world_selected_id() == 0u) {
        return;
    }
    if (policy_mode() == MODE_2D) {
        lighting_prepare(g, 0u, 0u);
        rg_flush(g, cmd);
        return;
    }
    camera_bind(live);
    ViewPush vp = {0};
    view_push_fill(&vp, 0u);
    LightingCameraParams lcam = {
        .qx    = vp.qx,
        .qy    = vp.qy,
        .qz    = vp.qz,
        .qw    = vp.qw,
        .cam_x = vp.cam_x,
        .cam_y = vp.cam_y,
        .cam_z = vp.cam_z,
        .zoom  = vp.zoom,
        .persp = vp.persp,
    };
    lighting_begin_frame(width, height, &lcam);
    if (terrain_is_active()) {
        uint64_t dir_bda = backend_directory_bda();
        if (dir_bda != 0ull) {
            lighting_set_terrain_field(dir_bda, backend_elev_base_m(), backend_vertical_scale());
        }
    } else if (ground_is_visible() && coord_ground_set()) {
        const float ground_alb[3] = { 0.145f, 0.162f, 0.195f };
        lighting_set_ground_plane(true, 0.0f, ground_alb);
    }

    view_push_fill(&vp, 0u);
    SceneViewParams sv = {
        .materials_bda = vp.materials,
        .mat_index     = vp.mat_index,
        .persp         = vp.persp,
        .qx            = vp.qx,
        .qy            = vp.qy,
        .qz            = vp.qz,
        .qw            = vp.qw,
        .cam_x         = vp.cam_x,
        .cam_y         = vp.cam_y,
        .cam_z         = vp.cam_z,
        .zoom          = vp.zoom,
    };
    scene_begin_frame(width, height, &sv);
    table_sync_ecs(live);
    table_prepare_scene(live);

    s_prep_ctx = (WorldPrepCtx){ .width = width, .height = height, .live = live };
    rg_add_pass(g, "SceneCull", RG_PASS_COMPUTE, nullptr, 0u, exec_world_scene_cull, &s_prep_ctx);
    rg_add_pass(g, "ShadowMap", RG_PASS_SHADOW,  nullptr, 0u, exec_world_shadow_map, &s_prep_ctx);
    rg_flush(g, cmd);

    lighting_prepare(g, width, height);
    rg_flush(g, cmd);
}

void world_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    camera_bind(live);
    table_sync_ecs(live);
    ground_draw(cmd, width, height);
    if (policy_mode() != MODE_2D) {
        table_draw(cmd, width, height, live);
        scene_draw(cmd, width, height);
        face_draw_chain(cmd, width, height, live);
    } else {
        table_draw_ops(cmd, width, height, live, face_ops());
        face_draw_chain(cmd, width, height, live);
    }
    if (policy_mode() != MODE_3D) {
        sheet_draw(cmd, width, height);
    }
    overlay_draw(cmd, width, height);
}

void world_stats(uint32_t *objects, uint32_t *vertices) {
    if (objects != nullptr) {
        *objects = table_alive_count();
    }
    if (vertices != nullptr) {
        *vertices = table_alive_vertices();
    }
}
