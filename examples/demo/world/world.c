#include "world/world_int.h"
#include "frame/camera.h"
#include "frame/gimbal.h"
#include "frame/hud.h"
#include "frame/plot.h"
#include "frame/policy.h"
#include "frame/sheet.h"
#include "frame/terrain.h"
#include "frame/text.h"
#include <stdio.h>
#include <string.h>

// Starts the world and calls the layers under src/world. Object kinds live
// in kind/. Faces live in face/. The file reader is load.c. This file does
// not contain a cube, a face list, or a key's geometry.

enum : uint32_t { k_removed_cap = 24 };

static WorldRemoved s_removed[k_removed_cap];
static uint32_t     s_removed_n = 0;
static uint32_t     s_world_id  = 0;
static char         s_custom_file[1024];
static float        s_clear_rgba[4] = { 0.015f, 0.018f, 0.024f, 1.0f };

void world_set_clear_color(float r, float g, float b, float a) {
    s_clear_rgba[0] = r;
    s_clear_rgba[1] = g;
    s_clear_rgba[2] = b;
    s_clear_rgba[3] = a;
}

void world_clear_color(float out_rgba[4]) {
    if (out_rgba == nullptr) {
        return;
    }
    out_rgba[0] = s_clear_rgba[0];
    out_rgba[1] = s_clear_rgba[1];
    out_rgba[2] = s_clear_rgba[2];
    out_rgba[3] = s_clear_rgba[3];
}

void world_select(uint32_t world_id) {
    if (world_id > k_world_preset_max) {
        world_id = 0u;
    }
    s_world_id = world_id;
    s_custom_file[0] = '\0';
    sheet_select_world(s_world_id);
}

bool world_select_file(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    int wrote = snprintf(s_custom_file, sizeof s_custom_file, "%s", path);
    if (wrote <= 0 || (size_t)wrote >= sizeof s_custom_file) {
        s_custom_file[0] = '\0';
        return false;
    }
    s_world_id = k_world_custom_file;
    sheet_select_world(9u);
    return true;
}

const char *world_custom_path(void) {
    return s_custom_file[0] != '\0' ? s_custom_file : nullptr;
}

uint32_t world_selected_id(void) {
    return s_world_id;
}

const char *world_selected_title(void) {
    switch (s_world_id) {
    case 0u:
        return "Pure Empty Engine (Zero Objects, Zero Overlays)";
    case 1u:
        return "3D-Only Click-to-Draw Studio (Points -> Lines -> Faces)";
    case 2u:
        return "3D-Only Gravity & Elastic Non-Cube Solids (Squash-and-Stretch)";
    case 3u:
        return "2D-Only Gravity, Deformable Soft Bodies & Elastic Polygons";
    case 4u:
        return "2D-Only Click-to-Draw Polygon Workshop & Elastic Sheet";
    case 5u:
        return "Hybrid 3D+2D Click-to-Draw, Gravity & Multi-Elasticity";
    case 6u:
        return "3D+2D Switchable Restitution Staircase & Bumper Lab";
    case 7u:
        return "Procedural C23 WorldBuilder: Kinetic Cathedral & Cascade";
    case 8u:
        return "Baklan & Beşparmak Dağı SSD-Streamed DEM + Satellite Map (Denizli, TR)";
    case k_world_custom_file:
        return s_custom_file[0] != '\0' ? s_custom_file : "Custom .world Document";
    case 9u:
    default:
        return "Interactive Full Sandbox (Switchable 3D / 2D / Hybrid + Plots)";
    }
}

void world_apply_policy(void) {
    switch (s_world_id) {
    case 0u:
        // World 0: Pure Empty Engine canvas. Revoke all permissions and seal.
        policy_apply_world(MODE_3D, PERM_NONE, 0xFFFFu, true);
        return;
    case 1u:
        // World 1: 3D-only, click-to-draw (PERM_DRAW) enabled at boot, mode switching locked.
        policy_apply_world(MODE_3D, PERM_DRAW, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        break;
    case 2u:
        // World 2: 3D-only gravity & elastic non-cube solids, mode switching locked.
        policy_apply_world(MODE_3D, PERM_NONE, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        break;
    case 3u:
        // World 3: 2D-only gravity & deformable soft patches, mode switching locked.
        policy_apply_world(MODE_2D, PERM_NONE, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        (void)camera_snap_front();
        camera_commit();
        break;
    case 4u:
        // World 4: 2D-only with click-to-draw (PERM_DRAW) enabled at boot, mode switching locked.
        policy_apply_world(MODE_2D, PERM_DRAW, PERM_MODE_SWITCH | PERM_PLOT_CYCLE, false);
        (void)camera_snap_front();
        camera_commit();
        break;
    case 5u:
        // World 5: Hybrid 3D+2D with click-to-draw enabled at boot, Tab mode switch allowed.
        policy_apply_world(MODE_HYBRID, PERM_DRAW, PERM_PLOT_CYCLE, false);
        break;
    case 6u:
        // World 6: 3D+2D switchable restitution staircase & bumper lab, Tab mode switch allowed.
        policy_apply_world(MODE_3D, PERM_NONE, PERM_PLOT_CYCLE, false);
        break;
    case 7u:
        // World 7: Procedural C23 WorldBuilder (Hybrid 3D+2D + click-to-draw + Tab mode switch).
        policy_apply_world(MODE_HYBRID, PERM_DRAW, PERM_PLOT_CYCLE, false);
        break;
    case 8u:
        // World 8: Baklan & Beşparmak Dağı SSD-Streamed DEM + Satellite Map (Pure 3D Map, Z-up turntable mouse controls).
        policy_apply_world(MODE_3D, PERM_NONE, PERM_PLOT_CYCLE | PERM_OBJ_SPAWN | PERM_MODE_SWITCH, false);
        camera_set_turntable_mode(true);
        camera_set_home_pose(1595.0f, -650.0f, 520.0f, -0.18f, 0.52f, 0.16f);
        printf("[Bird's-Eye Overlay] Active at 50%% transparency | Keys: 'O'=+Transparency, 'P'=-Transparency, 'W/A/S/D'=Move parallel to map, 'Shift+WASD' (or I/U, J/N, ]/[)=0.5%% 2D Stretch, 'B'=Top-Down Bird's-Eye View, 'Enter'=Upload & Bake onto Map\n");
        break;
    case k_world_custom_file:
        policy_boot();
        return;
    case 9u:
    default:
        policy_boot();
        break;
    }
    const WorkspacePolicy *pol = policy_active();
    printf("[World] Active #%u: %s | Mode=%s (Tab switch=%s, click-draw=%s)\n",
           s_world_id, world_selected_title(), pol->name,
           policy_allows(PERM_MODE_SWITCH) ? "on" : "locked",
           policy_allows(PERM_DRAW) ? "on" : "off");
    fflush(stdout);
}

bool world_switch_to(uint32_t world_id) {
    if (world_id > k_world_preset_max) {
        return false;
    }
    (void)world_save();
    (void)sheet_save();
    world_select(world_id);
    world_reset();
    world_apply_policy();
    if (world_load() != 0) {
        return false;
    }
    printf("[World] Runtime hot-switched to World %u: %s\n",
           s_world_id, world_selected_title());
    fflush(stdout);
    return true;
}

bool world_reload(void) {
    if (s_world_id == 0u) {
        return false;
    }
    if (s_world_id == 8u) {
        bool ok = terrain_reload();
        printf("[World] Hot-reloaded World 8 (%s) terrain & survey dataset\n",
               world_selected_title());
        fflush(stdout);
        return ok;
    }
    world_reset();
    world_apply_policy();
    if (world_load() != 0) {
        return false;
    }
    const char *src = world_source_path();
    printf("[World] Hot-reloaded World %u (%s) from %s\n",
           s_world_id, world_selected_title(),
           src != nullptr ? src : "builtin");
    fflush(stdout);
    return true;
}

void world_removed_clear(void) {
    s_removed_n = 0;
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
    WorldRemoved g = {0};
    g.at = o->at;
    if (o->spin != nullptr) {
        g.yaw_milli = o->spin->live_milli;
        g.spinning = o->spin->spinning;
        g.infinite = o->spin->infinite;
        g.glow = o->spin->glow;
    }
    if (o->ops == cube_ops()) {
        g.kind = k_removed_cube;
        g.restitution = cube_kind_restitution(o);
    } else if (o->ops == solid_ops()) {
        const char *name = o->ops->name != nullptr ? o->ops->name(o) : nullptr;
        int kind = solid_kind_index(name);
        if (kind < 0) {
            return false;
        }
        g.kind = k_removed_solid;
        g.solid = kind;
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
    world_removed_clear();
    table_reset();
    ground_reset();
    sheet_reset();
    face_reset();
    table_kinds_reset();
    solid_kind_reset();
    mesh_kind_reset();
    terrain_kind_reset();
    terrain_set_contours(false);
    spin_clear();
    camera_reset();
    if (s_world_id == 0u) {
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
        plot_set_visible(s_world_id == 9u);
        text_set_visible(true);
        world_set_clear_color(0.08f, 0.24f, 0.48f, 1.0f);
    }
    face_self_check();
    table_kinds_check();
}

void world_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    // 2D keeps the fillet heat and the 128³ lattice off the queue.
    if (policy_mode() == MODE_2D) {
        return;
    }
    table_record(cmd, heat_dt, live);
}

void world_record_ok(uint64_t signal_point) {
    table_record_ok(signal_point);
}

void world_record_abort(void) {
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

bool world_simulation(uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt) {
    bool pulse = false;
    if (policy_mode() == MODE_2D) {
        if (heat_dt != nullptr) {
            *heat_dt = 0;
        }
    } else {
        pulse = table_simulate(time_ms, sim_dt_ms, heat_dt);
    }
    if (policy_mode() != MODE_3D && sheet_simulate(sim_dt_ms)) {
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
    bool was = table_find(mesh_ops()) != nullptr;
    bool changed = mesh_kind_apply(has, bytes);
    bool now = table_find(mesh_ops()) != nullptr;
    if (was != now) {
        world_store();
    }
    return changed;
}

bool world_click(float screen_x, float screen_y) {
    WorkspaceMode mode = policy_mode();
    CameraRay ray;
    camera_click_ray(screen_x, screen_y, &ray);
    if (mode != MODE_3D && policy_allows(PERM_OBJ_SELECT)) {
        Coord ground = {0};
        if (coord_ground_hit(&ray, &ground, nullptr) && sheet_click(ground.x, ground.y)) {
            table_clear_selection();
            return true;
        }
    }
    if (mode != MODE_2D) {
        Coord hit = {0};
        int index = table_pick(&ray, &hit, nullptr);
        if (index >= 0) {
            // A rejected select must not fall through into a face spawn.
            if (!policy_allows(PERM_OBJ_SELECT)) {
                return false;
            }
            sheet_clear();
            bool changed = table_select(index);
            WorldObj *o = table_at(index);
            if (o != nullptr && o->ops != nullptr && o->ops->on_click != nullptr &&
                o->ops->on_click(o, hit)) {
                changed = true;
            }
            return changed;
        }
    } else if (face_chain_count() == 0u) {
        Coord hit = {0};
        int index = table_pick(&ray, &hit, nullptr);
        if (index >= 0) {
            WorldObj *o = table_at(index);
            if (o != nullptr && o->ops == face_ops()) {
                if (!policy_allows(PERM_OBJ_SELECT)) {
                    return false;
                }
                sheet_clear();
                bool changed = table_select(index);
                if (o->ops->on_click != nullptr && o->ops->on_click(o, hit)) {
                    changed = true;
                }
                return changed;
            }
        }
    }
    Coord ground = {0};
    if (!coord_ground_hit(&ray, &ground, nullptr)) {
        return false;
    }
    if (!policy_allows(PERM_DRAW)) {
        return false;
    }
    return face_spawn(ground);
}

bool world_close_face(void) {
    return face_close();
}

bool world_cancel(void) {
    bool sheet = policy_mode() != MODE_3D && sheet_cancel();
    bool corners = face_chain_count() > 0u;
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
    // Nothing selected and no open corner: the first placed cube is the
    // fallback, the same object R and G reach when the selection is empty.
    return delete_obj(table_fallback());
}

bool world_restore(void) {
    if (s_removed_n == 0u) {
        printf("[Engine] Nothing removed to put back.\n");
        fflush(stdout);
        return false;
    }
    WorldRemoved g = s_removed[--s_removed_n];
    int before = table_count();
    bool ok = false;
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

bool world_toggle_spin(void) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && o->ops->toggle_spin != nullptr) {
        bool changed = o->ops->toggle_spin(o);
        if (changed) {
            world_store();
        }
        return changed;
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
        bool changed = o->ops->toggle_glow(o);
        if (changed) {
            world_store();
        }
        return changed;
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
    bool busy = camera_busy() || text_busy();
    if (policy_mode() == MODE_2D) {
        busy = busy || plot_wants_frame() || sheet_busy() || face_chain_busy() || table_busy_ops(face_ops());
    } else {
        busy = busy || overlay_busy() || face_chain_busy() || solid_kind_smooth_dirty() || table_busy();
        if (policy_mode() == MODE_HYBRID) {
            busy = busy || sheet_busy();
        }
    }
    return busy;
}

bool world_advance(uint32_t time_ms) {
    bool changed = text_busy();
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
    // 2D freezes the 3D turn. Do not call it finished, and do not hold the lattice.
    if (policy_mode() == MODE_2D && (table_busy() || table_record_poke_pending())) {
        return false;
    }
    if (world_busy() || !table_spun_pending()) {
        return false;
    }
    table_clear_spun();
    return true;
}

void world_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    camera_bind(live);
    ground_draw(cmd, width, height);
    if (policy_mode() != MODE_2D) {
        table_draw(cmd, width, height, live);
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
