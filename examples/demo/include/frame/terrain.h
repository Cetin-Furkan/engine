#ifndef ENGINE_FRAME_TERRAIN_H
#define ENGINE_FRAME_TERRAIN_H

#include "vulkan/core.h"
#include <stdint.h>

[[nodiscard]] int  terrain_init(VulkanContext *vk);
void               terrain_shutdown(VulkanContext *vk);
void               terrain_record_compute(VkCommandBuffer cmd);
void               terrain_draw(VkCommandBuffer cmd,
                                uint32_t        width,
                                uint32_t        height,
                                float           marker_x,
                                float           marker_y,
                                bool            marker_active);
[[nodiscard]] bool terrain_reload(void);
[[nodiscard]] uint32_t terrain_active_vertices(void);

#endif /* ENGINE_FRAME_TERRAIN_H */
