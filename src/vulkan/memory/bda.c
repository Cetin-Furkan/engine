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
                            VkMemoryPropertyFlags req_flags, float priority,
                            bool host_map, BdaBlock *out) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || out == nullptr || size == 0) {
        return -1;
    }
    *out = (BdaBlock){0};

    // Frame dispatches use the graphics queue. Mesh bakes use the compute
    // queue. Nothing submits the transfer queue, so it is not a sharing member.
    uint32_t queue_families[2];
    uint32_t queue_family_count = 0;
    queue_families[queue_family_count++] = vk->queues.graphics_family_index;
    if (vk->queues.compute_family_index != vk->queues.graphics_family_index) {
        queue_families[queue_family_count++] = vk->queues.compute_family_index;
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
    if (mem_res != VULKAN_SUCCESS && (req_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
        mem_res = vulkan_find_memory_type(
            vk, reqs.memoryTypeBits, req_flags & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &type_index);
    }
    if (mem_res != VULKAN_SUCCESS &&
        (req_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
        (req_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        mem_res = vulkan_find_memory_type(
            vk, reqs.memoryTypeBits, req_flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    }
    if (mem_res != VULKAN_SUCCESS && (req_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        mem_res = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem_res != VULKAN_SUCCESS) {
        fprintf(stderr, "[BDA Error] Failed finding memory type for %llu byte slab\n",
                (unsigned long long)size);
        vkDestroyBuffer(vk->device, out->buffer, nullptr);
        return -1;
    }

    VkMemoryPriorityAllocateInfoEXT prio_info = {0};
    VkMemoryAllocateFlagsInfo alloc_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = vulkan_chain_memory_priority(vk, &prio_info, priority, nullptr),
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
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
        (req_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
        (req_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        // Fallback when 256 MiB PCIe BAR is exhausted: retry in cached/coherent system RAM.
        VkMemoryPropertyFlags sys_flags =
            (req_flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        uint32_t alt_type = 0;
        VulkanResult alt_res = vulkan_find_memory_type(vk, reqs.memoryTypeBits, sys_flags, &alt_type);
        if (alt_res != VULKAN_SUCCESS) {
            alt_res = vulkan_find_memory_type(
                vk, reqs.memoryTypeBits, sys_flags & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &alt_type);
        }
        if (alt_res == VULKAN_SUCCESS && alt_type != type_index) {
            type_index = alt_type;
            alloc.memoryTypeIndex = type_index;
            res = vkAllocateMemory(vk->device, &alloc, nullptr, &out->memory);
        }
    }
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

    VkMemoryPropertyFlags got = vk->device_info.memory_properties.memoryTypes[type_index].propertyFlags;
    bool coherent = (got & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                    (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    // A caller that asked for a map must get one. A device-local slab that
    // landed in coherent memory is mapped too, so the CPU can read it after
    // the acquire wait. A failed optional map leaves the mirror path.
    if (host_map || coherent) {
        VkMemoryMapInfo map_info = {
            .sType  = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
            .memory = out->memory,
            .offset = 0,
            .size   = reqs.size,
        };
        void *mapped = nullptr;
        res = vkMapMemory2(vk->device, &map_info, &mapped);
        if (res != VK_SUCCESS) {
            if (host_map) {
                da_fail("vkMapMemory2 (raw slab)", res);
                vkFreeMemory(vk->device, out->memory, nullptr);
                vkDestroyBuffer(vk->device, out->buffer, nullptr);
                return -1;
            }
        } else {
            out->base_cpu = (uint8_t *)mapped;
        }
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
    s_bda_sys.vk_alloc_count++;
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
        if (s_bda_sys.vk_alloc_count > 0u) {
            s_bda_sys.vk_alloc_count--;
        }
    }
    free(block->free_extra);
    *block = (BdaBlock){0};
}

enum : uint32_t { k_free_inline = 64u };

static BdaRange *block_ranges(BdaBlock *b) {
    return b->free_extra != nullptr ? b->free_extra : b->free_list;
}

static bool usage_fits_slab(VkBufferUsageFlags usage, VkBufferUsageFlags slab) {
    return (usage & ~slab) == 0;
}

static bool free_push(BdaBlock *b, BdaRange range) {
    if (b->free_extra == nullptr) {
        if (b->free_count < k_free_inline) {
            b->free_list[b->free_count++] = range;
            return true;
        }
        uint32_t cap = k_free_inline * 2u;
        BdaRange *extra = malloc((size_t)cap * sizeof(BdaRange));
        if (extra == nullptr) {
            fprintf(stderr, "[BDA Error] Free list cannot grow; %llu bytes not returned\n",
                    (unsigned long long)range.size);
            return false;
        }
        memcpy(extra, b->free_list, sizeof b->free_list);
        b->free_extra = extra;
        b->free_cap = cap;
    } else if (b->free_count >= b->free_cap) {
        uint32_t cap = b->free_cap * 2u;
        BdaRange *extra = realloc(b->free_extra, (size_t)cap * sizeof(BdaRange));
        if (extra == nullptr) {
            fprintf(stderr, "[BDA Error] Free list cannot grow; %llu bytes not returned\n",
                    (unsigned long long)range.size);
            return false;
        }
        b->free_extra = extra;
        b->free_cap = cap;
    }
    b->free_extra[b->free_count++] = range;
    return true;
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
            BdaRange *ranges = block_ranges(b);
            VkDeviceSize cur = (ranges[i].offset + align - 1u) & ~(align - 1u);
            VkDeviceSize pad = cur - ranges[i].offset;
            if (pad + alloc_size <= ranges[i].size) {
                BdaSlice slice = {
                    .cpu    = b->base_cpu != nullptr ? b->base_cpu + cur : nullptr,
                    .gpu    = b->base_gpu != 0 ? b->base_gpu + cur : 0,
                    .bytes  = (uint32_t)alloc_size,
                    .offset = (uint32_t)cur,
                    .buffer = b->buffer,
                };
                VkDeviceSize rem = ranges[i].size - (pad + alloc_size);
                if (pad > 0 && rem > 0) {
                    ranges[i].size = pad;
                    if (!free_push(b, (BdaRange){ cur + alloc_size, rem })) {
                        ranges = block_ranges(b);
                        ranges[i].size = pad + alloc_size + rem;
                        continue;
                    }
                } else if (pad > 0) {
                    ranges[i].size = pad;
                } else if (rem > 0) {
                    ranges[i].offset = cur + alloc_size;
                    ranges[i].size   = rem;
                } else {
                    ranges[i] = ranges[--b->free_count];
                }
                return slice;
            }
        }
        VkDeviceSize cur = (b->allocated + align - 1u) & ~(align - 1u);
        if (cur >= b->allocated && cur + alloc_size <= b->capacity) {
            if (cur > b->allocated) {
                if (!free_push(b, (BdaRange){ b->allocated, cur - b->allocated })) {
                    continue;
                }
            }
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
    if (create_raw_block(vk, slab_cap, pool->usage, pool->mem_flags, pool->priority,
                         pool->is_host_mapped, new_block) != 0) {
        free(new_block);
        return (BdaSlice){0};
    }
    vulkan_name_object(vk, VK_OBJECT_TYPE_BUFFER, (uint64_t)new_block->buffer,
                       pool == &s_bda_sys.staging_pool ? "BDA:StagingSlab" : "BDA:DeviceSlab");

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

    const bool uma = vk->device_info.is_true_uma;
    const VkDeviceSize slab_size = vk->device_info.recommended_bda_slab_mib > 0u
        ? ((VkDeviceSize)vk->device_info.recommended_bda_slab_mib * 1024ull * 1024ull)
        : BDA_DEFAULT_SLAB_SIZE;
    s_bda_sys.vk          = vk;
    s_bda_sys.unified_uma = uma;

    // Device-local high-speed VRAM slab pool (or unified zero-staging LLC pool on UMA iGPUs)
    s_bda_sys.device_pool = (BdaPool){
        .slab_size      = slab_size,
        .usage          = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .mem_flags      = uma ? (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .priority       = 0.75f,
        .is_host_mapped = uma,
    };

    // Host-visible coherent staging RAM slab pool (bypassed on unified UMA)
    s_bda_sys.staging_pool = (BdaPool){
        .slab_size      = slab_size,
        .usage          = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .mem_flags      = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        .priority       = 0.25f,
        .is_host_mapped = true,
    };

    // Per-frame 4-slice transient bump ring (8 MiB on UMA iGPUs = 2 MiB/slice,
    // 16 MiB on discrete GPUs = 4 MiB/slice).
    VkDeviceSize transient_bytes = uma ? (8ull * 1024ull * 1024ull) : (16ull * 1024ull * 1024ull);
    VkBufferUsageFlags transient_usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkMemoryPropertyFlags transient_mem = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                          (uma ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0u);
    (void)create_raw_block(vk, transient_bytes, transient_usage,
                           transient_mem, 0.75f, true, &s_bda_sys.transient_ring);
    vulkan_name_object(vk, VK_OBJECT_TYPE_BUFFER, (uint64_t)s_bda_sys.transient_ring.buffer,
                       "BDA:TransientRing");
    if (s_bda_sys.transient_ring.capacity >= BDA_TRANSIENT_SLICES) {
        VkDeviceSize slice_cap = s_bda_sys.transient_ring.capacity / BDA_TRANSIENT_SLICES;
        for (uint32_t i = 0; i < BDA_TRANSIENT_SLICES; ++i) {
            s_bda_sys.transient_at[i] = slice_cap * (VkDeviceSize)i;
        }
    }

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
    BdaPool *pool = s_bda_sys.unified_uma ? &s_bda_sys.device_pool : &s_bda_sys.staging_pool;
    return alloc_from_pool(vk, pool, size, align);
}

BdaSlice bda_alloc_transient(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align) {
    if (!s_bda_sys.is_initialized) {
        if (bda_system_init(vk) != 0) {
            return (BdaSlice){0};
        }
    }
    if (size == 0u || s_bda_sys.transient_slice_busy) {
        return (BdaSlice){0};
    }
    if (align == 0) {
        align = BDA_CACHE_LINE_ALIGN;
    }
    BdaBlock *ring = &s_bda_sys.transient_ring;
    if (ring->buffer == VK_NULL_HANDLE || ring->capacity < BDA_TRANSIENT_SLICES) {
        fprintf(stderr, "[BDA Error] Transient ring unavailable for %llu byte request\n",
                (unsigned long long)size);
        return (BdaSlice){0};
    }
    uint32_t slot = s_bda_sys.transient_slot & (BDA_TRANSIENT_SLICES - 1u);
    VkDeviceSize slice_cap = ring->capacity / BDA_TRANSIENT_SLICES;
    VkDeviceSize base = slice_cap * (VkDeviceSize)slot;
    VkDeviceSize end  = base + slice_cap;
    VkDeviceSize at   = s_bda_sys.transient_at[slot];
    if (at < base) {
        at = base;
    }
    VkDeviceSize cur = (at + align - 1u) & ~(align - 1u);
    if (cur < at || size > slice_cap || cur + size > end) {
        fprintf(stderr,
                "[BDA Error] Transient slice %u overflow (%llu requested, %llu/%llu used)\n",
                slot,
                (unsigned long long)size,
                (unsigned long long)(at - base),
                (unsigned long long)slice_cap);
        return (BdaSlice){0};
    }
    BdaSlice slice = {
        .cpu    = ring->base_cpu != nullptr ? ring->base_cpu + cur : nullptr,
        .gpu    = ring->base_gpu != 0 ? ring->base_gpu + cur : 0,
        .bytes  = (uint32_t)size,
        .offset = (uint32_t)cur,
        .buffer = ring->buffer,
    };
    s_bda_sys.transient_at[slot] = cur + size;
    return slice;
}

void bda_transient_reset(void) {
    if (!s_bda_sys.is_initialized) {
        return;
    }
    s_bda_sys.transient_slot = 0;
    s_bda_sys.transient_slice_busy = false;
    VkDeviceSize slice_cap = s_bda_sys.transient_ring.capacity / BDA_TRANSIENT_SLICES;
    for (uint32_t i = 0; i < BDA_TRANSIENT_SLICES; ++i) {
        s_bda_sys.transient_at[i]    = slice_cap * (VkDeviceSize)i;
        s_bda_sys.transient_point[i] = 0u;
    }
}

void bda_transient_begin_frame(VkSemaphore acquire, uint64_t signal_point) {
    if (!s_bda_sys.is_initialized) {
        return;
    }
    uint32_t slot = (uint32_t)(signal_point & (BDA_TRANSIENT_SLICES - 1u));
    s_bda_sys.transient_slot = slot;
    uint64_t prev_pt = s_bda_sys.transient_point[slot];
    if (acquire != VK_NULL_HANDLE && prev_pt != 0u &&
        s_bda_sys.vk != nullptr && s_bda_sys.vk->device != VK_NULL_HANDLE) {
        uint64_t done = 0u;
        VkResult r = vkGetSemaphoreCounterValue(s_bda_sys.vk->device, acquire, &done);
        if (r != VK_SUCCESS || done < prev_pt) {
            s_bda_sys.transient_slice_busy = true;
            return;
        }
    }
    s_bda_sys.transient_slice_busy = false;
    VkDeviceSize slice_cap = s_bda_sys.transient_ring.capacity / BDA_TRANSIENT_SLICES;
    s_bda_sys.transient_at[slot]    = slice_cap * (VkDeviceSize)slot;
    s_bda_sys.transient_point[slot] = signal_point;
}

void bda_transient_abort_frame(uint64_t signal_point) {
    if (!s_bda_sys.is_initialized || signal_point == 0u) {
        return;
    }
    uint32_t slot = (uint32_t)(signal_point & (BDA_TRANSIENT_SLICES - 1u));
    if (s_bda_sys.transient_point[slot] == signal_point) {
        s_bda_sys.transient_point[slot] = 0u;
    }
    s_bda_sys.transient_slice_busy = false;
}

static VkDeviceSize pool_allocated_bytes(const BdaPool *pool) {
    VkDeviceSize used = 0;
    for (const BdaBlock *b = pool->blocks; b != nullptr; b = b->next) {
        VkDeviceSize b_used = b->allocated;
        const BdaRange *list = b->free_extra != nullptr ? b->free_extra : b->free_list;
        for (uint32_t i = 0; i < b->free_count; ++i) {
            if (b_used >= list[i].size) {
                b_used -= list[i].size;
            }
        }
        used += b_used;
    }
    return used;
}

void bda_get_stats(BdaStats *out) {
    if (out == nullptr) {
        return;
    }
    *out = (BdaStats){0};
    if (!s_bda_sys.is_initialized) {
        return;
    }
    out->device_allocated     = pool_allocated_bytes(&s_bda_sys.device_pool);
    out->device_capacity      = s_bda_sys.device_pool.total_bytes;
    out->device_blocks        = s_bda_sys.device_pool.block_count;
    out->staging_allocated    = pool_allocated_bytes(&s_bda_sys.staging_pool);
    out->staging_capacity     = s_bda_sys.staging_pool.total_bytes;
    out->staging_blocks       = s_bda_sys.staging_pool.block_count;
    out->transient_capacity   = s_bda_sys.transient_ring.capacity;
    out->standalone_allocated = s_bda_sys.standalone_bytes;
    out->standalone_blocks    = s_bda_sys.standalone_count;
    out->vk_alloc_count       = s_bda_sys.vk_alloc_count;
    if (s_bda_sys.transient_ring.capacity >= BDA_TRANSIENT_SLICES) {
        VkDeviceSize slice_cap = s_bda_sys.transient_ring.capacity / BDA_TRANSIENT_SLICES;
        VkDeviceSize used = 0;
        for (uint32_t i = 0; i < BDA_TRANSIENT_SLICES; ++i) {
            VkDeviceSize base = slice_cap * (VkDeviceSize)i;
            if (s_bda_sys.transient_at[i] > base) {
                VkDeviceSize u = s_bda_sys.transient_at[i] - base;
                used += u <= slice_cap ? u : slice_cap;
            }
        }
        out->transient_used = used;
    }
    if (s_bda_sys.vk != nullptr &&
        s_bda_sys.vk->physical_device != VK_NULL_HANDLE &&
        s_bda_sys.vk->device_info.supports_memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
        };
        VkPhysicalDeviceMemoryProperties2 mem2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
            .pNext = &budget,
        };
        vkGetPhysicalDeviceMemoryProperties2(s_bda_sys.vk->physical_device, &mem2);
        VkDeviceSize heap_usage  = 0u;
        VkDeviceSize heap_budget = 0u;
        for (uint32_t h = 0u; h < mem2.memoryProperties.memoryHeapCount; ++h) {
            if ((mem2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                heap_usage  += budget.heapUsage[h];
                heap_budget += budget.heapBudget[h];
            }
        }
        out->device_heap_usage  = heap_usage;
        out->device_heap_budget = heap_budget;
    }
}

int da_buffer_create(VulkanContext *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                     bool host, DaBuffer *out) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || out == nullptr || size == 0) {
        return -1;
    }
    *out = (DaBuffer){0};

    // A suballocation shares the slab's VkBuffer, so the request has to be
    // a subset of that usage. Anything else gets its own buffer with the
    // flags the caller passed. The slab still carries storage, transfer,
    // index, and indirect for the requests that fit.
    bool usage_fits = false;
    bool use_staging_pool = false;
    if (size <= 16u * 1024u * 1024u) {
        if (!s_bda_sys.is_initialized) {
            if (bda_system_init(vk) != 0) {
                return -1;
            }
        }
        use_staging_pool = host && !s_bda_sys.unified_uma;
        const BdaPool *pool = use_staging_pool ? &s_bda_sys.staging_pool : &s_bda_sys.device_pool;
        usage_fits = usage_fits_slab(usage, pool->usage);
    }
    if (usage_fits) {
        BdaSlice slice = use_staging_pool ? bda_alloc_staging(vk, size, BDA_STORAGE_ALIGN)
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
    const bool uma = vk->device_info.is_true_uma;
    VkMemoryPropertyFlags flags;
    if (uma) {
        flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    } else if (host) {
        // Prefer cached host RAM (GTT) on discrete GPUs so large dedicated host
        // staging buffers do not exhaust a scarce 256 MiB uncached PCIe BAR.
        flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    } else {
        flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }
    float standalone_prio = (host && !uma) ? 0.25f : 0.75f;
    if (create_raw_block(vk, size, usage, flags, standalone_prio, host, &block) != 0) {
        return -1;
    }
    vulkan_name_object(vk, VK_OBJECT_TYPE_BUFFER, (uint64_t)block.buffer, "BDA:StandaloneBuffer");
    out->buffer          = block.buffer;
    out->memory          = block.memory;
    out->map             = block.base_cpu;
    out->address         = block.base_gpu;
    out->bytes           = size;
    out->offset          = 0;
    out->is_suballocated = false;
    s_bda_sys.standalone_bytes += size;
    s_bda_sys.standalone_count++;
    return 0;
}

static bool return_to_pool(BdaPool *pool, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (pool == nullptr || buffer == VK_NULL_HANDLE || size == 0) {
        return false;
    }
    for (BdaBlock *b = pool->blocks; b != nullptr; b = b->next) {
        if (b->buffer != buffer) continue;

        VkDeviceSize end = offset + size;
        bool merged = false;
        BdaRange *ranges = block_ranges(b);

        for (uint32_t i = 0; i < b->free_count; ++i) {
            if (ranges[i].offset + ranges[i].size == offset) {
                ranges[i].size += size;
                offset = ranges[i].offset;
                size = ranges[i].size;
                end = offset + size;
                for (uint32_t j = 0; j < b->free_count; ++j) {
                    if (j != i && end == ranges[j].offset) {
                        ranges[i].size += ranges[j].size;
                        ranges[j] = ranges[--b->free_count];
                        break;
                    }
                }
                merged = true;
                break;
            }
            if (end == ranges[i].offset) {
                ranges[i].offset = offset;
                ranges[i].size += size;
                offset = ranges[i].offset;
                size = ranges[i].size;
                end = offset + size;
                for (uint32_t j = 0; j < b->free_count; ++j) {
                    if (j != i && ranges[j].offset + ranges[j].size == offset) {
                        ranges[i].offset = ranges[j].offset;
                        ranges[i].size += ranges[j].size;
                        ranges[j] = ranges[--b->free_count];
                        break;
                    }
                }
                merged = true;
                break;
            }
        }

        if (!merged) {
            if (offset + size == b->allocated) {
                b->allocated = offset;
            } else if (!free_push(b, (BdaRange){ offset, size })) {
                return true;
            }
        }

        bool shrank = true;
        while (shrank) {
            shrank = false;
            ranges = block_ranges(b);
            for (uint32_t i = 0; i < b->free_count; ++i) {
                if (ranges[i].offset + ranges[i].size == b->allocated) {
                    b->allocated = ranges[i].offset;
                    ranges[i] = ranges[--b->free_count];
                    shrank = true;
                    break;
                }
            }
        }
        return true;
    }
    return false;
}

void da_buffer_destroy(VulkanContext *vk, DaBuffer *buf) {
    if (buf == nullptr) {
        return;
    }
    if (buf->is_suballocated) {
        if (!return_to_pool(&s_bda_sys.device_pool, buf->buffer, buf->offset, buf->bytes)) {
            (void)return_to_pool(&s_bda_sys.staging_pool, buf->buffer, buf->offset, buf->bytes);
        }
    } else if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (buf->buffer != VK_NULL_HANDLE || buf->memory != VK_NULL_HANDLE) {
            if (s_bda_sys.standalone_bytes >= buf->bytes) {
                s_bda_sys.standalone_bytes -= buf->bytes;
            } else {
                s_bda_sys.standalone_bytes = 0u;
            }
            if (s_bda_sys.standalone_count > 0u) {
                s_bda_sys.standalone_count--;
            }
        }
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

int da_buffer_write(DaBuffer *buf, VkDeviceSize offset, const void *src, VkDeviceSize bytes) {
    if (buf == nullptr || buf->map == nullptr || (src == nullptr && bytes != 0u)) {
        return -1;
    }
    if (offset > buf->bytes || bytes > buf->bytes - offset) {
        return -1;
    }
    if (bytes != 0u) {
        memcpy((uint8_t *)buf->map + offset, src, (size_t)bytes);
    }
    return 0;
}

int da_buffer_replace(VulkanContext *vk, DaBuffer *buf, VkDeviceSize size,
                      VkBufferUsageFlags usage, bool host) {
    if (buf == nullptr || size == 0u) {
        return -1;
    }
    DaBuffer next = {0};
    if (da_buffer_create(vk, size, usage, host, &next) != 0) {
        return -1;
    }
    da_buffer_destroy(vk, buf);
    *buf = next;
    return 0;
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
    arena->buffer        = buf.buffer;
    arena->memory        = buf.memory;
    arena->address       = buf.address;
    arena->map           = (uint8_t *)buf.map;
    arena->capacity      = capacity;
    arena->offset        = 0;
    arena->slice_offset  = buf.offset;
    arena->slice_bytes   = buf.bytes;
    arena->suballocated  = buf.is_suballocated;
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
            .bytes           = arena->slice_bytes,
            .offset          = arena->slice_offset,
            .is_suballocated = arena->suballocated,
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
