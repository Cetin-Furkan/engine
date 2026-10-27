#include "world/world_int.h"
#include "frame/material.h"
#include "frame/solid.h"
#include <stdio.h>

enum { k_kinds = 6 };

static const char *k_name[k_kinds] = {
    "Tetrahedron", "Octahedron", "Icosahedron", "Cylinder", "Cone", "Torus",
};
static const Coord k_at[k_kinds] = {
    { -500.0f, 150.0f, 0.0f },
    { -500.0f, -180.0f, 0.0f },
    { 500.0f, 150.0f, 0.0f },
    { 500.0f, -180.0f, 0.0f },
    { 0.0f, 230.0f, 0.0f },
    { 0.0f, -250.0f, 0.0f },
};
static const float k_radius = 84.0f;

typedef struct SolidData {
    bool     used;
    uint32_t kind;
    Spin     spin;
} SolidData;

static SolidData s_solid[k_kinds];
static bool      s_curved = false;
static bool      s_shown_curved = false;
static const WorldOps s_ops;

static int find_kind(uint32_t kind) {
    int n = table_count();
    for (int i = 0; i < n; ++i) {
        WorldObj *o = table_at(i);
        if (o == nullptr || !o->alive || o->ops != &s_ops || o->data == nullptr) {
            continue;
        }
        const SolidData *s = o->data;
        if (s->kind == kind) {
            return i;
        }
    }
    return -1;
}

static bool solid_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    if (!coord_sphere_hit(ray, o->at, k_radius, t)) {
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

static bool solid_advance(WorldObj *o, uint32_t time_ms) {
    SolidData *s = o->data;
    return spin_advance(&s->spin, time_ms, false);
}

static void solid_draw_obj(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const SolidData *s = o->data;
    bool glow = live ? s->spin.glow : s->spin.shown_glow;
    material_set(k_mat_solid0 + s->kind, glow);
    bool curved = live ? s_curved : s_shown_curved;
    solid_draw(cmd, width, height, s->kind, curved, spin_deg(&s->spin, live),
               o->at.x, o->at.y, table_highlight(o, live));
}

static bool solid_toggle_spin(WorldObj *o) {
    SolidData *s = o->data;
    if (spin_toggle(&s->spin)) {
        printf("[Engine] %s spins.\n", k_name[s->kind]);
    } else {
        printf("[Engine] %s holds.\n", k_name[s->kind]);
    }
    fflush(stdout);
    return true;
}

static bool solid_toggle_glow(WorldObj *o) {
    SolidData *s = o->data;
    s->spin.glow = !s->spin.glow;
    printf("[Material] %s glow %s.\n", k_name[s->kind], s->spin.glow ? "on" : "off");
    fflush(stdout);
    return true;
}

static bool solid_respin(WorldObj *o) {
    SolidData *s = o->data;
    spin_respin(&s->spin);
    table_mark_spun();
    printf("[Engine] %s turns again.\n", k_name[s->kind]);
    fflush(stdout);
    return true;
}

static bool solid_remove(WorldObj *o) {
    SolidData *s = o->data;
    o->alive = false;
    s->used = false;
    s->spin.spinning = false;
    s->spin.infinite = false;
    material_set(k_mat_solid0 + s->kind, false);
    printf("[Engine] %s removed.\n", k_name[s->kind]);
    fflush(stdout);
    return true;
}

static const char *solid_name(const WorldObj *o) {
    const SolidData *s = o->data;
    return k_name[s->kind];
}

static uint32_t solid_vertices(const WorldObj *o) {
    const SolidData *sd = o->data;
    return solid_vertex_count_for(sd->kind, s_shown_curved);
}

static const WorldOps s_ops = {
    .hit         = solid_hit,
    .vertices    = solid_vertices,
    .draw        = solid_draw_obj,
    .advance     = solid_advance,
    .toggle_spin = solid_toggle_spin,
    .toggle_glow = solid_toggle_glow,
    .respin      = solid_respin,
    .remove      = solid_remove,
    .name        = solid_name,
};

void solid_kind_reset(void) {
    for (int i = 0; i < k_kinds; ++i) {
        s_solid[i] = (SolidData){0};
        material_set(k_mat_solid0 + (uint32_t)i, false);
    }
    s_curved = false;
    s_shown_curved = false;
}

bool solid_kind_raise(uint32_t kind) {
    if (kind >= k_kinds) {
        return false;
    }
    int existing = find_kind(kind);
    if (existing >= 0) {
        if (table_selected() == table_at(existing)) {
            return false;
        }
        return table_select(existing);
    }
    SolidData *s = &s_solid[kind];
    *s = (SolidData){0};
    s->used = true;
    s->kind = kind;
    s->spin = spin_start(0);
    int index = table_add(&s_ops, s, k_at[kind], &s->spin);
    if (index < 0) {
        s->used = false;
        fprintf(stderr, "[World] No room for %s.\n", k_name[kind]);
        return false;
    }
    table_select_quiet(index);
    printf("[Engine] %s up. One revolution, then it holds.\n", k_name[kind]);
    fflush(stdout);
    return true;
}

bool solid_kind_toggle_smooth(void) {
    s_curved = !s_curved;
    if (s_curved) {
        printf("[Engine] Edges smooth.\n");
    } else {
        printf("[Engine] Edges faceted.\n");
    }
    fflush(stdout);
    bool alive = false;
    for (uint32_t i = 0; i < k_kinds; ++i) {
        if (find_kind(i) >= 0) {
            alive = true;
            break;
        }
    }
    if (!alive) {
        s_shown_curved = s_curved;
        return false;
    }
    return true;
}

bool solid_kind_smooth_dirty(void) {
    return s_curved != s_shown_curved;
}

void solid_kind_commit_smooth(void) {
    s_shown_curved = s_curved;
}

uint32_t solid_kind_stat_vertices(void) {
    // Only kinds actually raised count; the baked buffer size does not.
    uint32_t total = 0;
    for (int i = 0; i < k_kinds; ++i) {
        if (find_kind(i) >= 0) {
            total += solid_vertex_count_for((uint32_t)i, s_shown_curved);
        }
    }
    return total;
}
