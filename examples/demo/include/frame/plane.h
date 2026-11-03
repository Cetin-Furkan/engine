#ifndef ENGINE_FRAME_PLANE_H
#define ENGINE_FRAME_PLANE_H

#include "vulkan/types.h"

// Universal metric backdrop: one fullscreen triangle, no vertex buffer.
// The fragment shader draws three nested base-10 decades in screen space
// (frame/grid_math.h chooses the decade). The sheet ignores yaw and pitch,
// fills the view at every zoom the camera allows, and neither tests nor
// writes depth. Scene draws after it and covers it. The z = 0 hit plane
// stays in world/coord.h.

[[nodiscard]] int plane_init(VulkanContext *vk);
void plane_shutdown(VulkanContext *vk);

void plane_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

#endif /* ENGINE_FRAME_PLANE_H */
