#include "world/motion.h"
#include "frame/fillet.h"
#include <math.h>

static constexpr float k_gravity     = 3200.0f;
static constexpr float k_damp        = 1.8f;
// Inelastic. A rebound would leave the faces apart after they had already met.
static constexpr float k_bounce      = 0.0f;
static constexpr float k_sleep_speed = 12.0f;
static constexpr float k_slop        = 0.5f;
static constexpr int   k_passes      = 4;

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

static void clamp_ground(MotionBody *b, float ground, float half) {
    if (!b->awake) {
        return;
    }
    float rest = ground + motion_rest_lift(b->yaw, half);
    if (b->z < rest) {
        b->z = rest;
        if (b->vz < 0.0f) {
            b->vz = 0.0f;
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

static bool separated(const MotionBody *a, const MotionBody *b, float half) {
    float pen = 0.0f;
    float n[3] = { 0.0f, 0.0f, 0.0f };
    return !obb_hit(a, b, half, &pen, n);
}

static void trade(MotionBody *a, MotionBody *b, float nx, float ny, float nz) {
    float vrel = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny + (b->vz - a->vz) * nz;
    if (!(vrel < 0.0f)) {
        return;
    }
    float j = -(1.0f + k_bounce) * vrel * 0.5f;
    a->vx -= nx * j;
    a->vy -= ny * j;
    a->vz -= nz * j;
    b->vx += nx * j;
    b->vy += ny * j;
    b->vz += nz * j;
}

static void collide_pair(MotionBody *a, MotionBody *b, float ground, float half) {
    if (!a->awake && !b->awake) {
        return;
    }
    float pen = 0.0f;
    float n[3] = { 0.0f, 0.0f, 0.0f };
    if (!obb_hit(a, b, half, &pen, n)) {
        return;
    }
    // A still cube stands up when something hits it. Separate after that,
    // at the height the two of them will actually keep.
    a->awake = true;
    b->awake = true;
    clamp_ground(a, ground, half);
    clamp_ground(b, ground, half);
    if (!obb_hit(a, b, half, &pen, n)) {
        return;
    }
    float horiz = sqrtf(n[0] * n[0] + n[1] * n[1]);
    // A pitched face leans. Pushing along that normal sinks one center, and
    // the plane hands the lift back as a shove. A side hit slides in xy by
    // enough to open the same face. A floor hit keeps the vertical push.
    if (horiz >= fabsf(n[2]) && horiz > 1.0e-4f) {
        float inv = 1.0f / horiz;
        float hx = n[0] * inv;
        float hy = n[1] * inv;
        float shift = pen * 0.5f * inv;
        a->x -= hx * shift;
        a->y -= hy * shift;
        b->x += hx * shift;
        b->y += hy * shift;
        trade(a, b, hx, hy, 0.0f);
        return;
    }
    float shift = pen * 0.5f;
    a->x -= n[0] * shift;
    a->y -= n[1] * shift;
    a->z -= n[2] * shift;
    b->x += n[0] * shift;
    b->y += n[1] * shift;
    b->z += n[2] * shift;
    float az = a->z;
    float bz = b->z;
    clamp_ground(a, ground, half);
    clamp_ground(b, ground, half);
    float lift_a = a->z - az;
    float lift_b = b->z - bz;
    b->z += lift_a;
    a->z += lift_b;
    clamp_ground(a, ground, half);
    clamp_ground(b, ground, half);
    trade(a, b, n[0], n[1], n[2]);
}

static void sleep_one(MotionBody *bodies, uint32_t count, uint32_t index, float ground, float half) {
    MotionBody *b = &bodies[index];
    if (!b->awake) {
        return;
    }
    float speed = sqrtf(b->vx * b->vx + b->vy * b->vy + b->vz * b->vz);
    float rest = ground + motion_rest_lift(b->yaw, half);
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

void motion_substep(MotionBody *bodies, uint32_t count, float dt, float ground, float half) {
    if (bodies == nullptr || count == 0 || !(dt > 0.0f) || !(half > 0.0f)) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        MotionBody *b = &bodies[i];
        if (!b->awake) {
            continue;
        }
        b->vz -= k_gravity * dt;
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        b->z += b->vz * dt;
        clamp_ground(b, ground, half);
    }
    for (int pass = 0; pass < k_passes; ++pass) {
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t j = i + 1; j < count; ++j) {
                collide_pair(&bodies[i], &bodies[j], ground, half);
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            clamp_ground(&bodies[i], ground, half);
        }
    }
    float keep = expf(-k_damp * dt);
    for (uint32_t i = 0; i < count; ++i) {
        if (!bodies[i].awake) {
            continue;
        }
        bodies[i].vx *= keep;
        bodies[i].vy *= keep;
        bodies[i].vz *= keep;
    }
    for (uint32_t i = 0; i < count; ++i) {
        sleep_one(bodies, count, i, ground, half);
    }
}
