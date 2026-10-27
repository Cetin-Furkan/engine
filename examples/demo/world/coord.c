#include "world/coord.h"
#include <math.h>

static int   s_axis = 2;
static float s_offset = 0.0f;
static bool  s_set = false;

static const float k_near_px = 8.0f;

void coord_set_ground(int axis, float offset) {
    s_axis = axis;
    s_offset = offset;
    s_set = true;
}

void coord_clear_ground(void) {
    s_set = false;
    s_axis = 2;
    s_offset = 0.0f;
}

bool coord_ground_set(void) {
    return s_set;
}

static void set_axis(Coord *p, int axis, float v) {
    if (axis == 0) {
        p->x = v;
    } else if (axis == 1) {
        p->y = v;
    } else {
        p->z = v;
    }
}

bool coord_ground_hit(const CameraRay *ray, Coord *out, float *t_hit) {
    if (!s_set || ray == nullptr || out == nullptr) {
        return false;
    }
    float origin = s_axis == 0 ? ray->ox : (s_axis == 1 ? ray->oy : ray->oz);
    float dir = s_axis == 0 ? ray->dx : (s_axis == 1 ? ray->dy : ray->dz);
    Coord hit = { ray->ox, ray->oy, ray->oz };
    float t = 0.0f;
    if (fabsf(dir) <= 1.0e-4f) {
        return false;
    }
    t = (s_offset - origin) / dir;
    if (t < 0.0f) {
        return false;
    }
    hit.x = ray->ox + t * ray->dx;
    hit.y = ray->oy + t * ray->dy;
    hit.z = ray->oz + t * ray->dz;
    set_axis(&hit, s_axis, s_offset);
    *out = hit;
    if (t_hit != nullptr) {
        *t_hit = t;
    }
    return true;
}

float coord_near(void) {
    float z = camera_zoom_shown();
    if (z < 0.04f) {
        z = 0.04f;
    }
    return k_near_px / z;
}

bool coord_sphere_hit(const CameraRay *ray, Coord center, float radius, float *t_hit) {
    if (ray == nullptr || radius < 0.0f) {
        return false;
    }
    float ox = ray->ox - center.x;
    float oy = ray->oy - center.y;
    float oz = ray->oz - center.z;
    float r = radius + coord_near();
    float b = ox * ray->dx + oy * ray->dy + oz * ray->dz;
    float c = ox * ox + oy * oy + oz * oz - r * r;
    float disc = b * b - c;
    if (disc < 0.0f) {
        return false;
    }
    float s = sqrtf(disc);
    float t = -b - s;
    if (t < 0.0f) {
        t = -b + s;
    }
    if (t < 0.0f) {
        return false;
    }
    if (t_hit != nullptr) {
        *t_hit = t;
    }
    return true;
}
