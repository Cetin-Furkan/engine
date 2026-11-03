#include "world/world_int.h"
#include "frame/camera.h"
#include "frame/policy.h"
#include "frame/text.h"
#include <stdio.h>
#include <string.h>

// Starts the world and calls the layers under src/world. Object kinds live
// in kind/. Faces live in face/. The file reader is load.c. This file does
// not contain a cube, a face list, or a key's geometry.

enum : uint32_t { k_removed_cap = 24 };

static WorldRemoved s_removed[k_removed_cap];
static uint32_t     s_removed_n = 0;

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
    } else if (o->ops == solid_ops()) {
        const char *name = o->ops->name != nullptr ? o->ops->name(o) : nullptr;
        int kind = solid_kind_index(name);
        if (kind < 0) {
            return false;
        }
        g.kind = k_removed_solid;
        g.solid = kind;
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
    face_reset();
    table_kinds_reset();
    solid_kind_reset();
    mesh_kind_reset();
    spin_clear();
    camera_reset();
    face_self_check();
    table_kinds_check();
}

void world_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
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
    return table_record_poke_pending();
}

bool world_simulation(uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt) {
    return table_simulate(time_ms, sim_dt_ms, heat_dt);
}

void world_simulation_hold(void) {
    table_simulate_hold();
}

void world_simulation_commit(uint32_t time_ms, uint32_t heat_dt) {
    table_simulate_commit(time_ms, heat_dt);
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
    CameraRay ray;
    camera_click_ray(screen_x, screen_y, &ray);
    Coord hit = {0};
    int index = table_pick(&ray, &hit, nullptr);
    if (index >= 0) {
        // A rejected select must not fall through into a face spawn.
        if (!policy_allows(PERM_OBJ_SELECT)) {
            return false;
        }
        bool changed = table_select(index);
        WorldObj *o = table_at(index);
        if (o != nullptr && o->ops != nullptr && o->ops->on_click != nullptr &&
            o->ops->on_click(o, hit)) {
            changed = true;
        }
        return changed;
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
    bool corners = face_chain_count() > 0u;
    bool selected = table_selected() != nullptr;
    face_cancel_chain();
    table_clear_selection();
    if (!corners && !selected) {
        return false;
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
        ok = cube_kind_spawn_pose(g.at, g.yaw_milli, g.spinning, g.infinite, g.glow);
    } else if (g.kind == k_removed_solid) {
        ok = solid_kind_place((uint32_t)g.solid, g.at, g.yaw_milli, g.spinning, g.infinite, g.glow);
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
    return camera_busy() || overlay_busy() || face_chain_busy() ||
           solid_kind_smooth_dirty() || table_busy() || text_busy();
}

bool world_advance(uint32_t time_ms) {
    bool changed = table_advance(time_ms) || face_chain_busy() || solid_kind_smooth_dirty() ||
                   overlay_busy() || text_busy();
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
    if (world_busy() || !table_spun_pending()) {
        return false;
    }
    table_clear_spun();
    return true;
}

void world_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    camera_bind(live);
    ground_draw(cmd, width, height);
    table_draw(cmd, width, height, live);
    face_draw_chain(cmd, width, height, live);
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
