#ifndef ENGINE_WORLD_MOTION_H
#define ENGINE_WORLD_MOTION_H

#include "physics/physics.h"
#include <stdint.h>

// One cube center. yaw is radians about Y, from fillet_axes: the same
// Ry(yaw) * Rx(28°) the fillet draw uses. A body that is not awake keeps
// its center. The ground is the z plane. An awake body rests with its
// oriented box on that plane. Two cubes meet on that box.

typedef struct MotionBody {
    float x;
    float y;
    float z;
    float vx;
    float vy;
    float vz;
    float yaw;
    float restitution;
    float radius;
    float impact;
    bool  spherical;
    bool  awake;
} MotionBody;

// How far the center sits above the ground so the pitched box clears it.
[[nodiscard]] float motion_rest_lift(float yaw, float half);

// Clears across-tick AVBD dual multiplier warm-start cache on world reset.
void motion_reset_warm_cache(void);

// Builds pre-step AvbdVertex and AvbdConstraint arrays (including sleeper wake
// propagation and dual warm-starting) WITHOUT integrating bodies[] positions.
// Returns true when at least one body is awake and out_verts/out_cons are ready.
[[nodiscard]] bool motion_prepare_avbd_step(MotionBody     *bodies,
                                            uint32_t        count,
                                            float           dt,
                                            float           ground,
                                            float           half,
                                            AvbdVertex     *out_verts,
                                            uint32_t       *out_vert_count,
                                            AvbdConstraint *out_cons,
                                            uint32_t       *out_con_count);

// Commits a retired GPU (or CPU) solved AvbdVertex/AvbdConstraint step into
// bodies[], updating positions, elastic restitution impulses, damping, and sleep.
void motion_apply_solved_avbd(MotionBody           *bodies,
                              uint32_t              count,
                              float                 dt,
                              float                 ground,
                              float                 half,
                              const AvbdVertex     *solved_verts,
                              uint32_t              vert_count,
                              const AvbdConstraint *solved_cons,
                              uint32_t              con_count);

// Variational 2026 AVBD CPU substep (CPU-only fallback and standalone tests).
void motion_substep_avbd(MotionBody     *bodies,
                         uint32_t        count,
                         float           dt,
                         float           ground,
                         float           half,
                         uint32_t        iterations,
                         AvbdVertex     *opt_gpu_verts,
                         uint32_t       *opt_vert_count,
                         AvbdConstraint *opt_gpu_cons,
                         uint32_t       *opt_con_count);

// One step. dt is seconds. ground is the plane's z. half is 120 for the fillet.
void motion_substep(MotionBody *bodies, uint32_t count, float dt, float ground, float half);

#endif /* ENGINE_WORLD_MOTION_H */
