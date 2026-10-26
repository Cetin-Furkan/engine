#ifndef ENGINE_FRAME_SOLID_H
#define ENGINE_FRAME_SOLID_H

#include "vulkan/types.h"
#include <stdint.h>

// Six meshes baked once on the GPU. Each has a faceted cage and a rounded
// cage. kind is 0 tetrahedron, 1 octahedron, 2 icosahedron, 3 cylinder,
// 4 cone, 5 torus. The draw reads the same 9-float vertex record as the cube
// and does not sample the fillet temperature.

[[nodiscard]] int solid_init(VulkanContext *vk);
void solid_shutdown(VulkanContext *vk);

void solid_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                uint32_t kind, bool curved, float yaw_deg,
                float origin_x, float origin_y, bool selected);

uint32_t solid_vertex_count(void);

#endif /* ENGINE_FRAME_SOLID_H */
