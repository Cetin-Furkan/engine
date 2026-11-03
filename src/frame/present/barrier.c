#include "frame/internal.h"

// Sync2 barriers for a DMA-BUF that is cleared and then handed to the
// compositor. oldLayout UNDEFINED has nothing to make available, so the
// source stage is NONE. The foreign-queue release is published by the
// timeline signal at the end of the command buffer, so the destination
// stage is NONE as well.

static VkImageSubresourceRange one_level(VkImageAspectFlags aspect) {
    return (VkImageSubresourceRange){
        .aspectMask     = aspect,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
}

void frame_fill_discard_color(VkImageMemoryBarrier2 *out, VkImage image) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

void frame_fill_discard_depth(VkImageMemoryBarrier2 *out, VkImage image) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_DEPTH_BIT),
    };
}

void frame_fill_release_foreign(VkImageMemoryBarrier2 *out, const VulkanContext *vk, VkImage image) {
    uint32_t dst_queue = vk->device_info.supports_queue_family_foreign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask       = VK_ACCESS_2_NONE,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk->queues.graphics_family_index,
        .dstQueueFamilyIndex = dst_queue,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

void frame_cmd_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *barriers, uint32_t count) {
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = count,
        .pImageMemoryBarriers    = barriers,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}
