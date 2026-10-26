#ifndef ENGINE_FRAME_SHAPE_H
#define ENGINE_FRAME_SHAPE_H

#include "vulkan/types.h"

// Four corners in the cube's multisample pass. Repeating the third corner
// draws one triangle, which is how a face is filled.
// x and y are pixels from the window center, +Y up, before rotation.
// The corners turn about (center_x, center_y) by angle_rad. They sit on
// the ground plane. depth is the view bias: larger draws closer.

[[nodiscard]] int shape_init(VulkanContext *vk);
void shape_shutdown(VulkanContext *vk);

void shape_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const float x[4], const float y[4],
                float center_x, float center_y, float angle_rad, float depth,
                float r, float g, float b, float a, uint32_t mat_index);

#endif /* ENGINE_FRAME_SHAPE_H */
