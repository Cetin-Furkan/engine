#include "world/world_int.h"
#include "world/motion.h"
#include "frame/fillet.h"
#include "frame/material.h"
#include "frame/solid.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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
static const float k_default_bounce[k_kinds] = {
    0.40f, // Tetrahedron
    0.68f, // Octahedron
    0.88f, // Icosahedron (high elasticity)
    0.22f, // Cylinder (low elasticity)
    0.56f, // Cone
    0.82f, // Torus (elastic ring)
};
static const float k_radius = 84.0f;

typedef struct SolidData {
    bool     used;
    uint32_t kind;
    Spin     spin;
    float    vx;
    float    vy;
    float    vz;
    float    restitution;
    float    squash;
    float    squash_vel;
    float    shown_squash;
    bool     awake;
} SolidData;

static SolidData s_solid[k_kinds];
static bool      s_curved = false;
static bool      s_shown_curved = false;
static bool      s_motion_drew = false;
static const WorldOps s_ops;

static float rad_of(int32_t milli) {
    return fillet_rad((float)milli / 1000.0f);
}

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

static bool solid_on_click(WorldObj *o, Coord hit) {
    SolidData *s = o->data;
    if (s == nullptr) {
        return false;
    }
    float dx = o->at.x - hit.x;
    float dy = o->at.y - hit.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    float e = s->restitution;
    s->vx += dx / len * 580.0f;
    s->vy += dy / len * 580.0f;
    s->vz += 520.0f + 460.0f * e;
    s->squash_vel -= 5.2f * (0.35f + e);
    s->awake = true;
    world_note_motion_epoch();
    s_motion_drew = true;
    printf("[Solid] %s shoved (elasticity %.2f).\n", k_name[s->kind], (double)e);
    fflush(stdout);
    return true;
}

static bool solid_advance(WorldObj *o, uint32_t time_ms) {
    SolidData *s = o->data;
    bool spun = spin_advance(&s->spin, time_ms, false);
    if (s_motion_drew || fabsf(s->squash - s->shown_squash) > 1.0e-3f) {
        s_motion_drew = false;
        return true;
    }
    return spun;
}

static void solid_commit(WorldObj *o) {
    SolidData *s = o->data;
    if (s != nullptr) {
        s->shown_squash = s->squash;
    }
}

static bool solid_busy(const WorldObj *o) {
    const SolidData *s = o->data;
    if (s == nullptr) {
        return false;
    }
    return s->awake || fabsf(s->squash - s->shown_squash) > 1.0e-3f;
}

static bool solid_simulate(WorldObj *o, uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt) {
    if (heat_dt != nullptr) {
        *heat_dt = 0u;
    }
    const SolidData *s = o->data;
    if (s == nullptr) {
        return false;
    }
    world_step_motion(time_ms, sim_dt_ms);
    return s->awake;
}

static void solid_prepare_obj(const WorldObj *o, bool live) {
    const SolidData *s = o->data;
    bool glow = live ? s->spin.glow : s->spin.shown_glow;
    material_set(k_mat_solid0 + s->kind, glow);
    bool curved = live ? s_curved : s_shown_curved;
    float squash = live ? s->squash : s->shown_squash;
    solid_submit_scene(s->kind, curved, spin_deg(&s->spin, live),
                       o->at.x, o->at.y, o->at.z, squash, table_highlight(o, live));
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
    s->awake = false;
    s->vx = 0.0f;
    s->vy = 0.0f;
    s->vz = 0.0f;
    s->squash = 0.0f;
    s->squash_vel = 0.0f;
    s->shown_squash = 0.0f;
    s->spin.spinning = false;
    s->spin.infinite = false;
    world_note_motion_epoch();
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
    .hit           = solid_hit,
    .on_click      = solid_on_click,
    .vertices      = solid_vertices,
    .prepare_scene = solid_prepare_obj,
    .advance       = solid_advance,
    .commit        = solid_commit,
    .busy          = solid_busy,
    .simulate      = solid_simulate,
    .toggle_spin   = solid_toggle_spin,
    .toggle_glow   = solid_toggle_glow,
    .respin        = solid_respin,
    .remove        = solid_remove,
    .name          = solid_name,
};

void solid_kind_reset(void) {
    for (int i = 0; i < k_kinds; ++i) {
        s_solid[i] = (SolidData){0};
        material_set(k_mat_solid0 + (uint32_t)i, false);
    }
    s_curved = false;
    s_shown_curved = false;
    s_motion_drew = false;
    world_note_motion_epoch();
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
    if (!solid_kind_place(kind, k_at[kind], 0, true, false, false)) {
        return false;
    }
    int index = find_kind(kind);
    if (index >= 0) {
        table_select_quiet(index);
    }
    printf("[Engine] %s up. One revolution, then it holds.\n", k_name[kind]);
    fflush(stdout);
    return true;
}

const char *solid_kind_name(uint32_t kind) {
    if (kind >= k_kinds) {
        return nullptr;
    }
    return k_name[kind];
}

int solid_kind_index(const char *name) {
    if (name == nullptr) {
        return -1;
    }
    for (uint32_t i = 0; i < k_kinds; ++i) {
        if (strcmp(name, k_name[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool solid_kind_place_elastic(uint32_t kind, Coord at, int32_t yaw_milli,
                              bool spinning, bool infinite, bool glow, float restitution) {
    if (kind >= k_kinds) {
        return false;
    }
    if (find_kind(kind) >= 0) {
        fprintf(stderr, "[World] %s is already placed.\n", k_name[kind]);
        return false;
    }
    SolidData *s = &s_solid[kind];
    *s = (SolidData){0};
    s->used = true;
    s->kind = kind;
    float e = restitution >= 0.0f ? restitution : k_default_bounce[kind];
    if (e > 0.95f) {
        e = 0.95f;
    }
    s->restitution = e;
    s->awake = (at.z > k_radius + 1.0f);
    s->spin = spin_pose(yaw_milli, spinning, infinite, glow);
    int index = table_add(&s_ops, s, at, &s->spin);
    if (index < 0) {
        s->used = false;
        fprintf(stderr, "[World] No room for %s.\n", k_name[kind]);
        return false;
    }
    if (spinning) {
        table_mark_spun();
    }
    world_note_motion_epoch();
    return true;
}

bool solid_kind_place(uint32_t kind, Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow) {
    return solid_kind_place_elastic(kind, at, yaw_milli, spinning, infinite, glow, -1.0f);
}

float solid_kind_restitution(const WorldObj *o) {
    if (o == nullptr || o->ops != &s_ops || o->data == nullptr) {
        return 0.0f;
    }
    const SolidData *s = o->data;
    return s->restitution;
}

uint32_t solid_kind_gather_motion(MotionBody *out, WorldObj **objs, uint32_t cap) {
    uint32_t n = 0;
    int count = table_count();
    for (int i = 0; i < count && n < cap; ++i) {
        WorldObj *o = table_at(i);
        if (o == nullptr || !o->alive || o->ops != &s_ops || o->data == nullptr) {
            continue;
        }
        SolidData *s = o->data;
        out[n] = (MotionBody){
            .x           = o->at.x,
            .y           = o->at.y,
            .z           = o->at.z,
            .vx          = s->vx,
            .vy          = s->vy,
            .vz          = s->vz,
            .yaw         = rad_of(s->spin.live_milli),
            .restitution = s->restitution,
            .radius      = k_radius,
            .spherical   = true,
            .awake       = s->awake,
        };
        objs[n] = o;
        n++;
    }
    return n;
}

void solid_kind_sync_awake(const MotionBody *bodies, WorldObj *const *objs, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        SolidData *s = objs[i] != nullptr ? objs[i]->data : nullptr;
        if (s != nullptr && bodies[i].awake) {
            s->awake = true;
        }
    }
}

bool solid_kind_apply_motion(const MotionBody *bodies, WorldObj *const *objs, uint32_t n,
                             float dt_s, bool *any_awake) {
    bool moved = false;
    for (uint32_t i = 0; i < n; ++i) {
        WorldObj *o = objs[i];
        SolidData *s = o->data;
        if (fabsf(o->at.x - bodies[i].x) > 1.0e-3f || fabsf(o->at.y - bodies[i].y) > 1.0e-3f ||
            fabsf(o->at.z - bodies[i].z) > 1.0e-3f) {
            moved = true;
        }
        o->at.x = bodies[i].x;
        o->at.y = bodies[i].y;
        o->at.z = bodies[i].z;
        s->vx = bodies[i].vx;
        s->vy = bodies[i].vy;
        s->vz = bodies[i].vz;
        s->awake = bodies[i].awake;
        if (bodies[i].impact > 24.0f) {
            float flex = 0.35f + 0.85f * s->restitution;
            float kick = (bodies[i].impact / 900.0f) * 8.5f * flex;
            if (kick > 12.0f) {
                kick = 12.0f;
            }
            s->squash_vel -= kick;
            s->awake = true;
        }
        if (dt_s > 0.0f && (s->awake || fabsf(s->squash) > 1.0e-3f || fabsf(s->squash_vel) > 1.0e-2f)) {
            float omega = 18.0f + 16.0f * (1.0f - s->restitution);
            float damp = 4.2f + 4.5f * (1.0f - s->restitution);
            float acc = -(omega * omega) * s->squash - damp * s->squash_vel;
            s->squash_vel += acc * dt_s;
            s->squash += s->squash_vel * dt_s;
            if (s->squash < -0.45f) {
                s->squash = -0.45f;
                s->squash_vel = 0.0f;
            } else if (s->squash > 0.55f) {
                s->squash = 0.55f;
                s->squash_vel = 0.0f;
            }
            if (!bodies[i].awake && fabsf(s->squash) < 2.5e-3f && fabsf(s->squash_vel) < 2.0e-2f) {
                s->squash = 0.0f;
                s->squash_vel = 0.0f;
            } else {
                s->awake = true;
                moved = true;
            }
        }
        if (s->awake && any_awake != nullptr) {
            *any_awake = true;
        }
    }
    if (moved) {
        s_motion_drew = true;
    }
    return moved;
}

const WorldOps *solid_ops(void) {
    return &s_ops;
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

bool solid_kind_smooth(void) {
    return s_curved;
}

void solid_kind_set_smooth(bool curved) {
    s_curved = curved;
    s_shown_curved = curved;
}

bool solid_kind_smooth_dirty(void) {
    return s_curved != s_shown_curved;
}

void solid_kind_commit_smooth(void) {
    s_shown_curved = s_curved;
}
