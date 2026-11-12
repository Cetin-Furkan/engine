#include "world/world_int.h"
#include "world/motion.h"
#include "frame/cube.h"
#include "frame/fillet.h"
#include "frame/grid.h"
#include "frame/material.h"
#include "lighting/lighting.h"
#include "physics/physics.h"
#include <math.h>
#include <stdio.h>

enum : int32_t { k_yaw_start_milli = 38000 };

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
    float    vx;
    float    vy;
    float    vz;
    float    restitution;
    bool     awake;
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
static bool cube_simulate(WorldObj *o, uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt);
static void cube_simulate_hold(WorldObj *o);
static void cube_simulate_commit(WorldObj *o, uint32_t stamp, uint32_t heat_dt);
static bool cube_busy(const WorldObj *o);
static void cube_shove(WorldObj *o, Coord hit);

static float rad_of(int32_t milli) {
    return fillet_rad((float)milli / 1000.0f);
}

static bool slab_hit(const CameraRay *ray, Coord center, float yaw_rad, float half, float *t_hit, Coord *at) {
    float ax[3];
    float ay[3];
    float az[3];
    fillet_axes(yaw_rad, ax, ay, az);
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
    // The slab remains until the bake has stored the mesh the vertex shader poses.
    if (cube_surface_ready()) {
        float yaw_deg = (float)c->spin.shown_milli / 1000.0f;
        float ht = 0.0f;
        float hx = 0.0f;
        float hy = 0.0f;
        float hz = 0.0f;
        if (!cube_surface_hit(c->heat, yaw_deg, o->at.x, o->at.y, o->at.z, ray, &ht, &hx, &hy, &hz)) {
            return false;
        }
        if (t != nullptr) {
            *t = ht;
        }
        if (at != nullptr) {
            *at = (Coord){ hx, hy, hz };
        }
        return true;
    }
    float yaw = rad_of(c->spin.shown_milli);
    return slab_hit(ray, o->at, yaw, k_fillet_half, t, at);
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

static bool s_motion_drew = false;

static bool cube_advance(WorldObj *o, uint32_t time_ms) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    advance_phase(c, time_ms);
    bool spun = spin_advance(&c->spin, time_ms, true);
    if (s_motion_drew) {
        s_motion_drew = false;
        return true;
    }
    return spun;
}

static void cube_commit(WorldObj *o) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    c->phase_shown = c->phase;
}

static void cube_prepare_scene(const WorldObj *o, bool live) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    bool glow = live ? c->spin.glow : c->spin.shown_glow;
    material_set(c->mat, glow);

    float phase   = live ? c->phase : c->phase_shown;
    float ang     = 6.28318530718f * phase;
    float ca      = cosf(ang);
    float sa      = sinf(ang);
    const float k = 0.57735026919f;
    const float base[3] = { 0.72f, 0.46f, 0.28f };
    float dot_kb  = k * (base[0] + base[1] + base[2]);
    float cx      = k * (base[2] - base[1]);
    float cy      = k * (base[0] - base[2]);
    float cz      = k * (base[1] - base[0]);

    float sel = table_highlight(o, live);
    float alb[3] = {
        fminf(fmaxf(base[0] * ca + cx * sa + k * dot_kb * (1.0f - ca), 0.0f), 1.0f),
        fminf(fmaxf(base[1] * ca + cy * sa + k * dot_kb * (1.0f - ca), 0.0f), 1.0f),
        fminf(fmaxf(base[2] * ca + cz * sa + k * dot_kb * (1.0f - ca), 0.0f), 1.0f),
    };
    for (int i = 0; i < 3; ++i) {
        alb[i] = alb[i] + (1.0f - alb[i]) * (sel * 0.42f);
    }

    float emi[3] = {0};
    lighting_material_get_emissive(c->mat, emi);
    if (!cube_heat_held(c->heat)) {
        emi[0] += 0.88f;
        emi[1] += 0.34f;
        emi[2] += 0.08f;
    }

    const float center[3]  = { o->at.x, o->at.y, o->at.z };
    const float extents[3] = { k_fillet_half, k_fillet_half, k_fillet_half };
    const float pitch_rad  = 28.0f * (3.14159265359f / 180.0f);
    const float yaw_rad    = spin_deg(&c->spin, live) * (3.14159265359f / 180.0f);

    LightingProxy proxy = {0};
    lighting_proxy_fill(&proxy, LIGHTING_PROXY_OBB, center, extents,
                        pitch_rad, yaw_rad, 0.36f, alb, emi);
    lighting_submit_proxy(&proxy);
}

static void cube_draw_obj(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    bool glow = live ? c->spin.glow : c->spin.shown_glow;
    material_set(c->mat, glow);
    float phase = live ? c->phase : c->phase_shown;
    cube_draw(cmd, width, height, spin_deg(&c->spin, live), o->at.x, o->at.y, o->at.z, 0.0f,
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

static bool cube_respin(WorldObj *o) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return false;
    }
    spin_respin(&c->spin);
    table_mark_spun();
    printf("[Engine] Cube turns again.\n");
    fflush(stdout);
    return true;
}

static bool cube_remove(WorldObj *o) {
    CubeData *c = o->data;
    o->alive = false;
    if (c != nullptr) {
        c->used = false;
        c->awake = false;
        c->vx = 0.0f;
        c->vy = 0.0f;
        c->vz = 0.0f;
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
    bool poked = cube_heat_poke(c->heat, hit.x - o->at.x, hit.y - o->at.y, hit.z - o->at.z);
    cube_shove(o, hit);
    return poked || c->awake;
}

static uint32_t cube_vertices(const WorldObj *o) {
    (void)o;
    return cube_vertex_count();
}

static const WorldOps s_ops = {
    .hit                  = cube_hit,
    .on_click             = cube_on_click,
    .vertices             = cube_vertices,
    .prepare_scene        = cube_prepare_scene,
    .draw                 = cube_draw_obj,
    .advance              = cube_advance,
    .commit               = cube_commit,
    .toggle_spin          = cube_toggle_spin,
    .toggle_glow          = cube_toggle_glow,
    .respin               = cube_respin,
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
    .busy                 = cube_busy,
    .fallback             = true,
};

const WorldOps *cube_ops(void) {
    return &s_ops;
}

static uint32_t s_motion_stamp = 0;
static bool     s_motion_have  = false;

static void cube_kind_reset(void) {
    for (uint32_t i = 0; i < k_cube_cap; ++i) {
        material_set(mat_of(i), false);
        s_pool[i] = (CubeData){0};
    }
    motion_reset_warm_cache();
    s_pick_logged = false;
    s_motion_have = false;
    s_motion_stamp = 0;
}

bool cube_kind_spawn_elastic(Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow, float restitution) {
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
    float e = restitution > 0.0f ? restitution : 0.0f;
    if (e > 0.95f) {
        e = 0.95f;
    }
    c->restitution = e;
    float rest = motion_rest_lift(rad_of(yaw_milli), k_fillet_half);
    c->awake = (at.z > rest + 1.0f);
    cube_heat_claim(c->heat);
    c->spin = spin_pose(yaw_milli, spinning, infinite, glow);
    int index = table_add(&s_ops, c, at, &c->spin);
    if (index < 0) {
        cube_heat_retire(c->heat);
        c->used = false;
        fprintf(stderr, "[World] No room for the cube.\n");
        return false;
    }
    if (spinning) {
        table_mark_spun();
    }
    return true;
}

bool cube_kind_spawn_pose(Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow) {
    return cube_kind_spawn_elastic(at, yaw_milli, spinning, infinite, glow, 0.0f);
}

bool cube_kind_spawn(Coord at) {
    return cube_kind_spawn_pose(at, k_yaw_start_milli, true, false, false);
}

float cube_kind_restitution(const WorldObj *o) {
    if (o == nullptr || o->ops != &s_ops || o->data == nullptr) {
        return 0.0f;
    }
    const CubeData *c = o->data;
    return c->restitution;
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
    bool hit = slab_hit(&mid, origin, yaw, k_fillet_half, &t_mid, nullptr);
    bool miss = slab_hit(&far, origin, yaw, k_fillet_half, &t_far, nullptr);
    bool plane = fabsf(mid.plane_x) < 1.0e-3f && fabsf(mid.plane_y) < 1.0e-3f;
    bool mesh = !cube_surface_ready() || cube_surface_self_check();
    if (!hit || miss || !plane || !mesh) {
        fprintf(stderr, "[World Error] Pick self-check failed\n");
        s_pick_logged = true;
    }
}

static void cube_record_compute(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    const CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    // heat_dt is the interval the table was given. This slot never
    // integrates more than that, and a held slot stays at zero.
    uint32_t dt = cube_heat_step_dt(c->heat);
    if (dt > heat_dt) {
        dt = heat_dt;
    }
    if (!cube_heat_record(cmd, c->heat, spin_deg(&c->spin, live), dt)) {
        return;
    }
    grid_record(cmd, dt);
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

static bool cube_busy(const WorldObj *o) {
    const CubeData *c = o->data;
    return c != nullptr && c->awake;
}

static void cube_shove(WorldObj *o, Coord hit) {
    CubeData *c = o->data;
    if (c == nullptr) {
        return;
    }
    float dx = o->at.x - hit.x;
    float dy = o->at.y - hit.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    c->vx += dx / len * 640.0f;
    c->vy += dy / len * 640.0f;
    if (c->restitution > 0.0f) {
        c->vz += 480.0f * c->restitution;
    }
    c->awake = true;
    s_motion_drew = true;
    printf("[Cube] Shoved.\n");
    fflush(stdout);
}

void world_step_motion(uint32_t time_ms, uint32_t sim_dt_ms) {
    if (sim_dt_ms == 0u || (s_motion_have && s_motion_stamp == time_ms)) {
        return;
    }
    s_motion_have = true;
    s_motion_stamp = time_ms;
    enum { k_body_cap = k_cube_cap + 6u };
    MotionBody bodies[k_body_cap];
    WorldObj *objs[k_body_cap];
    uint32_t cube_n = 0;
    int count = table_count();
    for (int i = 0; i < count && cube_n < k_cube_cap; ++i) {
        WorldObj *o = table_at(i);
        if (o == nullptr || !o->alive || o->ops != &s_ops) {
            continue;
        }
        CubeData *c = o->data;
        if (c == nullptr) {
            continue;
        }
        bodies[cube_n] = (MotionBody){
            .x           = o->at.x,
            .y           = o->at.y,
            .z           = o->at.z,
            .vx          = c->vx,
            .vy          = c->vy,
            .vz          = c->vz,
            .yaw         = rad_of(c->spin.live_milli),
            .restitution = c->restitution,
            .awake       = c->awake,
        };
        objs[cube_n] = o;
        cube_n++;
    }
    uint32_t solid_n = solid_kind_gather_motion(bodies + cube_n, objs + cube_n, 6u);
    uint32_t n = cube_n + solid_n;
    bool any_before = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (bodies[i].awake) {
            any_before = true;
        }
    }
    uint32_t iters = physics_recommended_iterations();
    uint32_t left = sim_dt_ms;
    while (left > 0u) {
        uint32_t step = left > 8u ? 8u : left;
        float dt_s = (float)step / 1000.0f;
        AvbdVertex gpu_v[K_AVBD_MAX_VERTS];
        AvbdConstraint gpu_c[K_AVBD_MAX_CONS];
        uint32_t gpu_vn = 0u;
        uint32_t gpu_cn = 0u;
        motion_substep_avbd(bodies, n, dt_s, 0.0f, k_fillet_half, iters,
                            left <= 8u ? gpu_v : nullptr,
                            left <= 8u ? &gpu_vn : nullptr,
                            left <= 8u ? gpu_c : nullptr,
                            left <= 8u ? &gpu_cn : nullptr);
        if (left <= 8u && gpu_vn > 0u) {
            const float g_world[3] = { 0.0f, 0.0f, -3200.0f };
            physics_submit_step(gpu_v, gpu_vn, gpu_c, gpu_cn, dt_s, g_world, expf(-1.8f * dt_s));
        }
        left -= step;
    }
    bool any_after = false;
    bool moved = false;
    for (uint32_t i = 0; i < cube_n; ++i) {
        WorldObj *o = objs[i];
        CubeData *c = o->data;
        if (fabsf(o->at.x - bodies[i].x) > 1.0e-3f || fabsf(o->at.y - bodies[i].y) > 1.0e-3f ||
            fabsf(o->at.z - bodies[i].z) > 1.0e-3f) {
            moved = true;
        }
        o->at.x = bodies[i].x;
        o->at.y = bodies[i].y;
        o->at.z = bodies[i].z;
        c->vx = bodies[i].vx;
        c->vy = bodies[i].vy;
        c->vz = bodies[i].vz;
        c->awake = bodies[i].awake;
        if (c->awake) {
            any_after = true;
        }
    }
    if (solid_kind_apply_motion(bodies + cube_n, objs + cube_n, solid_n,
                                (float)sim_dt_ms / 1000.0f, &any_after)) {
        moved = true;
    }
    if (moved) {
        s_motion_drew = true;
    }
    if (any_before && !any_after) {
        printf("[Cube] The cubes are resting.\n");
        fflush(stdout);
        (void)world_save();
    }
}

static bool cube_simulate(WorldObj *o, uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt) {
    CubeData *c = o->data;
    if (c == nullptr) {
        if (heat_dt != nullptr) {
            *heat_dt = 0;
        }
        return false;
    }
    world_step_motion(time_ms, sim_dt_ms);
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
    uint32_t one = cube_heat_interval(c->heat, sim_dt_ms);
    if (heat_dt != nullptr) {
        *heat_dt = one;
    }
    return pulse || c->awake;
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
    // The issued interval was stored in simulate. There is no private stamp.
    (void)o;
    (void)stamp;
    (void)heat_dt;
}

// The kind opts itself in. An empty boot and world.c do not name it.
// Reset still runs, and the pick self-check still runs after the camera.
[[gnu::constructor]] static void cube_kind_register(void) {
    table_kind_listen(cube_kind_reset, cube_kind_self_check);
}
