#ifndef ENGINE_FRAME_FILLET_H
#define ENGINE_FRAME_FILLET_H

// The fillet the shaders draw: Ry(yaw) * Rx(28°) on a 240 px cube.
// cube.vert, solid.vert, and heat.comp use radians() on those degrees.
// The click walk, the pick slab, and the contact step call these so a
// gap on screen is the same gap the bodies use.

static constexpr float k_fillet_deg_to_rad = 0.017453292519943295f;
static constexpr float k_fillet_pitch      = 28.0f * k_fillet_deg_to_rad;
static constexpr float k_fillet_half       = 120.0f;

[[nodiscard]] float fillet_rad(float deg);

// Columns of Ry(yaw) * Rx(pitch). yaw is radians about Y.
void fillet_axes(float yaw_rad, float x[3], float y[3], float z[3]);

// Mesh-local point, about 1 on a side, into world pixels.
// Matches posed * (k_edge_px * 0.5) + origin in cube.vert.
void fillet_place(const float local[3], float yaw_rad, float half, float origin_x, float origin_y,
                  float origin_z, float out[3]);

#endif /* ENGINE_FRAME_FILLET_H */
