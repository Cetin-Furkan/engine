#ifndef ENGINE_FRAME_MESH_H
#define ENGINE_FRAME_MESH_H

#include "ring/ring.h"
#include "vulkan/types.h"
#include <stdint.h>

// Faces from plots/mesh.obj, fitted once into a box beside the cube.
// The vertex shader yaws about Y through that center. L reads the file again.
// A missing file does not fail startup. The chart keeps the OBJ polylines.

[[nodiscard]] int mesh_init(VulkanContext *vk, RingCqeFn on_cqe, void *on_user);
void mesh_shutdown(VulkanContext *vk);

// *has is true when a triangle is stored after the call.
// True when the stored corners changed. A failed read leaves them.
bool mesh_reload(RingCqeFn on_cqe, void *on_user, bool *has);

bool mesh_has_triangles(void);
uint32_t mesh_vertex_count(void);

// Center on z = 0. radius is the fitted half-extent length.
bool mesh_sphere(float *cx, float *cy, float *radius);

// True until the next mesh_commit after a corner change.
bool mesh_wants_frame(void);
void mesh_commit(void);

void mesh_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
               float yaw_deg, bool selected);

#endif /* ENGINE_FRAME_MESH_H */
