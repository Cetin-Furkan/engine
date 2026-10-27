#ifndef ENGINE_FRAME_GIMBAL_H
#define ENGINE_FRAME_GIMBAL_H

#include "vulkan/types.h"
#include <stdint.h>

// A corner triad. It uses the camera quaternion and does not move in screen
// space. Home and numpad 1 snap the camera; that lives on the camera.

[[nodiscard]] int gimbal_init(VulkanContext *vk);
void gimbal_shutdown(VulkanContext *vk);
void gimbal_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

#endif /* ENGINE_FRAME_GIMBAL_H */
