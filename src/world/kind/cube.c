#include "world/world_int.h"
#include "frame/cube.h"
#include "frame/grid.h"
#include "frame/material.h"
#include <math.h>
#include <stdio.h>

enum : int32_t { k_yaw_start_milli = 38000 };

static const float k_cube_half = 120.0f;
static const float k_deg       = 0.01745329251f;

typedef struct CubeData {
    Spin     spin;
    float    phase;
    float    phase_shown;
    uint32_t phase_t0;
    bool     phase_have;
} CubeData;

static CubeData s_cube;
static bool     s_up = false;
static bool     s_pick_logged = false;
static const WorldOps s_ops;

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

static bool cube_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    float yaw = rad_of(s_cube.spin.shown_milli);
    return slab_hit(ray, o->at, yaw, k_cube_half, t, at);
}

static void advance_phase(uint32_t time_ms) {
    if (!s_cube.phase_have) {
        s_cube.phase_t0 = time_ms;
        s_cube.phase_have = true;
        return;
    }
    uint32_t step = time_ms - s_cube.phase_t0;
    if (step == 0u) {
        return;
    }
    if (step > 1000u) {
        s_cube.phase_t0 = time_ms;
        return;
    }
    s_cube.phase_t0 = time_ms;
    s_cube.phase += (float)step / 5000.0f;
    if (s_cube.phase >= 1.0f) {
        s_cube.phase -= floorf(s_cube.phase);
    }
}

static bool cube_advance(WorldObj *o, uint32_t time_ms) {
    (void)o;
    advance_phase(time_ms);
    return spin_advance(&s_cube.spin, time_ms, true);
}

static void cube_commit(WorldObj *o) {
    (void)o;
    s_cube.phase_shown = s_cube.phase;
}

static void cube_draw_obj(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    bool glow = live ? s_cube.spin.glow : s_cube.spin.shown_glow;
    material_set(k_mat_cube, glow);
    float phase = live ? s_cube.phase : s_cube.phase_shown;
    cube_draw(cmd, width, height, spin_deg(&s_cube.spin, live), o->at.x, o->at.y, 0.0f,
              phase, table_highlight(o, live));
}

static bool cube_toggle_spin(WorldObj *o) {
    (void)o;
    if (spin_toggle(&s_cube.spin)) {
        printf("[Engine] Cube spins.\n");
    } else {
        printf("[Engine] Cube holds.\n");
    }
    fflush(stdout);
    return true;
}

static bool cube_toggle_glow(WorldObj *o) {
    (void)o;
    s_cube.spin.glow = !s_cube.spin.glow;
    printf("[Material] Cube glow %s.\n", s_cube.spin.glow ? "on" : "off");
    fflush(stdout);
    return true;
}

static const char *cube_name(const WorldObj *o) {
    (void)o;
    return "Cube";
}

static const WorldOps s_ops = {
    .hit         = cube_hit,
    .draw        = cube_draw_obj,
    .advance     = cube_advance,
    .commit      = cube_commit,
    .toggle_spin = cube_toggle_spin,
    .toggle_glow = cube_toggle_glow,
    .name        = cube_name,
};

const WorldOps *cube_ops(void) {
    return &s_ops;
}

void cube_kind_reset(void) {
    s_cube = (CubeData){0};
    s_up = false;
    s_pick_logged = false;
    material_set(k_mat_cube, false);
}

bool cube_kind_spawn(Coord at) {
    if (s_up) {
        fprintf(stderr, "[World] The file names a second cube. One fillet is placed.\n");
        return false;
    }
    s_cube = (CubeData){0};
    s_cube.spin = spin_start(k_yaw_start_milli);
    int index = table_add(&s_ops, &s_cube, at, &s_cube.spin);
    if (index < 0) {
        fprintf(stderr, "[World] No room for the cube.\n");
        return false;
    }
    s_up = true;
    table_mark_spun();
    return true;
}

void cube_kind_self_check(void) {
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

float cube_kind_yaw(bool live) {
    if (!s_up) {
        return (float)k_yaw_start_milli / 1000.0f;
    }
    return spin_deg(&s_cube.spin, live);
}

bool cube_kind_toggle_fallback(void) {
    WorldObj *o = table_find(&s_ops);
    if (o == nullptr) {
        return false;
    }
    return cube_toggle_spin(o);
}

bool cube_kind_glow_fallback(void) {
    WorldObj *o = table_find(&s_ops);
    if (o == nullptr) {
        return false;
    }
    return cube_toggle_glow(o);
}

bool cube_kind_poke(Coord world_hit) {
    float x = world_hit.x;
    float y = world_hit.y;
    WorldObj *o = table_find(&s_ops);
    if (o != nullptr) {
        x -= o->at.x;
        y -= o->at.y;
    }
    return cube_heat_poke(x, y);
}

void cube_kind_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    if (heat_dt == 0u && !cube_heat_poke_pending()) {
        return;
    }
    cube_heat_record(cmd, heat_dt, cube_kind_yaw(live));
    grid_record(cmd, heat_dt);
}

void cube_kind_record_ok(uint64_t signal_point) {
    cube_heat_record_ok(signal_point);
    grid_record_ok();
}

void cube_kind_record_abort(void) {
    cube_heat_record_abort();
    grid_record_abort();
}

uint64_t cube_kind_published(void) {
    return cube_heat_published();
}

bool cube_kind_step_open(void) {
    return cube_heat_step_open();
}

bool cube_kind_poke_pending(void) {
    return cube_heat_poke_pending();
}

bool cube_kind_simulation(uint32_t time_ms, uint32_t *heat_dt) {
    bool ended = false;
    bool pulse = cube_heat_wants_frame(time_ms, &ended);
    if (ended) {
        cube_heat_hold();
        grid_hold();
    } else if (pulse) {
        grid_wake();
    }
    if (heat_dt != nullptr) {
        *heat_dt = cube_heat_interval(time_ms);
    }
    return pulse;
}

void cube_kind_hold(void) {
    cube_heat_hold();
    grid_hold();
}

void cube_kind_commit_stamp(uint32_t stamp, uint32_t heat_dt) {
    if (heat_dt > 0u) {
        cube_heat_commit_stamp(stamp);
    }
}

uint32_t cube_kind_stat_vertices(void) {
    if (!s_up) {
        return 0u;
    }
    return cube_vertex_count();
}
