#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BdaSystem s_bda_sys = {0};

static void da_fail(const char *what, VkResult res) {
    fprintf(stderr, "[BDA Error] %s failed: %s (%d)\n",
            what, vulkan_vkresult_to_str(res), (int)res);
}

static int create_raw_block(VulkanContext *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags req_flags, bool host_map, BdaBlock *out) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || out == nullptr || size == 0) {
        return -1;
    }
    *out = (BdaBlock){0};

    uint32_t queue_families[3];
    uint32_t queue_family_count = 0;
    queue_families[queue_family_count++] = vk->queues.graphics_family_index;
    if (vk->queues.compute_family_index != vk->queues.graphics_family_index) {
        queue_families[queue_family_count++] = vk->queues.compute_family_index;
    }
    if (vk->queues.transfer_family_index != vk->queues.graphics_family_index &&
        vk->queues.transfer_family_index != vk->queues.compute_family_index) {
        queue_families[queue_family_count++] = vk->queues.transfer_family_index;
    }

    VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size                  = size,
        .usage                 = usage,
        .sharingMode           = (queue_family_count > 1) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = (queue_family_count > 1) ? queue_family_count : 0,
        .pQueueFamilyIndices   = (queue_family_count > 1) ? queue_families : nullptr,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &out->buffer);
    if (res != VK_SUCCESS) {
        da_fail("vkCreateBuffer (raw slab)", res);
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
    VulkanResult mem_res = vulkan_find_memory_type(vk, reqs.memoryTypeBits, req_flags, &type_index);
    if (mem_res != VULKAN_SUCCESS && (req_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        mem_res = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem_res != VULKAN_SUCCESS) {
        fprintf(stderr, "[BDA Error] Failed finding memory type for %llu byte slab\n",
                (unsigned long long)size);
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
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
        da_fail("vkAllocateMemory (raw slab)", res);
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
        return -1;
    }

    VkBindBufferMemoryInfo bind = {
        .sType  = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
        .buffer = out->buffer,
        .memory = out->memory,
    };
    res = vkBindBufferMemory2(vk->device, 1, &bind);
    if (res != VK_SUCCESS) {
        da_fail("vkBindBufferMemory2 (raw slab)", res);
        vkFreeMemory(vk->device, out->memory, nullptr);
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
        return -1;
    }

    if (host_map) {
        VkMemoryMapInfo map_info = {
            .sType  = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
            .memory = out->memory,
            .offset = 0,
            .size   = reqs.size,
        };
        void *mapped = nullptr;
        res = vkMapMemory2(vk->device, &map_info, &mapped);
        if (res != VK_SUCCESS) {
            da_fail("vkMapMemory2 (raw slab)", res);
            vkFreeMemory(vk->device, out->memory, nullptr);
            vkDestroyBuffer(vk->device, out->buffer, nullptr);
            return -1;
        }
        out->base_cpu = (uint8_t *)mapped;
    }

    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        VkBufferDeviceAddressInfo addr_info = {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = out->buffer,
        };
        out->base_gpu = vkGetBufferDeviceAddress(vk->device, &addr_info);
    }

    out->capacity  = size;
    out->allocated = 0;
    out->next      = nullptr;
    return 0;
}

static void destroy_raw_block(VulkanContext *vk, BdaBlock *block) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || block == nullptr) {
        return;
    }
    if (block->base_cpu != nullptr && block->memory != VK_NULL_HANDLE) {
        VkMemoryUnmapInfo unmap = {
            .sType  = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
            .memory = block->memory,
        };
        vkUnmapMemory2(vk->device, &unmap);
    }
    if (block->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, block->buffer, nullptr);
    }
    if (block->memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, block->memory, nullptr);
    }
    *block = (BdaBlock){0};
}

static BdaSlice alloc_from_pool(VulkanContext *vk, BdaPool *pool, VkDeviceSize size, VkDeviceSize align) {
    if (pool == nullptr || size == 0) {
        return (BdaSlice){0};
    }
    if (align == 0) {
        align = BDA_CACHE_LINE_ALIGN;
    }
    VkDeviceSize alloc_size = (size + align - 1u) & ~(align - 1u);

    for (BdaBlock *b = pool->blocks; b != nullptr; b = b->next) {
        for (uint32_t i = 0; i < b->free_count; ++i) {
            VkDeviceSize cur = (b->free_list[i].offset + align - 1u) & ~(align - 1u);
            VkDeviceSize pad = cur - b->free_list[i].offset;
            if (pad + alloc_size <= b->free_list[i].size) {
                BdaSlice slice = {
                    .cpu    = b->base_cpu != nullptr ? b->base_cpu + cur : nullptr,
                    .gpu    = b->base_gpu != 0 ? b->base_gpu + cur : 0,
                    .bytes  = (uint32_t)alloc_size,
                    .offset = (uint32_t)cur,
                    .buffer = b->buffer,
                };
                VkDeviceSize rem = b->free_list[i].size - (pad + alloc_size);
                if (pad > 0 && rem > 0) {
                    b->free_list[i].size = pad;
                    if (b->free_count < 64u) {
                        b->free_list[b->free_count++] = (BdaRange){ cur + alloc_size, rem };
                    }
                } else if (pad > 0) {
                    b->free_list[i].size = pad;
                } else if (rem > 0) {
                    b->free_list[i].offset = cur + alloc_size;
                    b->free_list[i].size   = rem;
                } else {
                    b->free_list[i] = b->free_list[--b->free_count];
                }
                return slice;
            }
        }
        VkDeviceSize cur = (b->allocated + align - 1u) & ~(align - 1u);
        if (cur + alloc_size <= b->capacity) {
            BdaSlice slice = {
                .cpu    = b->base_cpu != nullptr ? b->base_cpu + cur : nullptr,
                .gpu    = b->base_gpu != 0 ? b->base_gpu + cur : 0,
                .bytes  = (uint32_t)alloc_size,
                .offset = (uint32_t)cur,
                .buffer = b->buffer,
            };
            b->allocated = cur + alloc_size;
            return slice;
        }
    }

    // Need a new slab block
    VkDeviceSize slab_cap = pool->slab_size > alloc_size ? pool->slab_size : alloc_size * 2u;
    BdaBlock *new_block = calloc(1, sizeof(BdaBlock));
    if (new_block == nullptr) {
        return (BdaSlice){0};
    }
    if (create_raw_block(vk, slab_cap, pool->usage, pool->mem_flags, pool->is_host_mapped, new_block) != 0) {
        free(new_block);
        return (BdaSlice){0};
    }

    new_block->next = pool->blocks;
    pool->blocks = new_block;
    pool->block_count++;
    pool->total_bytes += slab_cap;

    VkDeviceSize cur = (new_block->allocated + align - 1u) & ~(align - 1u);
    BdaSlice slice = {
        .cpu    = new_block->base_cpu != nullptr ? new_block->base_cpu + cur : nullptr,
        .gpu    = new_block->base_gpu != 0 ? new_block->base_gpu + cur : 0,
        .bytes  = (uint32_t)alloc_size,
        .offset = (uint32_t)cur,
        .buffer = new_block->buffer,
    };
    new_block->allocated = cur + alloc_size;
    return slice;
}

int bda_system_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    s_bda_sys = (BdaSystem){0};

    // Device-local high-speed VRAM slab pool
    s_bda_sys.device_pool = (BdaPool){
        .slab_size      = BDA_DEFAULT_SLAB_SIZE,
        .usage          = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .mem_flags      = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .is_host_mapped = false,
    };

    // Host-visible coherent staging RAM slab pool
    s_bda_sys.staging_pool = (BdaPool){
        .slab_size      = BDA_DEFAULT_SLAB_SIZE,
        .usage          = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .mem_flags      = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        .is_host_mapped = true,
    };

    // Per-frame 16 MiB transient bump ring
    VkBufferUsageFlags transient_usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    (void)create_raw_block(vk, 16u * 1024u * 1024u, transient_usage,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           true, &s_bda_sys.transient_ring);

    s_bda_sys.is_initialized = true;
    return 0;
}

void bda_system_shutdown(VulkanContext *vk) {
    if (!s_bda_sys.is_initialized) {
        return;
    }
    destroy_raw_block(vk, &s_bda_sys.transient_ring);

    for (BdaBlock *b = s_bda_sys.device_pool.blocks; b != nullptr;) {
        BdaBlock *next = b->next;
        destroy_raw_block(vk, b);
        free(b);
        b = next;
    }
    for (BdaBlock *b = s_bda_sys.staging_pool.blocks; b != nullptr;) {
        BdaBlock *next = b->next;
        destroy_raw_block(vk, b);
        free(b);
        b = next;
    }
    s_bda_sys = (BdaSystem){0};
}

BdaSlice bda_alloc_device(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align) {
    if (!s_bda_sys.is_initialized) {
        if (bda_system_init(vk) != 0) {
            return (BdaSlice){0};
        }
    }
    return alloc_from_pool(vk, &s_bda_sys.device_pool, size, align);
}

BdaSlice bda_alloc_staging(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align) {
    if (!s_bda_sys.is_initialized) {
        if (bda_system_init(vk) != 0) {
            return (BdaSlice){0};
        }
    }
    return alloc_from_pool(vk, &s_bda_sys.staging_pool, size, align);
}

BdaSlice bda_alloc_transient(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align) {
    if (!s_bda_sys.is_initialized) {
        if (bda_system_init(vk) != 0) {
            return (BdaSlice){0};
        }
    }
    if (align == 0) {
        align = BDA_CACHE_LINE_ALIGN;
    }
    BdaBlock *ring = &s_bda_sys.transient_ring;
    if (ring->buffer == VK_NULL_HANDLE) {
        return bda_alloc_staging(vk, size, align);
    }
    VkDeviceSize cur = (ring->allocated + align - 1u) & ~(align - 1u);
    if (cur + size > ring->capacity) {
        cur = 0;
        if (size > ring->capacity) {
            return bda_alloc_staging(vk, size, align);
        }
    }
    BdaSlice slice = {
        .cpu    = ring->base_cpu != nullptr ? ring->base_cpu + cur : nullptr,
        .gpu    = ring->base_gpu != 0 ? ring->base_gpu + cur : 0,
        .bytes  = (uint32_t)size,
        .offset = (uint32_t)cur,
        .buffer = ring->buffer,
    };
    ring->allocated = cur + size;
    return slice;
}

void bda_transient_reset(void) {
    if (s_bda_sys.is_initialized) {
        s_bda_sys.transient_ring.allocated = 0;
    }
}

int da_buffer_create(VulkanContext *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                     bool host, DaBuffer *out) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || out == nullptr || size == 0) {
        return -1;
    }
    *out = (DaBuffer){0};

    // Suballocate from 2026 BDA pool for standard buffers under 16 MiB
    if (size <= 16u * 1024u * 1024u) {
        if (!s_bda_sys.is_initialized) {
            if (bda_system_init(vk) != 0) {
                return -1;
            }
        }
        BdaSlice slice = host ? bda_alloc_staging(vk, size, BDA_STORAGE_ALIGN)
                              : bda_alloc_device(vk, size, BDA_STORAGE_ALIGN);
        if (slice.buffer != VK_NULL_HANDLE) {
            out->buffer          = slice.buffer;
            out->memory          = VK_NULL_HANDLE; // suballocated from pool
            out->map             = slice.cpu;
            out->address         = slice.gpu;
            out->bytes           = slice.bytes;
            out->offset          = (VkDeviceSize)slice.offset;
            out->is_suballocated = true;
            return 0;
        }
    }

    // Dedicated fallback allocation
    BdaBlock block = {0};
    VkMemoryPropertyFlags flags = host
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (create_raw_block(vk, size, usage, flags, host, &block) != 0) {
        return -1;
    }
    out->buffer          = block.buffer;
    out->memory          = block.memory;
    out->map             = block.base_cpu;
    out->address         = block.base_gpu;
    out->bytes           = size;
    out->offset          = 0;
    out->is_suballocated = false;
    return 0;
}

static void return_to_pool(BdaPool *pool, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (pool == nullptr || buffer == VK_NULL_HANDLE || size == 0) {
        return;
    }
    for (BdaBlock *b = pool->blocks; b != nullptr; b = b->next) {
        if (b->buffer != buffer) continue;

        VkDeviceSize end = offset + size;
        bool merged = false;

        for (uint32_t i = 0; i < b->free_count; ++i) {
            if (b->free_list[i].offset + b->free_list[i].size == offset) {
                b->free_list[i].size += size;
                offset = b->free_list[i].offset;
                size = b->free_list[i].size;
                end = offset + size;
                for (uint32_t j = 0; j < b->free_count; ++j) {
                    if (j != i && end == b->free_list[j].offset) {
                        b->free_list[i].size += b->free_list[j].size;
                        b->free_list[j] = b->free_list[--b->free_count];
                        break;
                    }
                }
                merged = true;
                break;
            }
            if (end == b->free_list[i].offset) {
                b->free_list[i].offset = offset;
                b->free_list[i].size += size;
                offset = b->free_list[i].offset;
                size = b->free_list[i].size;
                end = offset + size;
                for (uint32_t j = 0; j < b->free_count; ++j) {
                    if (j != i && b->free_list[j].offset + b->free_list[j].size == offset) {
                        b->free_list[i].offset = b->free_list[j].offset;
                        b->free_list[i].size += b->free_list[j].size;
                        b->free_list[j] = b->free_list[--b->free_count];
                        break;
                    }
                }
                merged = true;
                break;
            }
        }

        if (!merged && b->free_count < 64u) {
            b->free_list[b->free_count++] = (BdaRange){ offset, size };
        }

        bool shrank = true;
        while (shrank) {
            shrank = false;
            for (uint32_t i = 0; i < b->free_count; ++i) {
                if (b->free_list[i].offset + b->free_list[i].size == b->allocated) {
                    b->allocated = b->free_list[i].offset;
                    b->free_list[i] = b->free_list[--b->free_count];
                    shrank = true;
                    break;
                }
            }
        }
        return;
    }
}

void da_buffer_destroy(VulkanContext *vk, DaBuffer *buf) {
    if (buf == nullptr) {
        return;
    }
    if (buf->is_suballocated) {
        BdaPool *pool = (buf->map != nullptr) ? &s_bda_sys.staging_pool : &s_bda_sys.device_pool;
        return_to_pool(pool, buf->buffer, buf->offset, buf->bytes);
    } else if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        BdaBlock block = {
            .buffer   = buf->buffer,
            .memory   = buf->memory,
            .base_cpu = (uint8_t *)buf->map,
        };
        destroy_raw_block(vk, &block);
    }
    // Poison handle on destruction
    *buf = (DaBuffer){0};
}

int bda_arena_init(VulkanContext *vk, BdaArena *arena, uint32_t capacity, bool host) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || arena == nullptr || capacity == 0u) {
        return -1;
    }
    *arena = (BdaArena){0};
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    DaBuffer buf = {0};
    if (da_buffer_create(vk, capacity, usage, host, &buf) != 0 &&
        da_buffer_create(vk, capacity, usage, !host, &buf) != 0) {
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
    if (arena->buffer != VK_NULL_HANDLE) {
        DaBuffer buf = {
            .buffer          = arena->buffer,
            .memory          = arena->memory,
            .map             = arena->map,
            .is_suballocated = (arena->memory == VK_NULL_HANDLE),
        };
        da_buffer_destroy(vk, &buf);
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
        .cpu    = arena->map != nullptr ? arena->map + aligned : nullptr,
        .gpu    = arena->address + aligned,
        .bytes  = bytes,
        .offset = aligned,
        .buffer = arena->buffer,
    };
    arena->offset = aligned + bytes;
    return slice;
}
