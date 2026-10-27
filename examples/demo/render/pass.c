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

enum : uint32_t { k_pool = 16u };
static const VkDeviceSize k_pool_gran = 16ull * 1024ull * 1024ull;

typedef struct PoolSlot {
    VkDeviceMemory memory;
    VkDeviceSize   capacity;
    uint32_t       type_index;
    bool           live;
} PoolSlot;

static PoolSlot s_depth_pool[k_pool];
static PoolSlot s_cover_pool[k_pool];

static void pool_free_all(VulkanContext *vk, PoolSlot *pool) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    for (uint32_t i = 0; i < k_pool; ++i) {
        if (pool[i].memory != VK_NULL_HANDLE && !pool[i].live) {
            vkFreeMemory(vk->device, pool[i].memory, nullptr);
        }
        pool[i] = (PoolSlot){0};
    }
}

static int pool_acquire(VulkanContext *vk, PoolSlot *pool, const VkMemoryRequirements *reqs,
                        VkDeviceMemory *out) {
    for (uint32_t i = 0; i < k_pool; ++i) {
        if (pool[i].live || pool[i].memory == VK_NULL_HANDLE) {
            continue;
        }
        if (pool[i].capacity < reqs->size) {
            continue;
        }
        if ((reqs->memoryTypeBits & (1u << pool[i].type_index)) == 0) {
            continue;
        }
        pool[i].live = true;
        *out = pool[i].memory;
        return 0;
    }
    int free_i = -1;
    for (uint32_t i = 0; i < k_pool; ++i) {
        if (pool[i].memory == VK_NULL_HANDLE) {
            free_i = (int)i;
            break;
        }
    }
    if (free_i < 0) {
        fprintf(stderr, "[Pass Error] Render-target memory pool is full\n");
        return -1;
    }
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs->memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT,
        &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(
            vk, reqs->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs->memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        return -1;
    }
    VkDeviceSize cap = (reqs->size + k_pool_gran - 1ull) & ~(k_pool_gran - 1ull);
    if (cap < reqs->size) {
        cap = reqs->size;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = cap,
        .memoryTypeIndex = type_index,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult res = vkAllocateMemory(vk->device, &alloc, nullptr, &memory);
    if (res != VK_SUCCESS) {
        pass_fail("vkAllocateMemory", res);
        return -1;
    }
    pool[free_i] = (PoolSlot){
        .memory     = memory,
        .capacity   = cap,
        .type_index = type_index,
        .live       = true,
    };
    *out = memory;
    return 0;
}

static void pool_release(PoolSlot *pool, VkDeviceMemory memory) {
    if (memory == VK_NULL_HANDLE) {
        return;
    }
    for (uint32_t i = 0; i < k_pool; ++i) {
        if (pool[i].memory == memory) {
            pool[i].live = false;
            return;
        }
    }
}

void pass_shutdown(VulkanContext *vk) {
    pool_free_all(vk, s_depth_pool);
    pool_free_all(vk, s_cover_pool);
    s_depth_format = VK_FORMAT_UNDEFINED;
    s_samples = VK_SAMPLE_COUNT_1_BIT;
}

VkFormat pass_depth_format(void) {
    return s_depth_format;
}

VkSampleCountFlagBits pass_raster_samples(void) {
    return s_samples;
}

static void release_depth_memory(PassDepth *depth) {
    pool_release(s_depth_pool, depth->memory);
    depth->memory = VK_NULL_HANDLE;
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
        release_depth_memory(depth);
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
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &depth->image);
    if (res != VK_SUCCESS) {
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        res = vkCreateImage(vk->device, &image_info, nullptr, &depth->image);
    }
    if (res != VK_SUCCESS) {
        pass_fail("vkCreateImage depth", res);
        pass_depth_destroy(vk, depth);
        return -1;
    }
    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = depth->image,
    };
    VkMemoryRequirements2 reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetImageMemoryRequirements2(vk->device, &req_info, &reqs2);
    if (pool_acquire(vk, s_depth_pool, &reqs2.memoryRequirements, &depth->memory) != 0) {
        vkDestroyImage(vk->device, depth->image, nullptr);
        depth->image = VK_NULL_HANDLE;
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        res = vkCreateImage(vk->device, &image_info, nullptr, &depth->image);
        if (res != VK_SUCCESS) {
            pass_fail("vkCreateImage depth", res);
            pass_depth_destroy(vk, depth);
            return -1;
        }
        req_info.image = depth->image;
        reqs2 = (VkMemoryRequirements2){ .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        vkGetImageMemoryRequirements2(vk->device, &req_info, &reqs2);
        if (pool_acquire(vk, s_depth_pool, &reqs2.memoryRequirements, &depth->memory) != 0) {
            fprintf(stderr, "[Pass Error] No memory for the depth image\n");
            pass_depth_destroy(vk, depth);
            return -1;
        }
    }
    VkBindImageMemoryInfo bind = {
        .sType  = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image  = depth->image,
        .memory = depth->memory,
    };
    res = vkBindImageMemory2(vk->device, 1, &bind);
    if (res != VK_SUCCESS) {
        pass_fail("vkBindImageMemory2 depth", res);
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
        pool_release(s_cover_pool, coverage->memory);
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
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &coverage->image);
    if (res != VK_SUCCESS) {
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        res = vkCreateImage(vk->device, &image_info, nullptr, &coverage->image);
    }
    if (res != VK_SUCCESS) {
        pass_fail("vkCreateImage coverage", res);
        pass_coverage_destroy(vk, coverage);
        return -1;
    }
    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = coverage->image,
    };
    VkMemoryRequirements2 reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetImageMemoryRequirements2(vk->device, &req_info, &reqs2);
    if (pool_acquire(vk, s_cover_pool, &reqs2.memoryRequirements, &coverage->memory) != 0) {
        vkDestroyImage(vk->device, coverage->image, nullptr);
        coverage->image = VK_NULL_HANDLE;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        res = vkCreateImage(vk->device, &image_info, nullptr, &coverage->image);
        if (res != VK_SUCCESS) {
            pass_fail("vkCreateImage coverage", res);
            pass_coverage_destroy(vk, coverage);
            return -1;
        }
        req_info.image = coverage->image;
        reqs2 = (VkMemoryRequirements2){ .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        vkGetImageMemoryRequirements2(vk->device, &req_info, &reqs2);
        if (pool_acquire(vk, s_cover_pool, &reqs2.memoryRequirements, &coverage->memory) != 0) {
            fprintf(stderr, "[Pass Error] No memory for the coverage image\n");
            pass_coverage_destroy(vk, coverage);
            return -1;
        }
    }
    VkBindImageMemoryInfo bind = {
        .sType  = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image  = coverage->image,
        .memory = coverage->memory,
    };
    res = vkBindImageMemory2(vk->device, 1, &bind);
    if (res != VK_SUCCESS) {
        pass_fail("vkBindImageMemory2 coverage", res);
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


