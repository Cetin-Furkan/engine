#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <string.h>

static void da_fail(const char *what, VkResult res) {
    fprintf(stderr, "[BDA Error] %s failed: %s (%d)\n",
            what, vulkan_vkresult_to_str(res), (int)res);
}

int da_buffer_create(VulkanContext *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                     bool host, DaBuffer *out) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || out == nullptr || size == 0) {
        return -1;
    }
    *out = (DaBuffer){0};
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = usage,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &out->buffer);
    if (res != VK_SUCCESS) {
        da_fail("vkCreateBuffer", res);
        *out = (DaBuffer){0};
        return -1;
    }
    VkBufferMemoryRequirementsInfo2 req_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = out->buffer,
    };
    VkMemoryRequirements2 reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vkGetBufferMemoryRequirements2(vk->device, &req_info, &reqs2);
    VkMemoryRequirements reqs = reqs2.memoryRequirements;
    uint32_t type_index = 0;
    VulkanResult mem = VULKAN_ERROR_MEMORY_TYPE_NOT_FOUND;
    if (host) {
        mem = vulkan_find_memory_type(
            vk, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &type_index);
        if (mem != VULKAN_SUCCESS) {
            mem = vulkan_find_memory_type(
                vk, reqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &type_index);
        }
    } else {
        mem = vulkan_find_memory_type(
            vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
        if (mem != VULKAN_SUCCESS) {
            mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
        }
    }
    if (mem != VULKAN_SUCCESS) {
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
        *out = (DaBuffer){0};
        fprintf(stderr, "[BDA Error] No memory type for a %llu byte buffer\n",
                (unsigned long long)size);
        return -1;
    }
    VkMemoryAllocateFlagsInfo alloc_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0
                     ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
                     : 0,
    };
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &alloc_flags,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &out->memory);
    if (res != VK_SUCCESS) {
        da_fail("vkAllocateMemory", res);
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
        *out = (DaBuffer){0};
        return -1;
    }
    VkBindBufferMemoryInfo bind = {
        .sType  = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
        .buffer = out->buffer,
        .memory = out->memory,
    };
    res = vkBindBufferMemory2(vk->device, 1, &bind);
    if (res != VK_SUCCESS) {
        da_fail("vkBindBufferMemory2", res);
        vkFreeMemory(vk->device, out->memory, nullptr);
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
        *out = (DaBuffer){0};
        return -1;
    }
    if (host) {
        VkMemoryMapInfo map_info = {
            .sType  = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
            .memory = out->memory,
            .offset = 0,
            .size   = reqs.size,
        };
        void *mapped = nullptr;
        res = vkMapMemory2(vk->device, &map_info, &mapped);
        if (res != VK_SUCCESS) {
            da_fail("vkMapMemory2", res);
            vkFreeMemory(vk->device, out->memory, nullptr);
            vkDestroyBuffer(vk->device, out->buffer, nullptr);
            *out = (DaBuffer){0};
            return -1;
        }
        out->map = mapped;
    }
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        VkBufferDeviceAddressInfo addr_info = {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = out->buffer,
        };
        out->address = vkGetBufferDeviceAddress(vk->device, &addr_info);
        if (out->address == 0) {
            fprintf(stderr, "[BDA Error] Buffer device address is 0\n");
            da_buffer_destroy(vk, out);
            return -1;
        }
    }
    out->bytes = size;
    return 0;
}

void da_buffer_destroy(VulkanContext *vk, DaBuffer *buf) {
    if (buf == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (buf->map != nullptr && buf->memory != VK_NULL_HANDLE) {
            VkMemoryUnmapInfo unmap = {
                .sType  = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
                .memory = buf->memory,
            };
            vkUnmapMemory2(vk->device, &unmap);
        }
        if (buf->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk->device, buf->buffer, nullptr);
        }
        if (buf->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, buf->memory, nullptr);
        }
    }
    *buf = (DaBuffer){0};
}

int bda_arena_init(VulkanContext *vk, BdaArena *arena, uint32_t capacity) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || arena == nullptr || capacity == 0u) {
        return -1;
    }
    *arena = (BdaArena){0};
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    DaBuffer buf = {0};
    if (da_buffer_create(vk, capacity, usage, true, &buf) != 0 &&
        da_buffer_create(vk, capacity, usage, false, &buf) != 0) {
        fprintf(stderr, "[BDA Error] No memory type for a %u byte arena\n", capacity);
        return -1;
    }
    arena->buffer   = buf.buffer;
    arena->memory   = buf.memory;
    arena->address  = buf.address;
    arena->map      = (uint8_t *)buf.map;
    arena->capacity = capacity;
    arena->offset   = 0;
    return 0;
}

void bda_arena_destroy(VulkanContext *vk, BdaArena *arena) {
    if (arena == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (arena->map != nullptr && arena->memory != VK_NULL_HANDLE) {
            VkMemoryUnmapInfo unmap = {
                .sType  = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
                .memory = arena->memory,
            };
            vkUnmapMemory2(vk->device, &unmap);
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
