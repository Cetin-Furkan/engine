#ifndef ENGINE_FRAME_SOLID_H
#define ENGINE_FRAME_SOLID_H

#include "vulkan/types.h"
#include <stdint.h>

// Six procedural meshes baked once on the GPU and clusterized into 64v/124t
// 16-byte quantized BDA meshlets. Each kind has a faceted cage and a rounded
// cage: 0 tetrahedron, 1 octahedron, 2 icosahedron, 3 cylinder, 4 cone, 5 torus.

[[nodiscard]] int solid_init(VulkanContext *vk);
void solid_shutdown(VulkanContext *vk);

void solid_submit_scene(uint32_t kind, bool curved, float yaw_deg,
                        float origin_x, float origin_y, float origin_z,
                        float squash, bool selected);

// Vertices one raised solid of this kind represents (faceted or rounded cage).
uint32_t solid_vertex_count_for(uint32_t kind, bool curved);

#endif /* ENGINE_FRAME_SOLID_H */
