#include "frame/fillet.h"
#include <math.h>

float fillet_rad(float deg) {
    return deg * k_fillet_deg_to_rad;
}

void fillet_axes(float yaw_rad, float x[3], float y[3], float z[3]) {
    float cx = cosf(k_fillet_pitch);
    float sx = sinf(k_fillet_pitch);
    float cy = cosf(yaw_rad);
    float sy = sinf(yaw_rad);
    x[0] = cy;
    x[1] = 0.0f;
    x[2] = -sy;
    y[0] = sy * sx;
    y[1] = cx;
    y[2] = cy * sx;
    z[0] = sy * cx;
    z[1] = -sx;
    z[2] = cy * cx;
}

void fillet_place(const float local[3], float yaw_rad, float half, float origin_x, float origin_y,
                  float origin_z, float out[3]) {
    float cx = cosf(k_fillet_pitch);
    float sx = sinf(k_fillet_pitch);
    float rx = local[0];
    float ry = local[1] * cx - local[2] * sx;
    float rz = local[1] * sx + local[2] * cx;
    float cy = cosf(yaw_rad);
    float sy = sinf(yaw_rad);
    out[0] = (rx * cy + rz * sy) * half + origin_x;
    out[1] = ry * half + origin_y;
    out[2] = (-rx * sy + rz * cy) * half + origin_z;
}
