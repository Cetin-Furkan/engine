#ifndef ENGINE_VULKAN_MEMORY_H
#define ENGINE_VULKAN_MEMORY_H

#include "vulkan/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Represents an image allocated with dedicated memory and an exported Linux DMA-BUF FD.
 */
typedef struct VulkanExportableImage {
    VkImage           image;
    VkDeviceMemory    memory;
    int               dmabuf_fd;
    uint32_t          width;
    uint32_t          height;
    VkFormat          format;
    VkImageUsageFlags usage;
    VkImageTiling     tiling;
    uint64_t          drm_format_modifier;
    bool              has_drm_format_modifier;
    // The image was created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and the
    // UNORM/sRGB view list, so a B8G8R8A8_SRGB view of a UNORM image is legal.
    bool              mutable_bgra8;
    // The display GPU is not this device. The image is a transfer target.
    // The frame renders into a device-local image and copies into this one.
    bool              prime_copy;
    VkDeviceSize      allocation_size;
    VkDeviceSize      offset;
    VkDeviceSize      row_pitch;
} VulkanExportableImage;

/**
 * @brief Finds a compatible memory type index satisfying type filter and required property flags.
 */
[[nodiscard]] VulkanResult vulkan_find_memory_type(
    const VulkanContext   *ctx,
    uint32_t               type_filter,
    VkMemoryPropertyFlags  required_flags,
    uint32_t              *out_type_index);

/**
 * @brief Allocates a single-plane DRM-modifier exportable VkImage restricted to an explicit modifier list.
 *
 * Intersects the physical device's single-plane exportable modifiers for (format, usage)
 * with allowed_modifiers. Rejects optimal tiling (no modifier) and any chosen modifier
 * not present in allowed_modifiers. foreign_read places the buffer in memory another
 * GPU can DMA, which is required when the compositor's device is not this one.
 */
[[nodiscard]] VulkanResult vulkan_create_exportable_image_with_modifiers(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    const uint64_t        *allowed_modifiers,
    uint32_t               allowed_modifier_count,
    bool                   foreign_read,
    VulkanExportableImage *out_image);

/**
 * @brief Destroys an exportable image, frees its dedicated memory, and closes its DMA-BUF file descriptor.
 */
void vulkan_destroy_exportable_image(
    VulkanContext         *ctx,
    VulkanExportableImage *image);

/**
 * @brief Creates the primary graphics command pool.
 */
[[nodiscard]] VulkanResult vulkan_command_pool_create(VulkanContext *ctx);

/**
 * @brief Destroys the graphics command pool.
 */
void vulkan_command_pool_destroy(VulkanContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_VULKAN_MEMORY_H */