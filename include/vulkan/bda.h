#ifndef ENGINE_VULKAN_BDA_H
#define ENGINE_VULKAN_BDA_H

#include "vulkan/types.h"
#include <stdint.h>

// One device-address buffer. Sub-allocations stay live until reset or destroy.
// The volume grid is the user. Mesh buffers stay on their own allocations.

typedef struct BdaArena {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    VkDeviceAddress address;
    uint8_t        *map;
    uint32_t        capacity;
    uint32_t        offset;
} BdaArena;

typedef struct BdaSlice {
    void           *cpu;
    VkDeviceAddress gpu;
    uint32_t        bytes;
} BdaSlice;

[[nodiscard]] int bda_arena_init(VulkanContext *vk, BdaArena *arena, uint32_t capacity);
void bda_arena_destroy(VulkanContext *vk, BdaArena *arena);
void bda_arena_reset(BdaArena *arena);
[[nodiscard]] BdaSlice bda_arena_alloc(BdaArena *arena, uint32_t bytes, uint32_t align);

#endif /* ENGINE_VULKAN_BDA_H */
