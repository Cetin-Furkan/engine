#ifndef ENGINE_FRAME_GIMBAL_H
#define ENGINE_FRAME_GIMBAL_H

#include "vulkan/types.h"
#include <stdint.h>

// View ball in the top-right corner. Pixel size stays put. It turns with
// the camera quaternion. Numpad 7 shows or hides it. It starts shown.
// Hiding it does not move the camera, so gimbal_busy keeps a frame armed
// until the hidden picture has been presented.

[[nodiscard]] int gimbal_init(VulkanContext *vk);
void gimbal_shutdown(VulkanContext *vk);
void gimbal_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

bool gimbal_toggle(void);
bool gimbal_busy(void);
void gimbal_commit(void);

#endif /* ENGINE_FRAME_GIMBAL_H */
