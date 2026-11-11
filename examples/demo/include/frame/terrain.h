#ifndef ENGINE_FRAME_TERRAIN_H
#define ENGINE_FRAME_TERRAIN_H

#include "vulkan/core.h"
#include <stdint.h>

[[nodiscard]] int  terrain_init(VulkanContext *vk);
void               terrain_shutdown(VulkanContext *vk);
void               terrain_record_compute(VkCommandBuffer cmd, bool live);
void               terrain_draw(VkCommandBuffer cmd,
                                uint32_t        width,
                                uint32_t        height,
                                float           marker_x,
                                float           marker_y,
                                bool            marker_active,
                                float           marker_b_x,
                                float           marker_b_y,
                                bool            marker_b_active);
[[nodiscard]] bool terrain_reload(void);
[[nodiscard]] uint32_t terrain_active_vertices(void);
void               terrain_set_contours(bool enabled);
[[nodiscard]] bool terrain_toggle_contours(void);
[[nodiscard]] bool terrain_contours_enabled(void);

#endif /* ENGINE_FRAME_TERRAIN_H */
