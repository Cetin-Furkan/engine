#include "frame/pass.h"
#include "vulkan/memory.h"
#include <stdio.h>

static VkFormat              s_depth_format = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_samples = VK_SAMPLE_COUNT_1_BIT;

static void pass_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Pass Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static bool depth_format_ok(VulkanContext *vk, VkFormat format) {
    VkFormatProperties props = {0};
    vkGetPhysicalDeviceFormatProperties(vk->physical_device, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

static VkSampleCountFlags image_sample_counts(VulkanContext *vk, VkFormat format,
                                              VkImageUsageFlags usage) {
    VkImageFormatProperties props = {0};
    VkResult res = vkGetPhysicalDeviceImageFormatProperties(
        vk->physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        usage, 0, &props);
    if (res != VK_SUCCESS) {
        return 0;
    }
    return props.sampleCounts;
}

static VkSampleCountFlagBits pick_samples(VkSampleCountFlags counts) {
    if ((counts & VK_SAMPLE_COUNT_8_BIT) != 0) {
        return VK_SAMPLE_COUNT_8_BIT;
    }
    if ((counts & VK_SAMPLE_COUNT_4_BIT) != 0) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if ((counts & VK_SAMPLE_COUNT_2_BIT) != 0) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

int pass_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_depth_format != VK_FORMAT_UNDEFINED) {
        return 0;
    }
    if (depth_format_ok(vk, VK_FORMAT_D32_SFLOAT)) {
        s_depth_format = VK_FORMAT_D32_SFLOAT;
    } else if (depth_format_ok(vk, VK_FORMAT_D24_UNORM_S8_UINT)) {
        s_depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    } else if (depth_format_ok(vk, VK_FORMAT_D16_UNORM)) {
        s_depth_format = VK_FORMAT_D16_UNORM;
    } else {
        fprintf(stderr, "[Pass Error] No depth attachment format\n");
        return -1;
    }
    // Color and depth must share a count. A resolve and a depth test
    // that disagree are not the same pass.
    VkSampleCountFlags color_counts = image_sample_counts(
        vk, VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VkSampleCountFlags depth_counts = image_sample_counts(
        vk, s_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    s_samples = pick_samples(color_counts & depth_counts);
    return 0;
}

void pass_shutdown(VulkanContext *vk) {
    (void)vk;
    s_depth_format = VK_FORMAT_UNDEFINED;
    s_samples = VK_SAMPLE_COUNT_1_BIT;
}

VkFormat pass_depth_format(void) {
    return s_depth_format;
}

VkSampleCountFlagBits pass_raster_samples(void) {
    return s_samples;
}

void pass_depth_destroy(VulkanContext *vk, PassDepth *depth) {
    if (depth == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (depth->view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, depth->view, nullptr);
        }
        if (depth->image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, depth->image, nullptr);
        }
        if (depth->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, depth->memory, nullptr);
        }
    }
    *depth = (PassDepth){0};
}

int pass_depth_create(VulkanContext *vk, PassDepth *depth, uint32_t width, uint32_t height) {
    if (vk == nullptr || depth == nullptr || width == 0 || height == 0 ||
        s_depth_format == VK_FORMAT_UNDEFINED) {
        return -1;
    }
    pass_depth_destroy(vk, depth);
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = s_depth_format,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = s_samples,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &depth->image);
    if (res != VK_SUCCESS) {
        pass_fail("vkCreateImage depth", res);
        pass_depth_destroy(vk, depth);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetImageMemoryRequirements(vk->device, depth->image, &reqs);
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        fprintf(stderr, "[Pass Error] No memory for the depth image\n");
        pass_depth_destroy(vk, depth);
        return -1;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &depth->memory);
    if (res != VK_SUCCESS) {
        pass_fail("vkAllocateMemory depth", res);
        pass_depth_destroy(vk, depth);
        return -1;
    }
    res = vkBindImageMemory(vk->device, depth->image, depth->memory, 0);
    if (res != VK_SUCCESS) {
        pass_fail("vkBindImageMemory depth", res);
        pass_depth_destroy(vk, depth);
        return -1;
    }
    VkImageViewCreateInfo view = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = depth->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = s_depth_format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    res = vkCreateImageView(vk->device, &view, nullptr, &depth->view);
    if (res != VK_SUCCESS) {
        pass_fail("vkCreateImageView depth", res);
        pass_depth_destroy(vk, depth);
        return -1;
    }
    return 0;
}

void pass_coverage_destroy(VulkanContext *vk, PassCoverage *coverage) {
    if (coverage == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (coverage->view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, coverage->view, nullptr);
        }
        if (coverage->image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, coverage->image, nullptr);
        }
        if (coverage->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, coverage->memory, nullptr);
        }
    }
    *coverage = (PassCoverage){0};
}

int pass_coverage_create(VulkanContext *vk, PassCoverage *coverage,
                         uint32_t width, uint32_t height) {
    if (vk == nullptr || coverage == nullptr || width == 0 || height == 0) {
        return -1;
    }
    pass_coverage_destroy(vk, coverage);
    if (s_samples == VK_SAMPLE_COUNT_1_BIT) {
        return 0;
    }
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_B8G8R8A8_SRGB,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = s_samples,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &coverage->image);
    if (res != VK_SUCCESS) {
        pass_fail("vkCreateImage coverage", res);
        pass_coverage_destroy(vk, coverage);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetImageMemoryRequirements(vk->device, coverage->image, &reqs);
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        fprintf(stderr, "[Pass Error] No memory for the coverage image\n");
        pass_coverage_destroy(vk, coverage);
        return -1;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &coverage->memory);
    if (res != VK_SUCCESS) {
        pass_fail("vkAllocateMemory coverage", res);
        pass_coverage_destroy(vk, coverage);
        return -1;
    }
    res = vkBindImageMemory(vk->device, coverage->image, coverage->memory, 0);
    if (res != VK_SUCCESS) {
        pass_fail("vkBindImageMemory coverage", res);
        pass_coverage_destroy(vk, coverage);
        return -1;
    }
    VkImageViewCreateInfo view = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = coverage->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_B8G8R8A8_SRGB,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    res = vkCreateImageView(vk->device, &view, nullptr, &coverage->view);
    if (res != VK_SUCCESS) {
        pass_fail("vkCreateImageView coverage", res);
        pass_coverage_destroy(vk, coverage);
        return -1;
    }
    return 0;
}

void pass_prepare_draw(VkCommandBuffer cmd, VkImage depth) {
    if (cmd == VK_NULL_HANDLE || depth == VK_NULL_HANDLE) {
        return;
    }
    VkImageMemoryBarrier2 depth_to_attach = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = depth,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &depth_to_attach,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}
