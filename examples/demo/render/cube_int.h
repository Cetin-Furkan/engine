#ifndef ENGINE_DEMO_CUBE_INT_H
#define ENGINE_DEMO_CUBE_INT_H

#include <stdint.h>

// Shared by the fillet draw and the CPU pick. Counts match
// shaders/face_grid.slang and the bake in render/cube.c.

enum : uint32_t {
    k_pick_verts      = 3858u,
    k_pick_grid_verts = 2598u,
    k_pick_indices    = 21504u,
};

// mesh_bytes is the baked MeshVertex array, 36 bytes per vertex.
// indices is k_pick_indices uint16 values. False leaves the pick unready.
bool cube_pick_store_rest(const void *mesh_bytes, const uint16_t *indices);
void cube_pick_reset(void);

// The displacement the vertex shader would read for ids below the seam.
// k_pick_grid_verts * 3 floats. Zeros until a heat step's acquire has retired.
void cube_heat_shown_disp(uint32_t slot, float *xyz);

#endif /* ENGINE_DEMO_CUBE_INT_H */
