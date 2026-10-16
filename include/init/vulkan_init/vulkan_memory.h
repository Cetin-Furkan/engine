#ifndef VULKAN_MEMORY_H
#define VULKAN_MEMORY_H

#include "init/vulkan_init/vulkan_types.h"

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
 *
 * Matches memoryTypeBits and required property flags. Skips lazily allocated and
 * protected types unless those flags were requested. When DEVICE_LOCAL is requested,
 * a non-host-visible type (real VRAM) is preferred over a host-visible ReBAR/UMA type.
 *
 * @param ctx Pointer to the VulkanContext.
 * @param type_filter Bitmask of allowed memory types (from VkMemoryRequirements.memoryTypeBits).
 * @param required_flags Memory property flags required (e.g. VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT).
 * @param out_type_index Pointer where the found memory type index will be written.
 * @return VULKAN_SUCCESS on success, or VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND if no type matches.
 */
[[nodiscard]] VulkanResult vulkan_find_memory_type(
    const VulkanContext   *ctx,
    uint32_t               type_filter,
    VkMemoryPropertyFlags  required_flags,
    uint32_t              *out_type_index);

/**
 * @brief Allocates an exportable VkImage with dedicated memory and exports a Linux DMA-BUF FD.
 *
 * usage is the exact image usage. It is not widened to every feature the format supports.
 * Accepted bits are transfer src/dst, sampled, color attachment, depth/stencil attachment,
 * and storage. The image is a dedicated allocation so the DMA-BUF starts at offset 0.
 * DRM modifier images are limited to one plane.
 *
 * @param ctx Pointer to initialized VulkanContext.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param format Image format (e.g. VK_FORMAT_B8G8R8A8_SRGB or VK_FORMAT_R8G8B8A8_UNORM).
 * @param usage Exact VkImageUsageFlags the image must support.
 * @param tiling Which tiling to try. VULKAN_EXPORT_TILING_AUTO searches modifier, linear, optimal.
 * @param out_image Pointer to VulkanExportableImage struct to populate.
 * @return VULKAN_SUCCESS on success, or a descriptive VulkanResult error code.
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
 * @brief Destroys an exportable image, frees its dedicated memory, and closes its DMA-BUF file descriptor.
 *
 * @param ctx Pointer to initialized VulkanContext.
 * @param image Pointer to the VulkanExportableImage to destroy.
 */
void vulkan_destroy_exportable_image(
    VulkanContext         *ctx,
    VulkanExportableImage *image);

/**
 * @brief Creates the primary graphics command pool.
 *
 * @param ctx Pointer to initialized VulkanContext.
 * @return VULKAN_SUCCESS on success, or VULKAN_ERROR_INITIALIZATION_FAILED on error.
 */
[[nodiscard]] VulkanResult vulkan_command_pool_create(VulkanContext *ctx);

/**
 * @brief Destroys the graphics command pool.
 *
 * @param ctx Pointer to initialized VulkanContext.
 */
void vulkan_command_pool_destroy(VulkanContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_MEMORY_H */