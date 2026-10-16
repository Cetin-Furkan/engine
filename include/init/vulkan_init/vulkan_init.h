#ifndef VULKAN_INIT_H
#define VULKAN_INIT_H

#include "init/vulkan_init/vulkan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the Vulkan 1.4 subsystem and populates the given context.
 *
 * ctx must be zero-initialized, or a context previously passed to
 * vulkan_context_init / vulkan_context_shutdown. A live context is shut down
 * before it is initialized again.
 *
 * @return VULKAN_SUCCESS on success, or a descriptive VulkanResult error code.
 */
[[nodiscard]] VulkanResult vulkan_context_init(VulkanContext *ctx);

/**
 * @brief Safely shuts down the Vulkan subsystem using a symmetric LIFO sequence.
 * @param ctx Pointer to an initialized VulkanContext structure.
 */
void vulkan_context_shutdown(VulkanContext *ctx);

/**
 * @brief Standalone convenience init wrapper.
 */
[[nodiscard]] int vulkan_init(void);

/**
 * @brief Standalone convenience shutdown wrapper.
 */
void vulkan_shutdown(void);

/**
 * @brief Returns a pointer to the active global VulkanContext.
 */
[[nodiscard]] VulkanContext *vulkan_get_context(void);

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_INIT_H */