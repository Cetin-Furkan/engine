#ifndef ENGINE_WORLD_COORD_H
#define ENGINE_WORLD_COORD_H

#include "frame/camera.h"

// One point in the world. The ground plane named by the world file is the
// zero of the axis it names. Objects keep this point. Hit tests use it.

typedef struct Coord {
    float x;
    float y;
    float z;
} Coord;

// axis is 0, 1, or 2 for X, Y, Z. offset is the plane's coordinate on that axis.
void coord_set_ground(int axis, float offset);
void coord_clear_ground(void);
bool coord_ground_set(void);

// Where the ray crosses the ground. False when the ground was not named,
// or the ray runs along the plane.
bool coord_ground_hit(const CameraRay *ray, Coord *out, float *t_hit);

// Screen pixels of slop, in world pixels, from the shown zoom.
float coord_near(void);

bool coord_sphere_hit(const CameraRay *ray, Coord center, float radius, float *t_hit);

#endif /* ENGINE_WORLD_COORD_H */
