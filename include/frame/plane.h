#ifndef ENGINE_FRAME_PLANE_H
#define ENGINE_FRAME_PLANE_H

#include "vulkan/types.h"

// Infinite drawing plane as a 3-level geometry clipmap. Each level is
// 32 by 32 cells, cell size 16, 64, and 256 world pixels. The vertices
// are gl_VertexIndex plus a snap of the camera, so a cell that leaves
// the window is no longer indexed and a cell that enters is a new index.
// The buffers do not grow, and nothing is allocated per move.
// Coarser levels draw only the ring outside the finer patch.

[[nodiscard]] int plane_init(VulkanContext *vk);
void plane_shutdown(VulkanContext *vk);

void plane_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

#endif /* ENGINE_FRAME_PLANE_H */
