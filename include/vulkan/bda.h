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
    // Bump limit. Stays under 4 GiB because a BdaSlice offset is a uint32.
    uint32_t        capacity;
    uint32_t        offset;
    // The pool reservation. Destroy returns this range, not a zero size.
    VkDeviceSize    slice_offset;
    VkDeviceSize    slice_bytes;
    bool            suballocated;
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

typedef struct BdaRange {
    VkDeviceSize offset;
    VkDeviceSize size;
} BdaRange;

typedef struct BdaBlock {
    VkBuffer         buffer;
    VkDeviceMemory   memory;
    VkDeviceAddress  base_gpu;
    uint8_t         *base_cpu;
    VkDeviceSize     capacity;
    VkDeviceSize     allocated;
    uint32_t         free_count;
    uint32_t         free_cap;     // 0: the inline list holds 64. Else free_extra holds free_cap.
    BdaRange         free_list[64];
    BdaRange        *free_extra;   // Grown when 64 ranges will not coalesce.
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
    BdaBlock transient_ring;// Two frame halves. See bda_transient_begin_frame.
    uint32_t transient_slot;
    VkDeviceSize transient_at[2];
    bool     is_initialized;
} BdaSystem;

typedef struct BdaStats {
    VkDeviceSize device_allocated;
    VkDeviceSize device_capacity;
    VkDeviceSize staging_allocated;
    VkDeviceSize staging_capacity;
    VkDeviceSize transient_used;
    VkDeviceSize transient_capacity;
    uint32_t     device_blocks;
    uint32_t     staging_blocks;
} BdaStats;

// 2026 Unified BDA Slab System API
[[nodiscard]] int bda_system_init(VulkanContext *vk);
void bda_system_shutdown(VulkanContext *vk);
[[nodiscard]] BdaSlice bda_alloc_device(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
[[nodiscard]] BdaSlice bda_alloc_staging(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
[[nodiscard]] BdaSlice bda_alloc_transient(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
void bda_transient_reset(void);
void bda_get_stats(BdaStats *out);

// Rewind one half of the transient bump. signal_point & 1 selects the half.
// Call it after that slot's previous acquire has retired. The other half
// is left for the frame that may still be reading it. An allocation that
// does not fit the half comes from the staging pool and does not wrap.
void bda_transient_begin_frame(uint64_t signal_point);

// Compatible wrappers.
// A fixed buffer is created before the first frame and is not replaced:
// the cube rest mesh, the index buffer, and the heat copies. A dynamic
// buffer is host-mapped and rewritten after boot with da_buffer_write.
// da_buffer_replace builds a new buffer of a different size. The caller
// has to have retired every acquire that still reads the old one.
[[nodiscard]] int da_buffer_create(VulkanContext *vk, VkDeviceSize size,
                                   VkBufferUsageFlags usage, bool host, DaBuffer *out);
void da_buffer_destroy(VulkanContext *vk, DaBuffer *buf);
[[nodiscard]] int da_buffer_write(DaBuffer *buf, VkDeviceSize offset,
                                  const void *src, VkDeviceSize bytes);
[[nodiscard]] int da_buffer_replace(VulkanContext *vk, DaBuffer *buf, VkDeviceSize size,
                                    VkBufferUsageFlags usage, bool host);

[[nodiscard]] int bda_arena_init(VulkanContext *vk, BdaArena *arena, uint32_t capacity, bool host);
void bda_arena_destroy(VulkanContext *vk, BdaArena *arena);
void bda_arena_reset(BdaArena *arena);
[[nodiscard]] BdaSlice bda_arena_alloc(BdaArena *arena, uint32_t bytes, uint32_t align);

#endif /* ENGINE_VULKAN_BDA_H */
