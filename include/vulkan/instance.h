#ifndef ENGINE_VULKAN_INSTANCE_H
#define ENGINE_VULKAN_INSTANCE_H

#include "vulkan/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates a pure Vulkan 1.4 instance with safe layer discovery and debug messenger.
 *
 * Implements:
 * - Safe layer discovery (probes for validation layers, avoids crashing on clean systems).
 * - Safe extension discovery (probes for VK_EXT_debug_utils).
 * - Zero WSI extensions (VK_KHR_surface / VK_KHR_wayland_surface are strictly omitted).
 * - Pre-instance pNext debug hook to catch driver-level initialization errors.
 * - Dynamic loading of debug messenger entry points via vkGetInstanceProcAddr.
 *
 * @param ctx Pointer to the VulkanContext to initialize.
 * @return VULKAN_SUCCESS on success, or a descriptive VulkanResult error code.
 */
[[nodiscard]] VulkanResult vulkan_instance_create(VulkanContext *ctx);

/**
 * @brief Safely destroys the Vulkan instance and attached debug messenger.
 *
 * Enforces strict symmetric LIFO destruction order:
 * 1. Destroys the debug messenger (if active).
 * 2. Destroys the Vulkan instance.
 * 3. Nullifies handles to prevent dangling pointer references.
 *
 * @param ctx Pointer to the VulkanContext to clean up.
 */
void vulkan_instance_destroy(VulkanContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_VULKAN_INSTANCE_H */
