#ifndef ENGINE_VULKAN_DEVICE_H
#define ENGINE_VULKAN_DEVICE_H

#include "vulkan/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Selects a physical device and creates the Vulkan logical device.
 *
 * The frame command buffer is graphics and also records vkCmdDispatch, so the
 * graphics family must include compute. A dedicated compute family is still
 * opened for mesh bakes. Discrete GPUs outrank integrated ones. If logical
 * device creation fails, the next eligible GPU is tried unless ENGINE_GPU_INDEX
 * locked the choice. Wayland present failure can call vulkan_device_use_next.
 */
[[nodiscard]] VulkanResult vulkan_device_create(VulkanContext *ctx);

// Destroys the current logical device and opens the next ranked GPU.
// Command pools must already be destroyed. ENGINE_GPU_INDEX does not fall through.
[[nodiscard]] VulkanResult vulkan_device_use_next(VulkanContext *ctx);

// Prints the queues, features, and DRM nodes of the device that is open.
void vulkan_device_log(const VulkanContext *ctx);

/**
 * @brief Safely destroys the Vulkan logical device.
 *
 * Calls vkDeviceWaitIdle to guarantee all pending GPU work has completed,
 * then destroys the device handle and resets queue references.
 *
 * @param ctx Pointer to the VulkanContext to clean up.
 */
void vulkan_device_destroy(VulkanContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_VULKAN_DEVICE_H */
