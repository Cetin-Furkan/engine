#ifndef ENGINE_VULKAN_BDA_H
#define ENGINE_VULKAN_BDA_H

#include "vulkan/types.h"
#include <stdint.h>

enum : VkDeviceSize {
    BDA_DEFAULT_SLAB_SIZE = 64u * 1024u * 1024u, // 64 MiB default slab block
    BDA_CACHE_LINE_ALIGN  = 64u,                 // L1 Cache line alignment
    BDA_STORAGE_ALIGN     = 256u,                // Storage buffer alignment
};

typedef struct BdaSlice {
    void           *cpu;
    VkDeviceAddress gpu;
    uint32_t        bytes;
    uint32_t        offset;
    VkBuffer        buffer;
} BdaSlice;

typedef struct BdaArena {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    VkDeviceAddress address;
    uint8_t        *map;
    uint32_t        capacity;
    uint32_t        offset;
} BdaArena;

typedef struct DaBuffer {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    void           *map;
    VkDeviceAddress address;
    VkDeviceSize    bytes;
    VkDeviceSize    offset;
    bool            is_suballocated;
} DaBuffer;

typedef struct BdaBlock {
    VkBuffer         buffer;
    VkDeviceMemory   memory;
    VkDeviceAddress  base_gpu;
    uint8_t         *base_cpu;
    VkDeviceSize     capacity;
    VkDeviceSize     allocated;
    struct BdaBlock *next;
} BdaBlock;

typedef struct BdaPool {
    VkDeviceSize          slab_size;
    VkBufferUsageFlags    usage;
    VkMemoryPropertyFlags mem_flags;
    bool                  is_host_mapped;
    BdaBlock             *blocks;
    uint32_t              block_count;
    VkDeviceSize          total_bytes;
} BdaPool;

typedef struct BdaSystem {
    BdaPool  device_pool;   // Dedicated device-local VRAM
    BdaPool  staging_pool;  // Host-visible coherent RAM
    BdaBlock transient_ring;// Per-frame bump memory
    bool     is_initialized;
} BdaSystem;

// 2026 Unified BDA Slab System API
[[nodiscard]] int bda_system_init(VulkanContext *vk);
void bda_system_shutdown(VulkanContext *vk);
[[nodiscard]] BdaSlice bda_alloc_device(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
[[nodiscard]] BdaSlice bda_alloc_staging(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
[[nodiscard]] BdaSlice bda_alloc_transient(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
void bda_transient_reset(void);

// Compatible wrappers
[[nodiscard]] int da_buffer_create(VulkanContext *vk, VkDeviceSize size,
                                   VkBufferUsageFlags usage, bool host, DaBuffer *out);
void da_buffer_destroy(VulkanContext *vk, DaBuffer *buf);

[[nodiscard]] int bda_arena_init(VulkanContext *vk, BdaArena *arena, uint32_t capacity, bool host);
void bda_arena_destroy(VulkanContext *vk, BdaArena *arena);
void bda_arena_reset(BdaArena *arena);
[[nodiscard]] BdaSlice bda_arena_alloc(BdaArena *arena, uint32_t bytes, uint32_t align);

#endif /* ENGINE_VULKAN_BDA_H */
