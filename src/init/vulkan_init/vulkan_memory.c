#include "init/vulkan_init/vulkan_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

VulkanResult vulkan_find_memory_type(
    const VulkanContext   *ctx,
    uint32_t               type_filter,
    VkMemoryPropertyFlags  required_flags,
    uint32_t              *out_type_index
) {
    if (ctx == nullptr || out_type_index == nullptr) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    const VkPhysicalDeviceMemoryProperties *mem_props = &ctx->device_info.memory_properties;

    // Bitwise search across available hardware memory types
    for (uint32_t i = 0; i < mem_props->memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props->memoryTypes[i].propertyFlags & required_flags) == required_flags) {
            *out_type_index = i;
            return VULKAN_SUCCESS;
        }
    }

    return VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND;
}

VulkanResult vulkan_create_exportable_image(
    VulkanContext         *ctx,
    uint32_t               width,
    uint32_t               height,
    VkFormat               format,
    VulkanExportableImage *out_image
) {
    if (ctx == nullptr || out_image == nullptr || width == 0 || height == 0 || ctx->device == VK_NULL_HANDLE) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    *out_image = (VulkanExportableImage){.dmabuf_fd = -1};

    // 1. External memory image configuration for Linux DMA-BUF export
    VkExternalMemoryImageCreateInfo ext_image_info = {
        .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext       = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = &ext_image_info,
        .flags         = 0,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = {.width = width, .height = height, .depth = 1},
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_LINEAR,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image = VK_NULL_HANDLE;
    VkResult img_res = vkCreateImage(ctx->device, &image_info, nullptr, &image);
    if (img_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateImage for exportable DMA-BUF failed with code %d\n", img_res);
        return VULKAN_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // 2. Query exact memory requirements
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(ctx->device, image, &mem_reqs);

    // 3. Locate compatible device-local VRAM memory type
    uint32_t memory_type_index = 0;
    VulkanResult mem_type_res = vulkan_find_memory_type(
        ctx,
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &memory_type_index);

    if (mem_type_res != VULKAN_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] Could not find device-local memory type for DMA-BUF image\n");
        vkDestroyImage(ctx->device, image, nullptr);
        return mem_type_res;
    }

    // 4. Configure dedicated allocation with DMA-BUF export handle
    // Commandment #7: Dedicated allocation ensures offset 0 requirement for Linux DMA-BUF
    VkExportMemoryAllocateInfo export_info = {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext       = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext  = &export_info,
        .image  = image,
        .buffer = VK_NULL_HANDLE,
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &dedicated_info,
        .allocationSize  = mem_reqs.size,
        .memoryTypeIndex = memory_type_index,
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult alloc_res = vkAllocateMemory(ctx->device, &alloc_info, nullptr, &memory);
    if (alloc_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkAllocateMemory for DMA-BUF image failed with code %d\n", alloc_res);
        vkDestroyImage(ctx->device, image, nullptr);
        return VULKAN_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    // 5. Bind dedicated memory to image at offset 0
    VkResult bind_res = vkBindImageMemory(ctx->device, image, memory, 0);
    if (bind_res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkBindImageMemory failed with code %d\n", bind_res);
        vkFreeMemory(ctx->device, memory, nullptr);
        vkDestroyImage(ctx->device, image, nullptr);
        return VULKAN_ERROR_INITIALIZATION_FAILED;
    }

    // 6. Export Linux DMA-BUF file descriptor
    int dmabuf_fd = -1;
    if (ctx->dispatch.vkGetMemoryFdKHR != nullptr) {
        VkMemoryGetFdInfoKHR get_fd_info = {
            .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .pNext      = nullptr,
            .memory     = memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkResult fd_res = ctx->dispatch.vkGetMemoryFdKHR(ctx->device, &get_fd_info, &dmabuf_fd);
        if (fd_res != VK_SUCCESS || dmabuf_fd < 0) {
            fprintf(stderr, "[Vulkan Error] vkGetMemoryFdKHR failed with code %d\n", fd_res);
            vkFreeMemory(ctx->device, memory, nullptr);
            vkDestroyImage(ctx->device, image, nullptr);
            return VULKAN_ERROR_FUNCTION_POINTER_MISSING;
        }
    } else {
        fprintf(stderr, "[Vulkan Error] vkGetMemoryFdKHR dispatch pointer is null\n");
        vkFreeMemory(ctx->device, memory, nullptr);
        vkDestroyImage(ctx->device, image, nullptr);
        return VULKAN_ERROR_FUNCTION_POINTER_MISSING;
    }

    out_image->image           = image;
    out_image->memory          = memory;
    out_image->dmabuf_fd       = dmabuf_fd;
    out_image->width           = width;
    out_image->height          = height;
    out_image->format          = format;
    out_image->allocation_size = mem_reqs.size;

    return VULKAN_SUCCESS;
}

void vulkan_destroy_exportable_image(
    VulkanContext         *ctx,
    VulkanExportableImage *image
) {
    if (ctx == nullptr || image == nullptr) {
        return;
    }

    if (image->dmabuf_fd >= 0) {
        close(image->dmabuf_fd);
        image->dmabuf_fd = -1;
    }

    if (ctx->device != VK_NULL_HANDLE) {
        if (image->image != VK_NULL_HANDLE) {
            vkDestroyImage(ctx->device, image->image, nullptr);
            image->image = VK_NULL_HANDLE;
        }

        if (image->memory != VK_NULL_HANDLE) {
            vkFreeMemory(ctx->device, image->memory, nullptr);
            image->memory = VK_NULL_HANDLE;
        }
    }
}

VulkanResult vulkan_command_pool_create(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE) {
        return VULKAN_ERROR_INVALID_ARGUMENT;
    }

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->queues.graphics_family_index,
    };

    VkResult res = vkCreateCommandPool(ctx->device, &pool_info, nullptr, &ctx->cmd_pool);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan Error] vkCreateCommandPool failed with code %d\n", res);
        return VULKAN_ERROR_INITIALIZATION_FAILED;
    }

    return VULKAN_SUCCESS;
}

void vulkan_command_pool_destroy(VulkanContext *ctx) {
    if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->cmd_pool == VK_NULL_HANDLE) {
        return;
    }

    vkDestroyCommandPool(ctx->device, ctx->cmd_pool, nullptr);
    ctx->cmd_pool = VK_NULL_HANDLE;
}
