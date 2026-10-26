#include "world/world_int.h"
#include "frame/material.h"
#include "frame/mesh.h"
#include <stdio.h>

typedef struct MeshData {
    Spin spin;
    bool up;
} MeshData;

static MeshData s_mesh;
static bool     s_wanted = false;
static const WorldOps s_ops;

static bool mesh_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    (void)o;
    float cx = 0.0f;
    float cy = 0.0f;
    float radius = 0.0f;
    if (!mesh_sphere(&cx, &cy, &radius)) {
        return false;
    }
    Coord center = { cx, cy, 0.0f };
    if (!coord_sphere_hit(ray, center, radius, t)) {
        return false;
    }
    if (at != nullptr && t != nullptr) {
        *at = (Coord){
            ray->ox + ray->dx * *t,
            ray->oy + ray->dy * *t,
            ray->oz + ray->dz * *t,
        };
    }
    return true;
}

static bool mesh_advance(WorldObj *o, uint32_t time_ms) {
    MeshData *m = o->data;
    bool changed = spin_advance(&m->spin, time_ms, false);
    if (mesh_wants_frame()) {
        changed = true;
    }
    return changed;
}

static void mesh_commit_obj(WorldObj *o) {
    (void)o;
    mesh_commit();
}

static bool mesh_busy(const WorldObj *o) {
    (void)o;
    return mesh_wants_frame();
}

static void mesh_draw_obj(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const MeshData *m = o->data;
    bool glow = live ? m->spin.glow : m->spin.shown_glow;
    material_set(k_mat_mesh, glow);
    mesh_draw(cmd, width, height, spin_deg(&m->spin, live), table_highlight(o, live));
}

static bool mesh_toggle_spin(WorldObj *o) {
    MeshData *m = o->data;
    if (spin_toggle(&m->spin)) {
        printf("[Engine] Mesh spins.\n");
    } else {
        printf("[Engine] Mesh holds.\n");
    }
    fflush(stdout);
    return true;
}

static bool mesh_toggle_glow(WorldObj *o) {
    MeshData *m = o->data;
    m->spin.glow = !m->spin.glow;
    printf("[Material] Mesh glow %s.\n", m->spin.glow ? "on" : "off");
    fflush(stdout);
    return true;
}

static bool mesh_respin(WorldObj *o) {
    MeshData *m = o->data;
    spin_respin(&m->spin);
    table_mark_spun();
    printf("[Engine] Mesh turns again.\n");
    fflush(stdout);
    return true;
}

static bool mesh_remove(WorldObj *o) {
    MeshData *m = o->data;
    o->alive = false;
    m->up = false;
    m->spin.spinning = false;
    m->spin.infinite = false;
    material_set(k_mat_mesh, false);
    printf("[Engine] Mesh removed.\n");
    fflush(stdout);
    return true;
}

static const char *mesh_name(const WorldObj *o) {
    (void)o;
    return "Mesh";
}

static const WorldOps s_ops = {
    .hit         = mesh_hit,
    .draw        = mesh_draw_obj,
    .advance     = mesh_advance,
    .commit      = mesh_commit_obj,
    .busy        = mesh_busy,
    .toggle_spin = mesh_toggle_spin,
    .toggle_glow = mesh_toggle_glow,
    .respin      = mesh_respin,
    .remove      = mesh_remove,
    .name        = mesh_name,
};

void mesh_kind_reset(void) {
    s_mesh = (MeshData){0};
    s_wanted = false;
    material_set(k_mat_mesh, false);
}

void mesh_kind_set_wanted(bool wanted) {
    s_wanted = wanted;
}

bool mesh_kind_wanted(void) {
    return s_wanted;
}

bool mesh_kind_raise(void) {
    if (s_mesh.up || !mesh_has_triangles()) {
        return false;
    }
    float cx = 0.0f;
    float cy = 0.0f;
    float radius = 0.0f;
    if (!mesh_sphere(&cx, &cy, &radius)) {
        return false;
    }
    s_mesh = (MeshData){0};
    s_mesh.up = true;
    s_mesh.spin = spin_start(0);
    Coord at = { cx, cy, 0.0f };
    int index = table_add(&s_ops, &s_mesh, at, &s_mesh.spin);
    if (index < 0) {
        s_mesh.up = false;
        fprintf(stderr, "[World] No room for the mesh.\n");
        return false;
    }
    table_select_quiet(index);
    table_mark_spun();
    printf("[Engine] Mesh up. One revolution, then it holds.\n");
    fflush(stdout);
    return true;
}

bool mesh_kind_apply(bool has, bool bytes) {
    if (!s_wanted) {
        return bytes;
    }
    WorldObj *o = table_find(&s_ops);
    if (has && o == nullptr) {
        return mesh_kind_raise();
    }
    if (!has && o != nullptr) {
        mesh_remove(o);
        table_clear_selection();
        return true;
    }
    return bytes;
}

uint32_t mesh_kind_stat_vertices(void) {
    if (table_find(&s_ops) == nullptr) {
        return 0u;
    }
    return mesh_vertex_count();
}
