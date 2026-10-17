#ifndef ENGINE_VULKAN_MEMORY_H
#define ENGINE_VULKAN_MEMORY_H

#include "vulkan/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tiling search used by vulkan_create_exportable_image().
 *
 * AUTO tries a single-plane DRM format modifier, then linear, then optimal.
 * The other values try only that tiling. Optimal has no DRM modifier, so the
 * file descriptor is not a Wayland linux-dmabuf import layout.
 */
typedef enum VulkanExportTiling {
    VULKAN_EXPORT_TILING_AUTO = 0,
    VULKAN_EXPORT_TILING_DRM_MODIFIER,
    VULKAN_EXPORT_TILING_LINEAR,
    VULKAN_EXPORT_TILING_OPTIMAL
} VulkanExportTiling;

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
 * @brief Allocates an exportable VkImage with dedicated memory and exports a Linux DMA-BUF FD.
 */
[[nodiscard]] VulkanResult vulkan_create_exportable_image(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    VulkanExportTiling     tiling,
    VulkanExportableImage *out_image);

/**
 * @brief Allocates a single-plane DRM-modifier exportable VkImage restricted to an explicit modifier list.
 *
 * Intersects the physical device's single-plane exportable modifiers for (format, usage)
 * with allowed_modifiers. The driver may pick any modifier in the list; order is not
 * a preference. When prefer_linear is true and DRM_FORMAT_MOD_LINEAR is in that
 * intersection, the image is created with only that modifier. If it is not, the
 * remaining intersection is used. prefer_linear also tries host-visible memory before
 * device-local memory, then any type in the image's memoryTypeBits.
 */
[[nodiscard]] VulkanResult vulkan_create_exportable_image_with_modifiers(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
    const uint64_t        *allowed_modifiers,
    uint32_t               allowed_modifier_count,
    bool                   prefer_linear,
    VulkanExportableImage *out_image);

/**
 * @brief Allocates a VK_IMAGE_TILING_LINEAR DMA-BUF and reports it as DRM_FORMAT_MOD_LINEAR.
 *
 * Used when the selected GPU and the compositor do not share a tiled modifier.
 * Host-visible memory is tried first so another device can read the buffer.
 * The result has has_drm_format_modifier set and drm_format_modifier 0.
 */
[[nodiscard]] VulkanResult vulkan_create_exportable_linear_dmabuf(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VkImageUsageFlags      usage,
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