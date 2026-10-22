#ifndef ENGINE_FRAME_SHAPE_H
#define ENGINE_FRAME_SHAPE_H

#include "vulkan/types.h"

// A filled convex quad in the cube's multisample pass.
// x and y are pixels from the window center, +Y up, before rotation.
// The quad turns about (center_x, center_y) by angle_rad. Depth is the
// clip-space z written for every vertex. Smaller is closer.

[[nodiscard]] int shape_init(VulkanContext *vk);
void shape_shutdown(VulkanContext *vk);

void shape_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const float x[4], const float y[4],
                float center_x, float center_y, float angle_rad, float depth,
                float r, float g, float b, float a);

#endif /* ENGINE_FRAME_SHAPE_H */
