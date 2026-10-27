#include "world/world_int.h"
#include "frame/camera.h"
#include <stdio.h>

// Starts the world and calls the layers under src/world. Object kinds live
// in kind/. Faces live in face/. The file reader is load.c. This file does
// not contain a cube, a face list, or a key's geometry.

void world_reset(void) {
    table_reset();
    ground_reset();
    face_reset();
    cube_kind_reset();
    solid_kind_reset();
    mesh_kind_reset();
    spin_clear();
    camera_reset();
    face_self_check();
    cube_kind_self_check();
}

void world_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    cube_kind_record(cmd, heat_dt, live);
}

void world_record_ok(uint64_t signal_point) {
    cube_kind_record_ok(signal_point);
}

void world_record_abort(void) {
    cube_kind_record_abort();
}

uint64_t world_record_published(void) {
    return cube_kind_published();
}

bool world_record_step_open(void) {
    return cube_kind_step_open();
}

bool world_record_poke_pending(void) {
    return cube_kind_poke_pending();
}

bool world_simulation(uint32_t time_ms, uint32_t *heat_dt) {
    return cube_kind_simulation(time_ms, heat_dt);
}

void world_simulation_hold(void) {
    cube_kind_hold();
}

void world_simulation_commit(uint32_t time_ms, uint32_t heat_dt) {
    cube_kind_commit_stamp(time_ms, heat_dt);
}

void world_adopt_mesh(void) {
    mesh_kind_set_wanted(true);
    mesh_kind_raise();
}

bool world_apply_mesh(bool has, bool bytes) {
    return mesh_kind_apply(has, bytes);
}

bool world_click(float screen_x, float screen_y) {
    CameraRay ray;
    camera_click_ray(screen_x, screen_y, &ray);
    Coord hit = {0};
    int index = table_pick(&ray, &hit, nullptr);
    if (index >= 0) {
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

bool world_delete(void) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && o->ops->remove != nullptr && o->ops->remove(o)) {
        table_clear_selection();
        return true;
    }
    return face_pop_corner();
}

bool world_respin_selected(void) {
    WorldObj *o = table_selected();
    if (o == nullptr || o->ops == nullptr || o->ops->respin == nullptr) {
        return false;
    }
    return o->ops->respin(o);
}

bool world_raise_solid(uint32_t kind) {
    return solid_kind_raise(kind);
}

bool world_toggle_spin(void) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && o->ops->toggle_spin != nullptr) {
        return o->ops->toggle_spin(o);
    }
    return cube_kind_toggle_fallback();
}

bool world_toggle_glow(void) {
    WorldObj *o = table_selected();
    if (o != nullptr && o->ops != nullptr && o->ops->toggle_glow != nullptr) {
        return o->ops->toggle_glow(o);
    }
    return cube_kind_glow_fallback();
}

bool world_toggle_smooth(void) {
    return solid_kind_toggle_smooth();
}

bool world_busy(void) {
    return camera_busy() || overlay_busy() || face_chain_busy() ||
           solid_kind_smooth_dirty() || table_busy();
}

bool world_advance(uint32_t time_ms) {
    bool changed = table_advance(time_ms) || face_chain_busy() || solid_kind_smooth_dirty() ||
                   overlay_busy();
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
}

bool world_consume_spin(void) {
    if (world_busy() || !table_spun_pending()) {
        return false;
    }
    table_clear_spun();
    return true;
}

float world_cube_yaw_deg(bool live) {
    return cube_kind_yaw(live);
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
