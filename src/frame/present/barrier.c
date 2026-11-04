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

static void fill_color_to_copy(VkImageMemoryBarrier2 *out, VkImage image) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

static void fill_export_as_copy_dst(VkImageMemoryBarrier2 *out, VkImage image) {
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

static void fill_release_from_copy(VkImageMemoryBarrier2 *out, const VulkanContext *vk, VkImage image) {
    uint32_t dst_queue = vk->device_info.supports_queue_family_foreign
        ? VK_QUEUE_FAMILY_FOREIGN_EXT
        : VK_QUEUE_FAMILY_EXTERNAL;
    *out = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask       = VK_ACCESS_2_NONE,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk->queues.graphics_family_index,
        .dstQueueFamilyIndex = dst_queue,
        .image               = image,
        .subresourceRange    = one_level(VK_IMAGE_ASPECT_COLOR_BIT),
    };
}

void frame_cmd_finish_present(VkCommandBuffer cmd, const VulkanContext *vk, const FrameSlot *frame) {
    if (frame->image.prime_copy && frame->prime_image != VK_NULL_HANDLE) {
        VkImageMemoryBarrier2 prepare[2];
        fill_color_to_copy(&prepare[0], frame->prime_image);
        fill_export_as_copy_dst(&prepare[1], frame->image.image);
        frame_cmd_barriers(cmd, prepare, 2);

        VkImageCopy2 region = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .srcSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .dstSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .extent = {
                .width  = frame->image.width,
                .height = frame->image.height,
                .depth  = 1,
            },
        };
        VkCopyImageInfo2 copy = {
            .sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
            .srcImage       = frame->prime_image,
            .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dstImage       = frame->image.image,
            .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .regionCount    = 1,
            .pRegions       = &region,
        };
        vkCmdCopyImage2(cmd, &copy);

        VkImageMemoryBarrier2 release;
        fill_release_from_copy(&release, vk, frame->image.image);
        frame_cmd_barriers(cmd, &release, 1);
        return;
    }

    VkImageMemoryBarrier2 to_general;
    frame_fill_release_foreign(&to_general, vk, frame->image.image);
    frame_cmd_barriers(cmd, &to_general, 1);
}
