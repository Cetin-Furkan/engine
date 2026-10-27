#ifndef ENGINE_FRAME_PLANE_H
#define ENGINE_FRAME_PLANE_H

#include "vulkan/types.h"

// Universal metric ground: one 6-vertex quad on the true z = 0 plane,
// no vertex buffer. The span is computed from the viewport diagonal and
// zoom (frame/grid_math.h), and the fragment shader draws three nested
// base-10 decades analytically with screen-pixel line widths. The plane
// depth-tests but never writes depth, so geometry centered on z = 0 is
// not sliced by its own ground.

[[nodiscard]] int plane_init(VulkanContext *vk);
void plane_shutdown(VulkanContext *vk);

void plane_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

#endif /* ENGINE_FRAME_PLANE_H */
