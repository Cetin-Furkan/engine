#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <string.h>

int bda_arena_init(VulkanContext *vk, BdaArena *arena, uint32_t capacity) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || arena == nullptr || capacity == 0u) {
        return -1;
    }
    *arena = (BdaArena){0};
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = capacity,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &arena->buffer);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[BDA Error] vkCreateBuffer failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetBufferMemoryRequirements(vk->device, arena->buffer, &reqs);
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(
            vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(
            vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        vkDestroyBuffer(vk->device, arena->buffer, nullptr);
        *arena = (BdaArena){0};
        fprintf(stderr, "[BDA Error] No memory type for a %u byte arena\n", capacity);
        return -1;
    }
    VkMemoryAllocateFlagsInfo alloc_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &alloc_flags,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &arena->memory);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[BDA Error] vkAllocateMemory failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        vkDestroyBuffer(vk->device, arena->buffer, nullptr);
        *arena = (BdaArena){0};
        return -1;
    }
    res = vkBindBufferMemory(vk->device, arena->buffer, arena->memory, 0);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[BDA Error] vkBindBufferMemory failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        vkFreeMemory(vk->device, arena->memory, nullptr);
        vkDestroyBuffer(vk->device, arena->buffer, nullptr);
        *arena = (BdaArena){0};
        return -1;
    }
    VkBufferDeviceAddressInfo addr_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = arena->buffer,
    };
    arena->address  = vkGetBufferDeviceAddress(vk->device, &addr_info);
    arena->capacity = capacity;
    arena->offset   = 0;
    if (arena->address == 0) {
        fprintf(stderr, "[BDA Error] Buffer device address is 0\n");
        bda_arena_destroy(vk, arena);
        return -1;
    }
    void *mapped = nullptr;
    if (vkMapMemory(vk->device, arena->memory, 0, reqs.size, 0, &mapped) == VK_SUCCESS) {
        arena->map = (uint8_t *)mapped;
    }
    return 0;
}

void bda_arena_destroy(VulkanContext *vk, BdaArena *arena) {
    if (arena == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (arena->map != nullptr && arena->memory != VK_NULL_HANDLE) {
            vkUnmapMemory(vk->device, arena->memory);
        }
        if (arena->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk->device, arena->buffer, nullptr);
        }
        if (arena->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, arena->memory, nullptr);
        }
    }
    *arena = (BdaArena){0};
}

void bda_arena_reset(BdaArena *arena) {
    if (arena != nullptr) {
        arena->offset = 0;
    }
}

BdaSlice bda_arena_alloc(BdaArena *arena, uint32_t bytes, uint32_t align) {
    if (arena == nullptr || arena->address == 0 || bytes == 0u) {
        return (BdaSlice){0};
    }
    if (align == 0u) {
        align = 1u;
    }
    uint32_t aligned = (arena->offset + align - 1u) & ~(align - 1u);
    if (aligned < arena->offset || aligned + bytes < aligned || aligned + bytes > arena->capacity) {
        fprintf(stderr, "[BDA Error] Arena overflow (%u requested, %u/%u used)\n",
                bytes, arena->offset, arena->capacity);
        return (BdaSlice){0};
    }
    BdaSlice slice = {
        .cpu   = arena->map != nullptr ? arena->map + aligned : nullptr,
        .gpu   = arena->address + aligned,
        .bytes = bytes,
    };
    arena->offset = aligned + bytes;
    return slice;
}
