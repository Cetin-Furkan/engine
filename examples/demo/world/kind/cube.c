#include "world/world_int.h"
#include "frame/cube.h"
#include "frame/grid.h"
#include "frame/material.h"
#include <math.h>
#include <stdio.h>

enum : int32_t { k_yaw_start_milli = 38000 };

static const float k_cube_half = 120.0f;
static const float k_deg       = 0.01745329251f;

// One fillet mesh, many placements. Each opening.world cube line takes a
// slot, its own spin, its own hue phase, and its own temperature field.
// Slot 0 uses material 0. Slots 1..23 use materials 40..62. A click plants
// heat on that fillet only. The 128^3 tint stays one mesh-local sample.
enum : uint32_t { k_cube_cap = k_cube_heat_slots };

typedef struct CubeData {
    bool     used;
    uint32_t mat;
    uint32_t heat;
    Spin     spin;
    float    phase;
    float    phase_shown;
    uint32_t phase_t0;
    bool     phase_have;
} CubeData;

static_assert(k_mat_cube_more + k_cube_cap - 2u < k_mat_none, "cube looks fit the table");

static CubeData s_pool[k_cube_cap];
static bool     s_pick_logged = false;
static const WorldOps s_ops;

static void cube_kind_reset(void);
static void cube_kind_self_check(void);
static uint32_t mat_of(uint32_t index);
static bool any_heat_running(void);
static void cube_record_compute(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live);
static void cube_record_ok(WorldObj *o, uint64_t signal_point);
static void cube_record_abort(WorldObj *o);
static uint64_t cube_record_published(const WorldObj *o);
static bool cube_record_step_open(const WorldObj *o);
static bool cube_record_poke_pending(const WorldObj *o);
static bool cube_simulate(WorldObj *o, uint32_t time_ms, uint32_t *heat_dt);
static void cube_simulate_hold(WorldObj *o);
static void cube_simulate_commit(WorldObj *o, uint32_t stamp, uint32_t heat_dt);

static float rad_of(int32_t milli) {
    return ((float)milli / 1000.0f) * k_deg;
}

// Local axes of Ry(yaw) * Rx(28°), matching cube.vert.
static void pose_axes(float yaw_rad, float x[3], float y[3], float z[3]) {
    const float pitch = 28.0f * k_deg;
    float cx = cosf(pitch);
    float sx = sinf(pitch);
    float cy = cosf(yaw_rad);
    float sy = sinf(yaw_rad);
    float rx0[3] = { 1.0f, 0.0f, 0.0f };
    float rx1[3] = { 0.0f, cx, sx };
    float rx2[3] = { 0.0f, -sx, cx };
    x[0] = cy * rx0[0] + sy * rx0[2];
    x[1] = rx0[1];
    x[2] = -sy * rx0[0] + cy * rx0[2];
    y[0] = cy * rx1[0] + sy * rx1[2];
    y[1] = rx1[1];
    y[2] = -sy * rx1[0] + cy * rx1[2];
    z[0] = cy * rx2[0] + sy * rx2[2];
    z[1] = rx2[1];
    z[2] = -sy * rx2[0] + cy * rx2[2];
}

static bool slab_hit(const CameraRay *ray, Coord center, float yaw_rad, float half, float *t_hit, Coord *at) {
    float ax[3];
    float ay[3];
    float az[3];
    pose_axes(yaw_rad, ax, ay, az);
    const float *axis[3] = { ax, ay, az };
    float delta[3] = { ray->ox - center.x, ray->oy - center.y, ray->oz - center.z };
    float dir[3] = { ray->dx, ray->dy, ray->dz };
    float t0 = 0.0f;
    float t1 = 1.0e30f;
    float h = half + coord_near();
    for (int i = 0; i < 3; ++i) {
        float o = delta[0] * axis[i][0] + delta[1] * axis[i][1] + delta[2] * axis[i][2];
        float d = dir[0] * axis[i][0] + dir[1] * axis[i][1] + dir[2] * axis[i][2];
        if (fabsf(d) < 1.0e-8f) {
            if (o < -h || o > h) {
                return false;
            }
            continue;
        }
        float a = (-h - o) / d;
        float b = (h - o) / d;
        if (a > b) {
            float tmp = a;
            a = b;
            b = tmp;
        }
        if (a > t0) {
            t0 = a;
        }
        if (b < t1) {
            t1 = b;
        }
        if (t0 > t1) {
            return false;
        }
    }
    float t = t0 >= 0.0f ? t0 : t1;
    if (t < 0.0f) {
        return false;
    }
    if (t_hit != nullptr) {
        *t_hit = t;
    }
    if (at != nullptr) {
        *at = (Coord){
            ray->ox + ray->dx * t,
            ray->oy + ray->dy * t,
            ray->oz + ray->dz * t,
        };
    }
    return true;
}

static uint32_t mat_of(uint32_t index) {
    if (index == 0u) {
        return k_mat_cube;
    }
    return k_mat_cube_more + (index - 1u);
}

static bool any_heat_running(void) {
    for (uint32_t i = 0; i < k_cube_cap; ++i) {
        if (s_pool[i].used && !cube_heat_held(s_pool[i].heat)) {
            return true;
        }
    }
    return false;
}

static bool cube_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    float yaw = rad_of(c->spin.shown_milli);
    return slab_hit(ray, o->at, yaw, k_cube_half, t, at);
}

static void advance_phase(CubeData *c, uint32_t time_ms) {
    if (!c->phase_have) {
        c->phase_t0 = time_ms;
        c->phase_have = true;
        return;
    }
    uint32_t step = time_ms - c->phase_t0;
    if (step == 0u) {
        return;
    }
    if (step > 1000u) {
        c->phase_t0 = time_ms;
        return;
    }
    c->phase_t0 = time_ms;
    c->phase += (float)step / 5000.0f;
    if (c->phase >= 1.0f) {
        c->phase -= floorf(c->phase);
    }
}

static bool cube_advance(WorldObj *o, uint32_t time_ms) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    advance_phase(c, time_ms);
    return spin_advance(&c->spin, time_ms, true);
}

static void cube_commit(WorldObj *o) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    c->phase_shown = c->phase;
}

static void cube_draw_obj(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    bool glow = live ? c->spin.glow : c->spin.shown_glow;
    material_set(c->mat, glow);
    float phase = live ? c->phase : c->phase_shown;
    cube_draw(cmd, width, height, spin_deg(&c->spin, live), o->at.x, o->at.y, 0.0f,
              phase, table_highlight(o, live), c->mat, c->heat);
}

static bool cube_toggle_spin(WorldObj *o) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    if (spin_toggle(&c->spin)) {
        printf("[Engine] Cube spins.\n");
    } else {
        printf("[Engine] Cube holds.\n");
    }
    fflush(stdout);
    return true;
}

static bool cube_toggle_glow(WorldObj *o) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    c->spin.glow = !c->spin.glow;
    printf("[Material] Cube glow %s.\n", c->spin.glow ? "on" : "off");
    fflush(stdout);
    return true;
}

static bool cube_remove(WorldObj *o) {
    CubeData *c = o->data;
    o->alive = false;
    if (c != nullptr) {
        c->used = false;
        c->spin.spinning = false;
        c->spin.infinite = false;
        material_set(c->mat, false);
        cube_heat_retire(c->heat);
    }
    if (!any_heat_running()) {
        grid_hold();
    }
    printf("[Engine] Cube removed.\n");
    fflush(stdout);
    return true;
}

static const char *cube_name(const WorldObj *o) {
    (void)o;
    return "Cube";
}

static bool cube_on_click(WorldObj *o, Coord hit) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    return cube_heat_poke(c->heat, hit.x - o->at.x, hit.y - o->at.y);
}

static uint32_t cube_vertices(const WorldObj *o) {
    (void)o;
    return cube_vertex_count();
}

static const WorldOps s_ops = {
    .hit                  = cube_hit,
    .on_click             = cube_on_click,
    .vertices             = cube_vertices,
    .draw                 = cube_draw_obj,
    .advance              = cube_advance,
    .commit               = cube_commit,
    .toggle_spin          = cube_toggle_spin,
    .toggle_glow          = cube_toggle_glow,
    .remove               = cube_remove,
    .name                 = cube_name,
    .record_compute       = cube_record_compute,
    .record_ok            = cube_record_ok,
    .record_abort         = cube_record_abort,
    .record_published     = cube_record_published,
    .record_step_open     = cube_record_step_open,
    .record_poke_pending  = cube_record_poke_pending,
    .simulate             = cube_simulate,
    .simulate_hold        = cube_simulate_hold,
    .simulate_commit      = cube_simulate_commit,
    .fallback             = true,
};

const WorldOps *cube_ops(void) {
    return &s_ops;
}

static void cube_kind_reset(void) {
    for (uint32_t i = 0; i < k_cube_cap; ++i) {
        material_set(mat_of(i), false);
        s_pool[i] = (CubeData){0};
    }
    s_pick_logged = false;
}

bool cube_kind_spawn(Coord at) {
    int slot = -1;
    for (uint32_t i = 0; i < k_cube_cap; ++i) {
        if (!s_pool[i].used) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        fprintf(stderr, "[World] The cube list is full.\n");
        return false;
    }
    CubeData *c = &s_pool[slot];
    *c = (CubeData){0};
    c->used = true;
    c->mat = mat_of((uint32_t)slot);
    c->heat = (uint32_t)slot;
    cube_heat_claim(c->heat);
    c->spin = spin_start(k_yaw_start_milli);
    int index = table_add(&s_ops, c, at, &c->spin);
    if (index < 0) {
        cube_heat_retire(c->heat);
        c->used = false;
        fprintf(stderr, "[World] No room for the cube.\n");
        return false;
    }
    table_mark_spun();
    return true;
}

static void cube_kind_self_check(void) {
    if (s_pick_logged) {
        return;
    }
    CameraRay mid;
    CameraRay far;
    camera_click_ray(0.0f, 0.0f, &mid);
    camera_click_ray(360.0f, 0.0f, &far);
    float yaw = rad_of(k_yaw_start_milli);
    float t_mid = 0.0f;
    float t_far = 0.0f;
    Coord origin = {0};
    bool hit = slab_hit(&mid, origin, yaw, k_cube_half, &t_mid, nullptr);
    bool miss = slab_hit(&far, origin, yaw, k_cube_half, &t_far, nullptr);
    bool plane = fabsf(mid.plane_x) < 1.0e-3f && fabsf(mid.plane_y) < 1.0e-3f;
    if (!hit || miss || !plane) {
        fprintf(stderr, "[World Error] Pick self-check failed\n");
        s_pick_logged = true;
    }
}

static void cube_record_compute(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    (void)heat_dt;
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    if (!cube_heat_record(cmd, c->heat, spin_deg(&c->spin, live))) {
        return;
    }
    grid_record(cmd, cube_heat_step_dt(c->heat));
}

static void cube_record_ok(WorldObj *o, uint64_t signal_point) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    cube_heat_record_ok(c->heat, signal_point);
    grid_record_ok();
}

static void cube_record_abort(WorldObj *o) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    cube_heat_record_abort(c->heat);
    grid_record_abort();
}

static uint64_t cube_record_published(const WorldObj *o) {
    (void)o;
    return cube_heat_published();
}

static bool cube_record_step_open(const WorldObj *o) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    return cube_heat_slot_open(c->heat);
}

static bool cube_record_poke_pending(const WorldObj *o) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    return cube_heat_slot_pending(c->heat);
}

static bool cube_simulate(WorldObj *o, uint32_t time_ms, uint32_t *heat_dt) {
    CubeData *c = o->data;
    if (c == nullptr) {
        if (heat_dt != nullptr) {
            *heat_dt = 0;
        }
        return false;
    }
    bool ended = false;
    bool pulse = cube_heat_wants_frame(c->heat, time_ms, &ended);
    if (ended) {
        cube_heat_hold(c->heat);
        if (!any_heat_running()) {
            grid_hold();
        }
    } else if (pulse) {
        grid_wake();
    }
    uint32_t one = cube_heat_interval(c->heat, time_ms);
    if (heat_dt != nullptr) {
        *heat_dt = one;
    }
    return pulse;
}

static void cube_simulate_hold(WorldObj *o) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    cube_heat_hold(c->heat);
    grid_hold();
}

static void cube_simulate_commit(WorldObj *o, uint32_t stamp, uint32_t heat_dt) {
    (void)heat_dt;
    CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    cube_heat_commit_stamp(c->heat, stamp);
}

// The kind opts itself in. An empty boot and world.c do not name it.
// Reset still runs, and the pick self-check still runs after the camera.
[[gnu::constructor]] static void cube_kind_register(void) {
    table_kind_listen(cube_kind_reset, cube_kind_self_check);
}
