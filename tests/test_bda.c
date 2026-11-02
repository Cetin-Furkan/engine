#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *vulkan_vkresult_to_str(VkResult result) {
    (void)result;
    return "MOCK_VK_RESULT";
}

VulkanResult vulkan_find_memory_type(const VulkanContext *vk, uint32_t type_bits,
                                    VkMemoryPropertyFlags properties, uint32_t *out_index) {
    (void)vk; (void)type_bits; (void)properties;
    if (out_index) *out_index = 0;
    return VULKAN_SUCCESS;
}

// Disable real Vulkan creation for unit testing pool logic
#define create_raw_block disabled_create_raw_block
#define destroy_raw_block disabled_destroy_raw_block
#include "../src/vulkan/bda.c"
#undef create_raw_block
#undef destroy_raw_block

static int s_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                   \
        }                                                                   \
    } while (0)

static void add_mock_block(BdaPool *pool, VkDeviceSize size) {
    BdaBlock *b = calloc(1, sizeof(BdaBlock));
    b->buffer = (VkBuffer)(uintptr_t)0x1234;
    b->memory = (VkDeviceMemory)(uintptr_t)0x5678;
    b->base_gpu = 0x100000;
    b->base_cpu = (uint8_t *)malloc((size_t)size);
    b->capacity = size;
    b->allocated = 0;
    b->free_count = 0;
    b->next = pool->blocks;
    pool->blocks = b;
    pool->block_count++;
    pool->total_bytes += size;
}

static void free_mock_pool(BdaPool *pool) {
    BdaBlock *b = pool->blocks;
    while (b != nullptr) {
        BdaBlock *next = b->next;
        if (b->base_cpu) free(b->base_cpu);
        free(b->free_extra);
        free(b);
        b = next;
    }
    *pool = (BdaPool){0};
}

static void test_bda_alloc_and_free(void) {
    BdaPool pool = {
        .slab_size = 1024 * 1024,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .mem_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .is_host_mapped = true,
    };
    add_mock_block(&pool, 1024 * 1024);

    // Initial allocation
    BdaSlice a = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(a.buffer != VK_NULL_HANDLE);
    CHECK(a.offset == 0);
    CHECK(a.bytes == 256);
    CHECK(pool.blocks->allocated == 256);
    CHECK(pool.blocks->free_count == 0);

    // Second allocation
    BdaSlice b = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(b.offset == 256);
    CHECK(pool.blocks->allocated == 512);

    // Third allocation
    BdaSlice c = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(c.offset == 512);
    CHECK(pool.blocks->allocated == 768);

    // Free C (at high-water mark): should immediately rewind allocated to 512
    return_to_pool(&pool, c.buffer, c.offset, c.bytes);
    CHECK(pool.blocks->allocated == 512);
    CHECK(pool.blocks->free_count == 0);

    // Re-allocate C
    c = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(c.offset == 512);
    CHECK(pool.blocks->allocated == 768);

    // Free A: goes into free_list
    return_to_pool(&pool, a.buffer, a.offset, a.bytes);
    CHECK(pool.blocks->allocated == 768);
    CHECK(pool.blocks->free_count == 1);
    CHECK(pool.blocks->free_list[0].offset == 0);
    CHECK(pool.blocks->free_list[0].size == 256);

    // Free C: high-water mark rewinds to 512 (where b ends)
    return_to_pool(&pool, c.buffer, c.offset, c.bytes);
    CHECK(pool.blocks->allocated == 512);
    CHECK(pool.blocks->free_count == 1);

    // Free B: merges with A [0, 256) into [0, 512), and touches allocated (512), so rewinds to 0!
    return_to_pool(&pool, b.buffer, b.offset, b.bytes);
    CHECK(pool.blocks->allocated == 0);
    CHECK(pool.blocks->free_count == 0);

    free_mock_pool(&pool);
}

static void test_bda_two_way_coalescing(void) {
    BdaPool pool = {
        .slab_size = 1024 * 1024,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .mem_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .is_host_mapped = true,
    };
    add_mock_block(&pool, 1024 * 1024);

    BdaSlice s0 = alloc_from_pool(nullptr, &pool, 256, 256);
    BdaSlice s1 = alloc_from_pool(nullptr, &pool, 256, 256);
    BdaSlice s2 = alloc_from_pool(nullptr, &pool, 256, 256);
    BdaSlice s3 = alloc_from_pool(nullptr, &pool, 256, 256); // Anchor to keep allocated at 1024

    CHECK(pool.blocks->allocated == 1024);

    // Free s0 [0, 256)
    return_to_pool(&pool, s0.buffer, s0.offset, s0.bytes);
    CHECK(pool.blocks->free_count == 1);

    // Free s2 [512, 768)
    return_to_pool(&pool, s2.buffer, s2.offset, s2.bytes);
    CHECK(pool.blocks->free_count == 2);

    // Free s1 [256, 512) in the middle: must bridge s0 and s2 into a single [0, 768) block
    return_to_pool(&pool, s1.buffer, s1.offset, s1.bytes);
    CHECK(pool.blocks->free_count == 1);
    CHECK(pool.blocks->free_list[0].offset == 0);
    CHECK(pool.blocks->free_list[0].size == 768);

    // Allocate 768: should take the entire coalesced free block
    BdaSlice big = alloc_from_pool(nullptr, &pool, 768, 256);
    CHECK(big.offset == 0);
    CHECK(big.bytes == 768);
    CHECK(pool.blocks->free_count == 0);
    CHECK(pool.blocks->allocated == 1024);

    // Clean up
    return_to_pool(&pool, big.buffer, big.offset, big.bytes);
    return_to_pool(&pool, s3.buffer, s3.offset, s3.bytes);
    CHECK(pool.blocks->allocated == 0);
    CHECK(pool.blocks->free_count == 0);

    free_mock_pool(&pool);
}

static void test_bda_padding_preservation(void) {
    BdaPool pool = {
        .slab_size = 1024 * 1024,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .mem_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .is_host_mapped = true,
    };
    add_mock_block(&pool, 1024 * 1024);

    // Allocate 1024 bytes and an anchor
    BdaSlice block = alloc_from_pool(nullptr, &pool, 1024, 256);
    BdaSlice anchor = alloc_from_pool(nullptr, &pool, 256, 256);
    (void)anchor;

    // Free block: free range [0, 1024)
    return_to_pool(&pool, block.buffer, block.offset, block.bytes);
    CHECK(pool.blocks->free_count == 1);

    // Offset free_list artificially: starts at 64
    pool.blocks->free_list[0] = (BdaRange){ 64, 960 };

    // Allocate size 256 with align 256:
    // cur = 256, pad = 192 (bytes [64, 256)), alloc = 256 (bytes [256, 512)), rem = 512 (bytes [512, 1024))
    BdaSlice sub = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(sub.offset == 256);
    CHECK(sub.bytes == 256);

    // Both pad and rem should be preserved in free_list!
    CHECK(pool.blocks->free_count == 2);
    bool found_pad = false;
    bool found_rem = false;
    for (uint32_t i = 0; i < pool.blocks->free_count; ++i) {
        if (pool.blocks->free_list[i].offset == 64 && pool.blocks->free_list[i].size == 192) found_pad = true;
        if (pool.blocks->free_list[i].offset == 512 && pool.blocks->free_list[i].size == 512) found_rem = true;
    }
    CHECK(found_pad);
    CHECK(found_rem);

    free_mock_pool(&pool);
}

static bool range_found(BdaBlock *b, VkDeviceSize offset, VkDeviceSize size) {
    BdaRange *ranges = block_ranges(b);
    for (uint32_t i = 0; i < b->free_count; ++i) {
        if (ranges[i].offset == offset && ranges[i].size == size) {
            return true;
        }
    }
    return false;
}

static void test_bda_free_list_grows(void) {
    BdaPool pool = {
        .slab_size = 1024 * 1024,
        .is_host_mapped = true,
    };
    add_mock_block(&pool, 1024 * 1024);
    BdaBlock *b = pool.blocks;
    b->allocated = 100000;
    b->free_count = 64;
    for (uint32_t i = 0; i < 64u; ++i) {
        b->free_list[i] = (BdaRange){ 10u + (VkDeviceSize)i * 4u, 1u };
    }
    return_to_pool(&pool, b->buffer, 5000, 8);
    CHECK(b->free_count == 65u);
    CHECK(b->free_extra != nullptr);
    CHECK(range_found(b, 5000, 8));
    CHECK(b->allocated == 100000);

    // A full inline list must still keep both sides of an aligned split.
    b->free_count = 64;
    free(b->free_extra);
    b->free_extra = nullptr;
    b->free_cap = 0;
    b->free_list[0] = (BdaRange){ 64, 960 };
    for (uint32_t i = 1; i < 64u; ++i) {
        b->free_list[i] = (BdaRange){ 20000u + (VkDeviceSize)i * 4u, 1u };
    }
    BdaSlice sub = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(sub.offset == 256);
    CHECK(sub.bytes == 256);
    CHECK(b->free_count == 65u);
    CHECK(range_found(b, 64, 192));
    CHECK(range_found(b, 512, 512));

    free_mock_pool(&pool);
}

static void test_bda_arena_returns_slice(void) {
    s_bda_sys = (BdaSystem){0};
    s_bda_sys.is_initialized = true;
    add_mock_block(&s_bda_sys.device_pool, 64 * 1024);
    BdaSlice slab = alloc_from_pool(nullptr, &s_bda_sys.device_pool, 4096, 256);
    BdaSlice keep = alloc_from_pool(nullptr, &s_bda_sys.device_pool, 256, 256);
    CHECK(slab.offset == 0);
    CHECK(keep.offset == 4096);
    BdaArena arena = {
        .buffer         = slab.buffer,
        .address        = slab.gpu,
        .capacity       = (uint32_t)slab.bytes,
        .slice_offset   = slab.offset,
        .slice_bytes    = slab.bytes,
        .suballocated   = true,
    };
    bda_arena_destroy(nullptr, &arena);
    CHECK(s_bda_sys.device_pool.blocks->allocated == 4096 + 256);
    CHECK(range_found(s_bda_sys.device_pool.blocks, 0, 4096));
    CHECK(arena.buffer == VK_NULL_HANDLE);

    free_mock_pool(&s_bda_sys.device_pool);
    s_bda_sys = (BdaSystem){0};
}

static void test_bda_usage_and_transient(void) {
    VkBufferUsageFlags slab = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    CHECK(usage_fits_slab(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, slab));
    CHECK(usage_fits_slab(VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, slab));
    CHECK(!usage_fits_slab(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, slab));

    s_bda_sys = (BdaSystem){0};
    s_bda_sys.is_initialized = true;
    s_bda_sys.device_pool.usage = slab;
    s_bda_sys.device_pool.is_host_mapped = true;
    add_mock_block(&s_bda_sys.device_pool, 64 * 1024);
    VulkanContext vk = {0};
    vk.device = (VkDevice)(uintptr_t)0x1;
    DaBuffer buf = {0};
    CHECK(da_buffer_create(&vk, 256, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, &buf) == 0);
    CHECK(buf.is_suballocated);
    CHECK(buf.offset == 0);
    CHECK(s_bda_sys.device_pool.blocks->allocated == 256);

    s_bda_sys.transient_ring.buffer = (VkBuffer)(uintptr_t)0x2;
    s_bda_sys.transient_ring.capacity = 2048;
    s_bda_sys.transient_ring.base_cpu = malloc(2048);
    s_bda_sys.transient_ring.base_gpu = 0x8000;
    bda_transient_begin_frame(1);
    BdaSlice hi = bda_alloc_transient(&vk, 64, 16);
    CHECK(hi.offset == 1024);
    BdaSlice hi_more = bda_alloc_transient(&vk, 64, 16);
    CHECK(hi_more.offset == 1088);
    bda_transient_begin_frame(2);
    BdaSlice lo = bda_alloc_transient(&vk, 64, 16);
    CHECK(lo.offset == 0);
    BdaSlice lo_more = bda_alloc_transient(&vk, 64, 16);
    CHECK(lo_more.offset == 64);
    bda_transient_begin_frame(1);
    BdaSlice hi_again = bda_alloc_transient(&vk, 64, 16);
    CHECK(hi_again.offset == 1024);
    CHECK(lo_more.offset + lo_more.bytes <= 1024);

    free(s_bda_sys.transient_ring.base_cpu);
    free_mock_pool(&s_bda_sys.device_pool);
    s_bda_sys = (BdaSystem){0};
}

int main(void) {
    test_bda_alloc_and_free();
    test_bda_two_way_coalescing();
    test_bda_padding_preservation();
    test_bda_free_list_grows();
    test_bda_arena_returns_slice();
    test_bda_usage_and_transient();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_bda: all checks passed\n");
    return 0;
}
