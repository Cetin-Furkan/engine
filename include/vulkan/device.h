#ifndef ENGINE_VULKAN_DEVICE_H
#define ENGINE_VULKAN_DEVICE_H

#include "vulkan/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Selects the optimal physical device and creates the Vulkan logical device.
 *
 * Implements:
 * - Scores discrete above integrated above virtual. Rejects CPU / llvmpipe devices.
 * - Requires a Vulkan 1.4 device, a graphics queue, and a compute queue.
 * - Requires dynamic rendering, synchronization2, buffer device address, and timeline semaphores.
 * - Requires VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf.
 * - Enables DRM-modifier and syncobj extensions only when that device lists them.
 * - Enables maintenance5, maintenance6, and pushDescriptor only when supported.
 * - Prefers a pure transfer family, then another non-graphics transfer family,
 *   then the graphics family.
 * - On a shared family, requests one queue per role when queueCount allows it.
 *   Otherwise the roles share one VkQueue and submission_aliased is set.
 * - Loads extension entry points with vkGetDeviceProcAddr and rejects a null vkGetMemoryFdKHR.
 *
 * @param ctx Pointer to the initialized VulkanContext (instance must be created).
 * @return VULKAN_SUCCESS on success, or a descriptive VulkanResult error code.
 */
[[nodiscard]] VulkanResult vulkan_device_create(VulkanContext *ctx);

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
