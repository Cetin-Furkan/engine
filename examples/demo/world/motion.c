#include "world/motion.h"
#include "frame/fillet.h"
#include <math.h>
#include <string.h>

static constexpr float k_gravity     = 3200.0f;
static constexpr float k_damp        = 1.8f;
static constexpr float k_sleep_speed = 12.0f;
static constexpr float k_slop        = 0.5f;

static AvbdWarmCache s_motion_warm;

void motion_reset_warm_cache(void) {
    avbd_warm_cache_reset(&s_motion_warm);
}

static void extent(float yaw, float half, float out[3]) {
    float ax[3];
    float ay[3];
    float az[3];
    fillet_axes(yaw, ax, ay, az);
    for (int i = 0; i < 3; ++i) {
        out[i] = half * (fabsf(ax[i]) + fabsf(ay[i]) + fabsf(az[i]));
    }
}

float motion_rest_lift(float yaw, float half) {
    float e[3];
    extent(yaw, half, e);
    return e[2];
}

static float body_rest_lift(const MotionBody *b, float half) {
    if (b->spherical) {
        return b->radius > 0.0f ? b->radius : half;
    }
    return motion_rest_lift(b->yaw, half);
}

static void resolve_ground_velocity(MotionBody *b, float rest_z) {
    if (!b->awake) {
        return;
    }
    if (b->z <= rest_z + 1.0e-2f) {
        b->z = rest_z;
        if (b->vz < 0.0f) {
            float down = -b->vz;
            if (down > b->impact) {
                b->impact = down;
            }
            float e = b->restitution;
            if (e > 0.95f) {
                e = 0.95f;
            }
            if (e > 0.0f && down > k_sleep_speed * 2.5f) {
                b->vz = down * e;
            } else {
                b->vz = 0.0f;
            }
        }
    }
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float radius_on(const float row[3][3], float half, const float n[3]) {
    return half * (fabsf(dot3(row[0], n)) + fabsf(dot3(row[1], n)) + fabsf(dot3(row[2], n)));
}

// Face normals, then the nine edge crosses. One open axis means the boxes
// are apart, so the loose bounds of a pitched cube are not a hit.
static bool overlap_on(const float raw[3], const float delta[3], const float ra[3][3], const float rb[3][3],
                       float half, float *best, float n_out[3]) {
    float len2 = dot3(raw, raw);
    if (!(len2 > 1.0e-8f)) {
        return true;
    }
    float inv = 1.0f / sqrtf(len2);
    float n[3] = { raw[0] * inv, raw[1] * inv, raw[2] * inv };
    float along = dot3(delta, n);
    if (along < 0.0f) {
        n[0] = -n[0];
        n[1] = -n[1];
        n[2] = -n[2];
        along = -along;
    }
    float pen = radius_on(ra, half, n) + radius_on(rb, half, n) - along;
    if (pen <= k_slop) {
        return false;
    }
    if (pen < *best) {
        *best = pen;
        n_out[0] = n[0];
        n_out[1] = n[1];
        n_out[2] = n[2];
    }
    return true;
}

static bool obb_hit(const MotionBody *a, const MotionBody *b, float half, float *pen, float n[3]) {
    float ra[3][3];
    float rb[3][3];
    fillet_axes(a->yaw, ra[0], ra[1], ra[2]);
    fillet_axes(b->yaw, rb[0], rb[1], rb[2]);
    float delta[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
    float best = 1.0e30f;
    for (int i = 0; i < 3; ++i) {
        if (!overlap_on(ra[i], delta, ra, rb, half, &best, n) ||
            !overlap_on(rb[i], delta, ra, rb, half, &best, n)) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float crossed[3];
            cross3(ra[i], rb[j], crossed);
            if (!overlap_on(crossed, delta, ra, rb, half, &best, n)) {
                return false;
            }
        }
    }
    if (!(best < 1.0e29f)) {
        return false;
    }
    *pen = best;
    return true;
}

static bool body_hit(const MotionBody *a, const MotionBody *b, float half, float *pen, float n[3]) {
    if (!a->spherical && !b->spherical) {
        return obb_hit(a, b, half, pen, n);
    }
    float ra = a->spherical ? (a->radius > 0.0f ? a->radius : half) : half;
    float rb = b->spherical ? (b->radius > 0.0f ? b->radius : half) : half;
    float dx = b->x - a->x;
    float dy = b->y - a->y;
    float dz = b->z - a->z;
    float dist2 = dx * dx + dy * dy + dz * dz;
    float reach = ra + rb;
    float limit = reach - k_slop;
    if (!(limit > 0.0f) || !(dist2 < limit * limit)) {
        return false;
    }
    float dist = sqrtf(dist2);
    if (dist < 1.0e-4f) {
        n[0] = 1.0f;
        n[1] = 0.0f;
        n[2] = 0.0f;
        *pen = reach;
        return true;
    }
    float inv = 1.0f / dist;
    n[0] = dx * inv;
    n[1] = dy * inv;
    n[2] = dz * inv;
    *pen = reach - dist;
    return true;
}

static bool separated(const MotionBody *a, const MotionBody *b, float half) {
    float pen = 0.0f;
    float n[3] = { 0.0f, 0.0f, 0.0f };
    return !body_hit(a, b, half, &pen, n);
}

static void trade(MotionBody *a, MotionBody *b, float nx, float ny, float nz) {
    float vrel = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny + (b->vz - a->vz) * nz;
    if (!(vrel < 0.0f)) {
        return;
    }
    float hit_speed = -vrel;
    if (hit_speed > a->impact) {
        a->impact = hit_speed;
    }
    if (hit_speed > b->impact) {
        b->impact = hit_speed;
    }
    float e = 0.5f * (a->restitution + b->restitution);
    if (e < 0.0f) {
        e = 0.0f;
    } else if (e > 0.95f) {
        e = 0.95f;
    }
    float j = -(1.0f + e) * vrel * 0.5f;
    a->vx -= nx * j;
    a->vy -= ny * j;
    a->vz -= nz * j;
    b->vx += nx * j;
    b->vy += ny * j;
    b->vz += nz * j;
}

static void sleep_one(MotionBody *bodies, uint32_t count, uint32_t index, float ground, float half) {
    MotionBody *b = &bodies[index];
    if (!b->awake) {
        return;
    }
    float speed = sqrtf(b->vx * b->vx + b->vy * b->vy + b->vz * b->vz);
    float rest = ground + body_rest_lift(b, half);
    if (speed > k_sleep_speed || fabsf(b->z - rest) > k_slop) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (i != index && !separated(b, &bodies[i], half)) {
            return;
        }
    }
    b->vx = 0.0f;
    b->vy = 0.0f;
    b->vz = 0.0f;
    b->z = rest;
    b->awake = false;
}

enum : uint32_t {
    k_avbd_vpre_flag = 0xA5BD0001u,
};

bool motion_prepare_avbd_step(MotionBody     *bodies,
                              uint32_t        count,
                              float           dt,
                              float           ground,
                              float           half,
                              AvbdVertex     *out_verts,
                              uint32_t       *out_vert_count,
                              AvbdConstraint *out_cons,
                              uint32_t       *out_con_count) {
    if (out_vert_count != nullptr) *out_vert_count = 0u;
    if (out_con_count != nullptr)  *out_con_count  = 0u;

    if (bodies == nullptr || out_verts == nullptr || out_vert_count == nullptr ||
        out_cons == nullptr || out_con_count == nullptr ||
        count == 0u || count > K_AVBD_MAX_VERTS || !(dt > 0.0f) || !(half > 0.0f)) {
        return false;
    }

    bool any_awake = false;
    for (uint32_t i = 0u; i < count; ++i) {
        if (bodies[i].awake) {
            any_awake = true;
            break;
        }
    }
    if (!any_awake) {
        return false;
    }

    float      rest_z[K_AVBD_MAX_VERTS];
    float      start_z[K_AVBD_MAX_VERTS];
    float      v_pre[K_AVBD_MAX_VERTS][3];
    MotionBody base[K_AVBD_MAX_VERTS];
    MotionBody pred[K_AVBD_MAX_VERTS];

    for (uint32_t i = 0u; i < count; ++i) {
        rest_z[i]  = ground + body_rest_lift(&bodies[i], half);
        start_z[i] = (bodies[i].awake && bodies[i].z < rest_z[i] && bodies[i].vz >= 0.0f)
            ? rest_z[i]
            : bodies[i].z;
        base[i]    = bodies[i];
        base[i].z  = start_z[i];
        pred[i]    = base[i];
        if (bodies[i].awake) {
            v_pre[i][0] = bodies[i].vx;
            v_pre[i][1] = bodies[i].vy;
            v_pre[i][2] = bodies[i].vz - k_gravity * dt;
            pred[i].x   = base[i].x + v_pre[i][0] * dt;
            pred[i].y   = base[i].y + v_pre[i][1] * dt;
            pred[i].z   = fmaxf(base[i].z + v_pre[i][2] * dt, rest_z[i]);
        } else {
            v_pre[i][0] = 0.0f;
            v_pre[i][1] = 0.0f;
            v_pre[i][2] = 0.0f;
        }
    }

    // Wake up resting bodies contacted by awake bodies without moving bodies[] on CPU
    for (int pass = 0; pass < 4; ++pass) {
        bool woke_any = false;
        for (uint32_t i = 0u; i < count; ++i) {
            for (uint32_t j = i + 1u; j < count; ++j) {
                if (bodies[i].awake == bodies[j].awake) {
                    continue;
                }
                float pen = 0.0f;
                float n[3] = { 0.0f, 0.0f, 0.0f };
                if (body_hit(&pred[i], &pred[j], half, &pen, n) ||
                    body_hit(&base[i], &base[j], half, &pen, n)) {
                    uint32_t sleeper = bodies[i].awake ? j : i;
                    bodies[sleeper].awake = true;
                    base[sleeper].awake   = true;
                    if (start_z[sleeper] < rest_z[sleeper]) {
                        start_z[sleeper] = rest_z[sleeper];
                        base[sleeper].z  = rest_z[sleeper];
                    }
                    v_pre[sleeper][0] = bodies[sleeper].vx;
                    v_pre[sleeper][1] = bodies[sleeper].vy;
                    v_pre[sleeper][2] = bodies[sleeper].vz - k_gravity * dt;
                    pred[sleeper]     = base[sleeper];
                    pred[sleeper].x   = base[sleeper].x + v_pre[sleeper][0] * dt;
                    pred[sleeper].y   = base[sleeper].y + v_pre[sleeper][1] * dt;
                    pred[sleeper].z   = fmaxf(base[sleeper].z + v_pre[sleeper][2] * dt, rest_z[sleeper]);
                    woke_any = true;
                }
            }
        }
        if (!woke_any) {
            break;
        }
    }

    memset(out_verts, 0, (size_t)count * sizeof(AvbdVertex));
    uint32_t con_cnt = 0u;

    for (uint32_t i = 0u; i < count; ++i) {
        out_verts[i].pos[0]      = base[i].x;
        out_verts[i].pos[1]      = base[i].y;
        out_verts[i].pos[2]      = base[i].z;
        out_verts[i].vel[0]      = bodies[i].vx;
        out_verts[i].vel[1]      = bodies[i].vy;
        out_verts[i].vel[2]      = bodies[i].vz;
        out_verts[i].prev_pos[0] = base[i].x;
        out_verts[i].prev_pos[1] = base[i].y;
        out_verts[i].prev_pos[2] = base[i].z;
        out_verts[i].inert_y[0]  = base[i].x + v_pre[i][0] * dt;
        out_verts[i].inert_y[1]  = base[i].y + v_pre[i][1] * dt;
        out_verts[i].inert_y[2]  = base[i].z + v_pre[i][2] * dt;
        out_verts[i].inv_mass    = bodies[i].awake ? 1.0f : 0.0f;
        out_verts[i].radius      = bodies[i].spherical ? (bodies[i].radius > 0.0f ? bodies[i].radius : half) : half;
        out_verts[i].restitution = bodies[i].restitution;
        out_verts[i].flags       = k_avbd_vpre_flag;

        if (bodies[i].awake && con_cnt < K_AVBD_MAX_CONS) {
            out_cons[con_cnt++] = (AvbdConstraint){
                .kind             = AVBD_CON_CONTACT_PLANE,
                .vert_a           = i,
                .vert_b           = UINT32_MAX,
                .hash_key         = 0x10000u | i,
                .normal_or_target = { 0.0f, 0.0f, 1.0f },
                .rest_or_offset   = rest_z[i],
                .stiffness_min    = 1.0e7f,
                .stiffness_max    = 1.0e10f,
                .penalty_k        = 1.0e7f,
                .beta_ramp        = 10.0f,
            };
        }
    }

    for (uint32_t i = 0u; i < count; ++i) {
        for (uint32_t j = i + 1u; j < count; ++j) {
            if (!bodies[i].awake && !bodies[j].awake) {
                continue;
            }
            if (con_cnt >= K_AVBD_MAX_CONS) {
                break;
            }
            float pen = 0.0f;
            float n[3] = { 0.0f, 0.0f, 0.0f };
            const MotionBody *ba = &pred[i];
            const MotionBody *bb = &pred[j];
            bool hit = body_hit(ba, bb, half, &pen, n);
            if (!hit) {
                ba  = &base[i];
                bb  = &base[j];
                hit = body_hit(ba, bb, half, &pen, n);
            }
            if (!hit) {
                continue;
            }

            float ux    = n[0];
            float uy    = n[1];
            float uz    = n[2];
            float d_sep = 0.0f;
            float horiz = hypotf(n[0], n[1]);
            // A pitched face leans; side impacts slide horizontally in xy so neither cube sinks into ground
            if (horiz >= fabsf(n[2]) && horiz > 1.0e-4f) {
                float inv_h = 1.0f / horiz;
                ux    = n[0] * inv_h;
                uy    = n[1] * inv_h;
                uz    = 0.0f;
                d_sep = ux * (bb->x - ba->x) + uy * (bb->y - ba->y) + pen * inv_h;
            } else {
                d_sep = ux * (bb->x - ba->x) + uy * (bb->y - ba->y) + uz * (bb->z - ba->z) + pen;
            }

            out_cons[con_cnt++] = (AvbdConstraint){
                .kind             = AVBD_CON_CONTACT_PAIR,
                .vert_a           = i,
                .vert_b           = j,
                .hash_key         = 0x20000u | (i << 8u) | j,
                .normal_or_target = { ux, uy, uz },
                .rest_or_offset   = d_sep,
                .stiffness_min    = 1.0e7f,
                .stiffness_max    = 1.0e10f,
                .penalty_k        = 1.0e7f,
                .beta_ramp        = 10.0f,
            };
        }
    }

    avbd_warm_apply(&s_motion_warm, out_cons, con_cnt, 0.85f);
    *out_vert_count = count;
    *out_con_count  = con_cnt;
    return true;
}

void motion_apply_solved_avbd(MotionBody           *bodies,
                              uint32_t              count,
                              float                 dt,
                              float                 ground,
                              float                 half,
                              const AvbdVertex     *solved_verts,
                              uint32_t              vert_count,
                              const AvbdConstraint *solved_cons,
                              uint32_t              con_count) {
    if (bodies == nullptr || solved_verts == nullptr || count == 0u ||
        vert_count < count || count > K_AVBD_MAX_VERTS || !(dt > 0.0f) || !(half > 0.0f)) {
        return;
    }

    const uint32_t safe_cons = (solved_cons != nullptr)
        ? (con_count > K_AVBD_MAX_CONS ? K_AVBD_MAX_CONS : con_count)
        : 0u;
    if (safe_cons > 0u) {
        avbd_warm_store(&s_motion_warm, solved_cons, safe_cons);
    }

    float rest_z[K_AVBD_MAX_VERTS];
    const float inv_dt = 1.0f / dt;
    for (uint32_t i = 0u; i < count; ++i) {
        rest_z[i] = ground + body_rest_lift(&bodies[i], half);
        if (!bodies[i].awake) {
            continue;
        }
        bodies[i].x = solved_verts[i].pos[0];
        bodies[i].y = solved_verts[i].pos[1];
        bodies[i].z = fmaxf(solved_verts[i].pos[2], rest_z[i]);
        if ((solved_verts[i].flags & 0xFFFF0000u) == 0xA5BD0000u) {
            bodies[i].vx = (solved_verts[i].inert_y[0] - solved_verts[i].prev_pos[0]) * inv_dt;
            bodies[i].vy = (solved_verts[i].inert_y[1] - solved_verts[i].prev_pos[1]) * inv_dt;
            bodies[i].vz = (solved_verts[i].inert_y[2] - solved_verts[i].prev_pos[2]) * inv_dt;
        } else {
            bodies[i].vx = solved_verts[i].vel[0];
            bodies[i].vy = solved_verts[i].vel[1];
            bodies[i].vz = solved_verts[i].vel[2];
        }
    }

    // Apply momentum & restitution transfer across active AVBD contact pairs
    for (uint32_t c = 0u; c < safe_cons; ++c) {
        if (solved_cons[c].kind != AVBD_CON_CONTACT_PAIR) {
            continue;
        }
        uint32_t va = solved_cons[c].vert_a;
        uint32_t vb = solved_cons[c].vert_b;
        if (va < count && vb < count) {
            trade(&bodies[va], &bodies[vb],
                  solved_cons[c].normal_or_target[0],
                  solved_cons[c].normal_or_target[1],
                  solved_cons[c].normal_or_target[2]);
        }
    }

    // Secondary nonlinear OBB corner polish if rotating multi-body piles expose a new SAT face
    for (int polish = 0; polish < 2; ++polish) {
        for (uint32_t i = 0u; i < count; ++i) {
            for (uint32_t j = i + 1u; j < count; ++j) {
                if (!bodies[i].awake && !bodies[j].awake) {
                    continue;
                }
                float pen = 0.0f;
                float n[3] = { 0.0f, 0.0f, 0.0f };
                if (!body_hit(&bodies[i], &bodies[j], half, &pen, n)) {
                    continue;
                }
                float horiz = hypotf(n[0], n[1]);
                if (horiz >= fabsf(n[2]) && horiz > 1.0e-4f) {
                    float inv_h = 1.0f / horiz;
                    float hx = n[0] * inv_h;
                    float hy = n[1] * inv_h;
                    float shift = pen * 0.5f * inv_h;
                    bodies[i].x -= hx * shift;
                    bodies[i].y -= hy * shift;
                    bodies[j].x += hx * shift;
                    bodies[j].y += hy * shift;
                    trade(&bodies[i], &bodies[j], hx, hy, 0.0f);
                } else {
                    float shift = pen * 0.5f;
                    bodies[i].x -= n[0] * shift;
                    bodies[i].y -= n[1] * shift;
                    bodies[i].z  = fmaxf(bodies[i].z - n[2] * shift, rest_z[i]);
                    bodies[j].x += n[0] * shift;
                    bodies[j].y += n[1] * shift;
                    bodies[j].z  = fmaxf(bodies[j].z + n[2] * shift, rest_z[j]);
                    trade(&bodies[i], &bodies[j], n[0], n[1], n[2]);
                }
            }
        }
    }

    float keep = expf(-k_damp * dt);
    for (uint32_t i = 0u; i < count; ++i) {
        if (!bodies[i].awake) {
            continue;
        }
        resolve_ground_velocity(&bodies[i], rest_z[i]);
        bodies[i].vx *= keep;
        bodies[i].vy *= keep;
        bodies[i].vz *= keep;
    }

    for (uint32_t i = 0u; i < count; ++i) {
        sleep_one(bodies, count, i, ground, half);
    }
}

void motion_substep_avbd(MotionBody     *bodies,
                         uint32_t        count,
                         float           dt,
                         float           ground,
                         float           half,
                         uint32_t        iterations,
                         AvbdVertex     *opt_gpu_verts,
                         uint32_t       *opt_vert_count,
                         AvbdConstraint *opt_gpu_cons,
                         uint32_t       *opt_con_count) {
    if (opt_vert_count != nullptr) *opt_vert_count = 0u;
    if (opt_con_count != nullptr)  *opt_con_count  = 0u;

    AvbdVertex     verts[K_AVBD_MAX_VERTS];
    AvbdConstraint cons[K_AVBD_MAX_CONS];
    uint32_t       vert_cnt = 0u;
    uint32_t       con_cnt  = 0u;

    if (!motion_prepare_avbd_step(bodies, count, dt, ground, half,
                                  verts, &vert_cnt, cons, &con_cnt)) {
        return;
    }

    if (opt_gpu_verts != nullptr && opt_vert_count != nullptr &&
        opt_gpu_cons != nullptr && opt_con_count != nullptr) {
        memcpy(opt_gpu_verts, verts, (size_t)vert_cnt * sizeof(AvbdVertex));
        memcpy(opt_gpu_cons, cons, (size_t)con_cnt * sizeof(AvbdConstraint));
        *opt_vert_count = vert_cnt;
        *opt_con_count  = con_cnt;
    }

    const float    grav[3] = { 0.0f, 0.0f, -k_gravity };
    const uint32_t n_iters = iterations > 0u ? iterations : 4u;
    (void)avbd_solve_step(verts, vert_cnt, cons, con_cnt, dt, grav, n_iters);
    motion_apply_solved_avbd(bodies, vert_cnt, dt, ground, half,
                             verts, vert_cnt, cons, con_cnt);
}

void motion_substep(MotionBody *bodies, uint32_t count, float dt, float ground, float half) {
    motion_substep_avbd(bodies, count, dt, ground, half, 4u, nullptr, nullptr, nullptr, nullptr);
}
