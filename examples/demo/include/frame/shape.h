#ifndef ENGINE_FRAME_SHAPE_H
#define ENGINE_FRAME_SHAPE_H

#include "vulkan/types.h"
#include <stdbool.h>

// Polygons are expanded on the GPU. Points are world pixels, +Y up, before
// the in-plane turn. idx has 3 entries per triangle. tri_n 0 draws no fill.
// closed wraps the last edge back to the first. An open chain does not.
// depth is the view bias: larger draws closer.
// shape_begin claims one slice for signal_point. That slice stays until the
// acquire timeline reaches the point, because the next record can run while
// this command buffer is still on the GPU. shape_release gives it back when
// the submit does not happen.

[[nodiscard]] int shape_init(VulkanContext *vk);
void shape_shutdown(VulkanContext *vk);
void shape_begin(VkSemaphore acquire, uint64_t signal_point);
void shape_release(uint64_t signal_point);

void shape_draw_polygon(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                        const float *x, const float *y, uint32_t n,
                        const uint16_t *idx, uint32_t tri_n, bool closed,
                        float center_x, float center_y, float angle_rad,
                        float fill_depth, float edge_depth, float mark_depth,
                        float fill_r, float fill_g, float fill_b,
                        float edge_r, float edge_g, float edge_b,
                        float stroke_half, float mark_half, uint32_t mat_index);

#endif /* ENGINE_FRAME_SHAPE_H */
