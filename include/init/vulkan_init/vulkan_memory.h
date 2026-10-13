#ifndef VULKAN_MEMORY_H
#define VULKAN_MEMORY_H

#include "init/vulkan_init/vulkan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Represents an image allocated with dedicated memory and an exported Linux DMA-BUF FD.
 */
typedef struct VulkanExportableImage {
    VkImage        image;
    VkDeviceMemory memory;
    int            dmabuf_fd;
    uint32_t       width;
    uint32_t       height;
    VkFormat       format;
    VkDeviceSize   allocation_size;
} VulkanExportableImage;

/**
 * @brief Finds a compatible memory type index satisfying type filter and required property flags.
 *
 * Implements Commandment #6: Safe bitwise checking preventing ReBAR and APU heap allocation failures.
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
 * Implements Commandment #7: Enforces VkMemoryDedicatedAllocateInfo so the exported buffer begins
 * at offset 0 of the physical allocation, satisfying Linux kernel DMA-BUF subsystem requirements.
 *
 * @param ctx Pointer to initialized VulkanContext.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param format Image format (e.g. VK_FORMAT_B8G8R8A8_SRGB or VK_FORMAT_R8G8B8A8_UNORM).
 * @param out_image Pointer to VulkanExportableImage struct to populate.
 * @return VULKAN_SUCCESS on success, or a descriptive VulkanResult error code.
 */
[[nodiscard]] VulkanResult vulkan_create_exportable_image(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
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
