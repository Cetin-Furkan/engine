#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static uint64_t s_mock_sem_counter = 0u;
static VkResult s_mock_sem_result  = VK_SUCCESS;
static uint32_t s_mock_sem_polls   = 0u;

static VkResult mock_vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore sem, uint64_t *pValue) {
    (void)device;
    (void)sem;
    s_mock_sem_polls++;
    if (pValue != nullptr) {
        *pValue = s_mock_sem_counter;
    }
    return s_mock_sem_result;
}

// Disable real Vulkan creation for unit testing pool logic
#define create_raw_block disabled_create_raw_block
#define destroy_raw_block disabled_destroy_raw_block
#define vkGetSemaphoreCounterValue mock_vkGetSemaphoreCounterValue
#include "../src/vulkan/memory/bda.c"
#undef vkGetSemaphoreCounterValue
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
    s_bda_sys.device_pool.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    add_mock_block(&s_bda_sys.device_pool, 64 * 1024);
    add_mock_block(&s_bda_sys.staging_pool, 32 * 1024);
    VulkanContext vk = { .device = (VkDevice)(uintptr_t)0x1 };

    BdaArena arena = {0};
    CHECK(bda_arena_init(&vk, &arena, 4096, false) == 0);
    // UMA / ReBAR: device_pool block has non-null base_cpu, so arena.map is non-null
    CHECK(arena.map != nullptr);
    CHECK(arena.suballocated);
    BdaSlice keep = alloc_from_pool(nullptr, &s_bda_sys.device_pool, 256, 256);
    CHECK(keep.offset == 4096);

    bda_arena_destroy(&vk, &arena);
    CHECK(s_bda_sys.device_pool.blocks->allocated == 4096 + 256);
    CHECK(range_found(s_bda_sys.device_pool.blocks, 0, 4096));
    CHECK(arena.buffer == VK_NULL_HANDLE);
    BdaStats st = {0};
    bda_get_stats(&st);
    CHECK(st.device_allocated == 256);

    free_mock_pool(&s_bda_sys.device_pool);
    free_mock_pool(&s_bda_sys.staging_pool);
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
    CHECK(buf.map != nullptr);
    CHECK(buf.offset == 0);
    CHECK(s_bda_sys.device_pool.blocks->allocated == 256);
    da_buffer_destroy(&vk, &buf);
    CHECK(s_bda_sys.device_pool.blocks->allocated == 0);

    s_bda_sys.transient_ring.buffer = (VkBuffer)(uintptr_t)0x2;
    s_bda_sys.transient_ring.capacity = 4096;
    s_bda_sys.transient_ring.base_cpu = malloc(4096);
    s_bda_sys.transient_ring.base_gpu = 0x8000;
    bda_transient_begin_frame(VK_NULL_HANDLE, 1);
    BdaSlice hi = bda_alloc_transient(&vk, 64, 16);
    CHECK(hi.offset == 1024);
    BdaSlice hi_more = bda_alloc_transient(&vk, 64, 16);
    CHECK(hi_more.offset == 1088);
    bda_transient_begin_frame(VK_NULL_HANDLE, 0);
    BdaSlice lo = bda_alloc_transient(&vk, 64, 16);
    CHECK(lo.offset == 0);
    BdaSlice lo_more = bda_alloc_transient(&vk, 64, 16);
    CHECK(lo_more.offset == 64);
    bda_transient_begin_frame(VK_NULL_HANDLE, 5);
    BdaSlice hi_again = bda_alloc_transient(&vk, 64, 16);
    CHECK(hi_again.offset == 1024);
    CHECK(lo_more.offset + lo_more.bytes <= 1024);

    free(s_bda_sys.transient_ring.base_cpu);
    free_mock_pool(&s_bda_sys.device_pool);
    s_bda_sys = (BdaSystem){0};
}

static void test_da_buffer_write_bounds(void) {
    uint8_t storage[32];
    memset(storage, 0xab, sizeof storage);
    DaBuffer buf = {
        .map = storage,
        .bytes = sizeof storage,
    };
    uint8_t four[4] = { 1, 2, 3, 4 };
    CHECK(da_buffer_write(&buf, 0, four, 4) == 0);
    CHECK(storage[0] == 1 && storage[1] == 2 && storage[2] == 3 && storage[3] == 4);
    CHECK(da_buffer_write(&buf, 30, four, 4) != 0);
    CHECK(storage[30] == 0xab && storage[31] == 0xab);
    CHECK(da_buffer_write(nullptr, 0, four, 4) != 0);
    CHECK(da_buffer_write(&buf, 32, nullptr, 0) == 0);
    CHECK(da_buffer_write(&buf, 33, nullptr, 0) != 0);
    DaBuffer unmapped = { .bytes = 4 };
    CHECK(da_buffer_write(&unmapped, 0, four, 4) != 0);
}

static void test_bda_stats(void) {
    bda_get_stats(nullptr);

    s_bda_sys = (BdaSystem){0};
    BdaStats st = {0};
    bda_get_stats(&st);
    CHECK(st.device_capacity == 0);
    CHECK(st.device_allocated == 0);
    CHECK(st.device_blocks == 0);

    s_bda_sys.is_initialized = true;
    add_mock_block(&s_bda_sys.device_pool, 64 * 1024);
    add_mock_block(&s_bda_sys.staging_pool, 32 * 1024);

    BdaSlice d0 = alloc_from_pool(nullptr, &s_bda_sys.device_pool, 1024, 256);
    BdaSlice d1 = alloc_from_pool(nullptr, &s_bda_sys.device_pool, 2048, 256);
    BdaSlice d2 = alloc_from_pool(nullptr, &s_bda_sys.device_pool, 512, 256);
    // Free middle slice d1 so free_list has a 2048-byte hole inside allocated=3584
    return_to_pool(&s_bda_sys.device_pool, d1.buffer, d1.offset, d1.bytes);
    (void)d0;
    (void)d2;

    BdaSlice s0 = alloc_from_pool(nullptr, &s_bda_sys.staging_pool, 4096, 256);
    (void)s0;

    s_bda_sys.transient_ring.buffer = (VkBuffer)(uintptr_t)0x2;
    s_bda_sys.transient_ring.capacity = 8192;
    s_bda_sys.transient_ring.base_cpu = malloc(8192);
    s_bda_sys.transient_ring.base_gpu = 0x9000;
    VulkanContext vk = { .device = (VkDevice)(uintptr_t)0x1 };
    bda_transient_begin_frame(VK_NULL_HANDLE, 0);
    (void)bda_alloc_transient(&vk, 512, 64);

    bda_get_stats(&st);
    CHECK(st.device_capacity == 64 * 1024);
    CHECK(st.device_allocated == 1024 + 512);
    CHECK(st.device_blocks == 1u);
    CHECK(st.staging_capacity == 32 * 1024);
    CHECK(st.staging_allocated == 4096);
    CHECK(st.staging_blocks == 1u);
    CHECK(st.transient_capacity == 8192);
    CHECK(st.transient_used == 512);

    free(s_bda_sys.transient_ring.base_cpu);
    free_mock_pool(&s_bda_sys.device_pool);
    free_mock_pool(&s_bda_sys.staging_pool);
    s_bda_sys = (BdaSystem){0};
}

static void test_bda_unified_uma_zero_staging(void) {
    VkBufferUsageFlags slab = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    s_bda_sys = (BdaSystem){0};
    s_bda_sys.is_initialized = true;
    s_bda_sys.unified_uma = true;
    s_bda_sys.device_pool.usage = slab;
    s_bda_sys.device_pool.is_host_mapped = true;
    add_mock_block(&s_bda_sys.device_pool, 64 * 1024);

    VulkanContext vk = {0};
    vk.device = (VkDevice)(uintptr_t)0x1;
    vk.device_info.is_true_uma = true;

    DaBuffer host_buf = {0};
    CHECK(da_buffer_create(&vk, 512, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           true, &host_buf) == 0);
    CHECK(host_buf.is_suballocated);
    CHECK(host_buf.map != nullptr);

    BdaStats st = {0};
    bda_get_stats(&st);
    CHECK(st.device_allocated == 512);
    CHECK(st.staging_allocated == 0);
    CHECK(st.staging_capacity == 0);

    da_buffer_destroy(&vk, &host_buf);
    bda_get_stats(&st);
    CHECK(st.device_allocated == 0);

    free_mock_pool(&s_bda_sys.device_pool);
    s_bda_sys = (BdaSystem){0};
}

static void test_bda_bump_padding_coalesce(void) {
    BdaPool pool = {
        .slab_size      = 64 * 1024,
        .usage          = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .mem_flags      = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .is_host_mapped = true,
    };
    add_mock_block(&pool, 64 * 1024);

    // 1. Allocate 64 bytes at align 64 -> [0, 64), allocated = 64
    BdaSlice u0 = alloc_from_pool(nullptr, &pool, 64, 64);
    CHECK(u0.offset == 0 && u0.bytes == 64);
    CHECK(pool.blocks->allocated == 64);
    CHECK(pool.blocks->free_count == 0);

    // 2. Allocate 256 bytes at align 256 -> bumps to [256, 512) and MUST preserve [64, 256) in free_list!
    BdaSlice u1 = alloc_from_pool(nullptr, &pool, 256, 256);
    CHECK(u1.offset == 256 && u1.bytes == 256);
    CHECK(pool.blocks->allocated == 512);
    CHECK(pool.blocks->free_count == 1);
    CHECK(range_found(pool.blocks, 64, 192));

    // 3. Return u0 and u1 -> must coalesce [0,64) + [64,256) + [256,512) back to allocated == 0!
    CHECK(return_to_pool(&pool, u0.buffer, u0.offset, u0.bytes));
    CHECK(return_to_pool(&pool, u1.buffer, u1.offset, u1.bytes));
    CHECK(pool.blocks->allocated == 0);
    CHECK(pool.blocks->free_count == 0);

    free_mock_pool(&pool);
}

static void test_bda_four_slice_ring_and_overflow(void) {
    s_bda_sys = (BdaSystem){0};
    s_bda_sys.is_initialized          = true;
    s_bda_sys.transient_ring.buffer   = (VkBuffer)(uintptr_t)0x77;
    s_bda_sys.transient_ring.capacity = 4096;
    s_bda_sys.transient_ring.base_cpu = malloc(4096);
    s_bda_sys.transient_ring.base_gpu = 0x200000;
    VulkanContext vk = { .device = (VkDevice)(uintptr_t)0x1 };

    // Verify all 4 slices occupy distinct 1024-byte quarters and wrap cleanly across points 0..7
    for (uint64_t pt = 0; pt < 8; ++pt) {
        bda_transient_begin_frame(VK_NULL_HANDLE, pt);
        BdaSlice s = bda_alloc_transient(&vk, 128, 64);
        uint32_t expected_base = (uint32_t)((pt & 3u) * 1024u);
        CHECK(s.buffer == s_bda_sys.transient_ring.buffer);
        CHECK(s.offset == expected_base);
        CHECK(s.bytes == 128);
    }

    // Overflowing a 1024-byte slice must return (BdaSlice){0} and NEVER leak into staging_pool
    BdaSlice over = bda_alloc_transient(&vk, 2048, 64);
    CHECK(over.buffer == VK_NULL_HANDLE);
    CHECK(over.cpu == nullptr && over.gpu == 0);

    BdaStats st = {0};
    bda_get_stats(&st);
    CHECK(st.staging_allocated == 0);
    CHECK(st.staging_capacity == 0);
    CHECK(st.staging_blocks == 0);

    // Track standalone accounting round-trip
    s_bda_sys.standalone_bytes = 32768;
    s_bda_sys.standalone_count = 2;
    s_bda_sys.vk_alloc_count   = 5;
    bda_get_stats(&st);
    CHECK(st.standalone_allocated == 32768);
    CHECK(st.standalone_blocks == 2);
    CHECK(st.vk_alloc_count == 5);

    free(s_bda_sys.transient_ring.base_cpu);
    s_bda_sys = (BdaSystem){0};
}

static void test_bda_transient_unretired_poll_nonblocking(void) {
    VulkanContext vk = { .device = (VkDevice)(uintptr_t)0x1 };
    VkSemaphore sem  = (VkSemaphore)(uintptr_t)0x99;

    s_bda_sys = (BdaSystem){0};
    s_bda_sys.is_initialized          = true;
    s_bda_sys.vk                      = &vk;
    s_bda_sys.transient_ring.buffer   = (VkBuffer)(uintptr_t)0x88;
    s_bda_sys.transient_ring.capacity = 4096;
    s_bda_sys.transient_ring.base_cpu = malloc(4096);
    s_bda_sys.transient_ring.base_gpu = 0x300000;
    bda_transient_reset();

    // 1. Claim slice 1 at timeline point 1 and write sentinel bytes.
    s_mock_sem_counter = 0u;
    s_mock_sem_result  = VK_SUCCESS;
    s_mock_sem_polls   = 0u;
    bda_transient_begin_frame(sem, 1u);
    CHECK(!s_bda_sys.transient_slice_busy);
    BdaSlice live = bda_alloc_transient(&vk, 256, 64);
    CHECK(live.buffer == s_bda_sys.transient_ring.buffer);
    CHECK(live.offset == 1024u);
    memset(live.cpu, 0xa5, 256);

    // 2. Wrap around to slice 1 at point 5 while GPU timeline is still at 0 (< 1).
    //    Must poll once (timeout=0, < 5 ms), mark slice busy, preserve live bytes,
    //    and return a null slice from bda_alloc_transient!
    struct timespec t0 = {0}, t1 = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    bda_transient_begin_frame(sem, 5u);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t elapsed_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                          (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    CHECK(elapsed_ns < 5000000ull);
    CHECK(s_mock_sem_polls == 1u);
    CHECK(s_bda_sys.transient_slice_busy);
    CHECK(s_bda_sys.transient_point[1] == 1u);
    CHECK(s_bda_sys.transient_at[1] == 1024u + 256u);

    BdaSlice blocked = bda_alloc_transient(&vk, 64, 16);
    CHECK(blocked.buffer == VK_NULL_HANDLE);
    CHECK(blocked.cpu == nullptr && blocked.gpu == 0);
    CHECK(((uint8_t *)s_bda_sys.transient_ring.base_cpu)[1024] == 0xa5u);

    // 3. Advance GPU timeline to 1 -> slice 1 is retired and rewinds cleanly!
    s_mock_sem_counter = 1u;
    bda_transient_begin_frame(sem, 9u);
    CHECK(s_mock_sem_polls == 2u);
    CHECK(!s_bda_sys.transient_slice_busy);
    CHECK(s_bda_sys.transient_point[1] == 9u);
    BdaSlice reused = bda_alloc_transient(&vk, 64, 16);
    CHECK(reused.buffer == s_bda_sys.transient_ring.buffer);
    CHECK(reused.offset == 1024u);

    // 4. Aborting an unsubmitted frame clears transient_point[slot] so the next
    //    frame does not wait on a point that was never submitted.
    bda_transient_abort_frame(9u);
    CHECK(s_bda_sys.transient_point[1] == 0u);
    CHECK(!s_bda_sys.transient_slice_busy);

    free(s_bda_sys.transient_ring.base_cpu);
    s_bda_sys = (BdaSystem){0};
}

int main(void) {
    test_da_buffer_write_bounds();
    test_bda_alloc_and_free();
    test_bda_two_way_coalescing();
    test_bda_padding_preservation();
    test_bda_free_list_grows();
    test_bda_arena_returns_slice();
    test_bda_usage_and_transient();
    test_bda_stats();
    test_bda_unified_uma_zero_staging();
    test_bda_bump_padding_coalesce();
    test_bda_four_slice_ring_and_overflow();
    test_bda_transient_unretired_poll_nonblocking();
    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_bda: all checks passed\n");
    return 0;
}

