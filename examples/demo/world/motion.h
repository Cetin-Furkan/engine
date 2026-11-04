#ifndef ENGINE_WORLD_MOTION_H
#define ENGINE_WORLD_MOTION_H

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

// One step. dt is seconds. ground is the plane's z. half is 120 for the fillet.
void motion_substep(MotionBody *bodies, uint32_t count, float dt, float ground, float half);

#endif /* ENGINE_WORLD_MOTION_H */
