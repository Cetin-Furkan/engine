#ifndef ENGINE_VULKAN_BDA_H
#define ENGINE_VULKAN_BDA_H

#include "vulkan/types.h"
#include <stdint.h>

enum : VkDeviceSize {
    BDA_DEFAULT_SLAB_SIZE = 64u * 1024u * 1024u, // 64 MiB default slab block
    BDA_CACHE_LINE_ALIGN  = 64u,                 // L1 Cache line alignment
    BDA_STORAGE_ALIGN     = 256u,                // Storage buffer alignment
};

enum : uint32_t {
    BDA_TRANSIENT_SLICES = 4u,                   // 4 timeline-guarded frame slices
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
    float                 priority;
    bool                  is_host_mapped;
    BdaBlock             *blocks;
    uint32_t              block_count;
    VkDeviceSize          total_bytes;
} BdaPool;

typedef struct BdaSystem {
    VulkanContext *vk;
    BdaPool        device_pool;   // Dedicated device-local VRAM (or unified LLC UMA pool)
    BdaPool        staging_pool;  // Host-visible coherent RAM (unused when unified_uma is true)
    BdaBlock       transient_ring;// Four timeline-guarded slices. See bda_transient_begin_frame.
    uint32_t       transient_slot;
    VkDeviceSize   transient_at[BDA_TRANSIENT_SLICES];
    uint64_t       transient_point[BDA_TRANSIENT_SLICES];
    VkDeviceSize   standalone_bytes;
    uint32_t       standalone_count;
    uint32_t       vk_alloc_count;
    bool           transient_slice_busy;
    bool           unified_uma;
    bool           is_initialized;
} BdaSystem;

typedef struct BdaStats {
    VkDeviceSize device_allocated;
    VkDeviceSize device_capacity;
    VkDeviceSize staging_allocated;
    VkDeviceSize staging_capacity;
    VkDeviceSize transient_used;
    VkDeviceSize transient_capacity;
    VkDeviceSize standalone_allocated;
    VkDeviceSize device_heap_usage;
    VkDeviceSize device_heap_budget;
    uint32_t     device_blocks;
    uint32_t     staging_blocks;
    uint32_t     standalone_blocks;
    uint32_t     vk_alloc_count;
} BdaStats;

// 2026 Unified BDA Slab System API
[[nodiscard]] int bda_system_init(VulkanContext *vk);
void bda_system_shutdown(VulkanContext *vk);
[[nodiscard]] BdaSlice bda_alloc_device(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
[[nodiscard]] BdaSlice bda_alloc_staging(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
[[nodiscard]] BdaSlice bda_alloc_transient(VulkanContext *vk, VkDeviceSize size, VkDeviceSize align);
void bda_transient_reset(void);
void bda_get_stats(BdaStats *out);

// Rewind one quarter of the transient bump. signal_point & 3 selects the slice.
// Polls acquire with timeout 0; if the previous timeline signal recorded on
// that slice has not yet retired, the slice is left untouched and
// bda_alloc_transient returns a null BdaSlice for this frame.
void bda_transient_begin_frame(VkSemaphore acquire, uint64_t signal_point);
void bda_transient_abort_frame(uint64_t signal_point);

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
