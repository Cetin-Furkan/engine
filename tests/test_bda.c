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

int main(void) {
    test_bda_alloc_and_free();
    test_bda_two_way_coalescing();
    test_bda_padding_preservation();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_bda: all checks passed\n");
    return 0;
}
