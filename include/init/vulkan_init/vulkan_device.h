#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include "init/vulkan_init/vulkan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Selects the optimal physical device and creates the Vulkan logical device.
 *
 * Implements:
 * - Multi-factor GPU scoring (Discrete > Integrated > Virtual/CPU).
 * - Vulkan 1.4 baseline enforcement on candidate physical devices.
 * - Dynamic queue family discovery (finds Graphics, Compute, and Transfer families).
 * - Safe queue creation with explicit 1.0f priority and family de-duplication.
 * - Pre-creation hardware capability queries via vkGetPhysicalDeviceFeatures2.
 * - Modern feature chaining via pNext (dynamicRendering, synchronization2, BDA, timelineSemaphore).
 * - Probing and activation of Linux DMA-BUF and DRM syncobj extensions.
 * - Dynamic resolution of device-level extension function pointers.
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

#endif /* VULKAN_DEVICE_H */
