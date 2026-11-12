#include "frame/internal.h"
#include "input/page.h"
#include "vulkan/bda.h"
#include "vulkan/device.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Focused failure-path & non-blocking contract tests exercising real src/ code:
// 1. Transient BDA slice overflow returns a null BdaSlice, app->draw skips
//    recording a null device address, and record_and_submit_clear still
//    presents the frame cleanly with zero waits.
// 2. Orphan table at k_orphan_max_cap (64) refuses a 65th slot (-1) without
//    blocking and without sliding any existing generation-tagged SlotRef handle.
// 3. VK_ERROR_DEVICE_LOST invokes vulkan_report_device_fault (including
//    VK_EXT_device_fault address + vendor telemetry) and returns cleanly.

static int s_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                   \
        }                                                                   \
    } while (0)

static uint64_t mono_ns_now(void) {
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

FrameState frame_state = {0};

static uint64_t s_mock_sem_counter   = 0u;
static VkResult s_mock_sem_result    = VK_SUCCESS;
static VkResult s_mock_submit_result = VK_SUCCESS;
static uint32_t s_sem_poll_calls     = 0u;
static uint32_t s_sem_wait_calls     = 0u;
static uint32_t s_queue_submit_calls = 0u;
static uint32_t s_begin_render_calls = 0u;
static uint32_t s_end_render_calls   = 0u;
static uint32_t s_end_cmd_calls      = 0u;
static uint32_t s_publish_acq_calls  = 0u;
static uint32_t s_destroy_gpu_calls  = 0u;
static uint64_t s_released_mask      = 0ull;

VulkanResult vulkan_find_memory_type(const VulkanContext *vk, uint32_t type_bits,
                                     VkMemoryPropertyFlags properties, uint32_t *out_index) {
    (void)vk;
    (void)type_bits;
    (void)properties;
    if (out_index != nullptr) {
        *out_index = 0u;
    }
    return VULKAN_SUCCESS;
}

static VkResult mock_vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore sem, uint64_t *pValue) {
    (void)device;
    (void)sem;
    s_sem_poll_calls++;
    if (pValue != nullptr) {
        *pValue = s_mock_sem_counter;
    }
    return s_mock_sem_result;
}

static VkResult mock_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    (void)device;
    (void)info;
    (void)timeout;
    s_sem_wait_calls++;
    return VK_SUCCESS;
}

static VkResult mock_vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence) {
    (void)queue;
    (void)count;
    (void)submits;
    (void)fence;
    s_queue_submit_calls++;
    return s_mock_submit_result;
}

static VkResult mock_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
                                              VkCommandBuffer *out) {
    (void)device;
    (void)info;
    if (out != nullptr) {
        *out = (VkCommandBuffer)(uintptr_t)0xabc0;
    }
    return VK_SUCCESS;
}

static VkResult mock_vkResetCommandBuffer(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
    (void)cmd;
    (void)flags;
    return VK_SUCCESS;
}

static VkResult mock_vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info) {
    (void)cmd;
    (void)info;
    return VK_SUCCESS;
}

static VkResult mock_vkEndCommandBuffer(VkCommandBuffer cmd) {
    (void)cmd;
    s_end_cmd_calls++;
    return VK_SUCCESS;
}

static void mock_vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    (void)cmd;
    (void)info;
    s_begin_render_calls++;
}

static void mock_vkCmdEndRendering(VkCommandBuffer cmd) {
    (void)cmd;
    s_end_render_calls++;
}

static void mock_vkCmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport *vp) {
    (void)cmd;
    (void)first;
    (void)count;
    (void)vp;
}

static void mock_vkCmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D *sc) {
    (void)cmd;
    (void)first;
    (void)count;
    (void)sc;
}

static void mock_vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo *dep) {
    (void)cmd;
    (void)dep;
}

static void mock_vkCmdCopyImage2(VkCommandBuffer cmd, const VkCopyImageInfo2 *info) {
    (void)cmd;
    (void)info;
}

// Stubs for Wayland/DRM/timer side-effects around orphan.c, barrier.c, and record.c
WaylandContext *wayland_get_context(void) { return nullptr; }
bool wayland_defer_destroy(WaylandContext *w, uint32_t id) { (void)w; (void)id; return true; }
bool wayland_put_raw(WaylandContext *w, uint32_t id, uint16_t op, const void *p, uint16_t len) {
    (void)w; (void)id; (void)op; (void)p; (void)len; return true;
}
int wayland_send_batch(WaylandContext *w) { (void)w; return 0; }
void wayland_untrack_buffer(WaylandContext *w, uint32_t id) { (void)w; (void)id; }
void drm_syncobj_destroy(int drm_fd, uint32_t *handle) {
    (void)drm_fd;
    if (handle != nullptr) {
        *handle = 0u;
    }
}
void input_page_record(VkCommandBuffer cmd) { (void)cmd; }

void gpu_timer_init(VulkanContext *vk) { (void)vk; }
void gpu_timer_shutdown(VulkanContext *vk) { (void)vk; }
void gpu_timer_begin_frame(VulkanContext *vk, VkCommandBuffer cmd, FrameSlot *frame) {
    (void)vk; (void)cmd; (void)frame;
}
void gpu_timer_mark_compute_end(VkCommandBuffer cmd, const FrameSlot *frame) { (void)cmd; (void)frame; }
void gpu_timer_mark_render_begin(VkCommandBuffer cmd, const FrameSlot *frame) { (void)cmd; (void)frame; }
void gpu_timer_end_frame(VkCommandBuffer cmd, FrameSlot *frame) { (void)cmd; (void)frame; }
void gpu_timer_begin_transfer(VulkanContext *vk, VkCommandBuffer cmd, FrameSlot *frame) {
    (void)vk; (void)cmd; (void)frame;
}
void gpu_timer_end_transfer(VkCommandBuffer cmd, FrameSlot *frame) { (void)cmd; (void)frame; }
void gpu_timer_harvest(VulkanContext *vk, FrameSlot *frame) { (void)vk; (void)frame; }

bool acquire_point_retired(VulkanContext *vk, uint64_t point) {
    if (point == 0u || vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return true;
    }
    uint64_t done = 0u;
    VkResult r = mock_vkGetSemaphoreCounterValue(vk->device, frame_state.acquire, &done);
    return r == VK_SUCCESS && done >= point;
}

bool wait_acquire_point(VulkanContext *vk, uint64_t point) {
    s_sem_wait_calls++;
    return acquire_point_retired(vk, point);
}

int wait_own_release(const FrameSlot *frame, uint64_t timeout_ns) {
    (void)frame;
    (void)timeout_ns;
    s_sem_wait_calls++;
    return 0;
}

bool poll_slot_released(const WaylandContext *wl, const FrameSlot *frame) {
    (void)wl;
    if (frame == nullptr || !frame->release_pending) {
        return true;
    }
    if (frame->release_point >= 100u && frame->release_point < 164u) {
        uint32_t bit = (uint32_t)(frame->release_point - 100u);
        return (s_released_mask & (1ull << bit)) != 0ull;
    }
    return false;
}

void clear_pixels(FrameSlot *frame) {
    frame->image       = (VulkanExportableImage){ .dmabuf_fd = -1 };
    frame->image_view  = VK_NULL_HANDLE;
    frame->view_format = VK_FORMAT_UNDEFINED;
    frame->prime_image  = VK_NULL_HANDLE;
    frame->prime_memory = VK_NULL_HANDLE;
    frame->prime_view   = VK_NULL_HANDLE;
    frame->targets      = (EngineTargets){0};
}

bool slot_has_pixels(const FrameSlot *frame) {
    return frame != nullptr &&
           (frame->image.image != VK_NULL_HANDLE ||
            frame->image_view != VK_NULL_HANDLE ||
            frame->wl_buffer_id != 0u);
}

void blank_frame(FrameSlot *frame) {
    uint32_t generation = slot_next_generation(frame->generation);
    *frame = (FrameSlot){
        .image           = { .dmabuf_fd = -1 },
        .generation      = generation,
        .release_eventfd = -1,
    };
}

SlotRef slot_ref(const FrameSlot *slot) {
    if (slot == nullptr || slot->generation == 0u) {
        return 0u;
    }
    for (int i = 0; i < 2; ++i) {
        if (&frame_state.slot[i] == slot) {
            return slot_ref_pack(slot->generation, (uint32_t)(i + 1));
        }
    }
    uint64_t mask = frame_state.orphan_mask;
    while (mask != 0ull) {
        uint32_t i = (uint32_t)__builtin_ctzll(mask);
        mask &= mask - 1ull;
        if (&frame_state.orphans[i] == slot) {
            return slot_ref_pack(slot->generation, i + 3u);
        }
    }
    return 0u;
}

FrameSlot *slot_from_ref(SlotRef ref) {
    return slot_resolve(ref, frame_state.slot, frame_state.orphans, frame_state.orphan_cap);
}

void destroy_frame_gpu(VulkanContext *vk, FrameSlot *frame) {
    (void)vk;
    if (frame != nullptr) {
        s_destroy_gpu_calls++;
        clear_pixels(frame);
        frame->cmd_buf       = VK_NULL_HANDLE;
        frame->acquire_point = 0u;
    }
}

int ensure_frame_targets(VulkanContext *vk, FrameSlot *frame) {
    (void)vk;
    if (frame == nullptr || frame->image_view == VK_NULL_HANDLE) {
        return -1;
    }
    return 0;
}

int publish_acquire_point_from(VulkanContext *vk, VkSemaphore *io_sync_sem,
                               uint64_t point, int dmabuf_fd) {
    (void)vk;
    (void)io_sync_sem;
    (void)point;
    (void)dmabuf_fd;
    s_publish_acq_calls++;
    return 0;
}

// Include real BDA, orphan, barrier, and record implementations with mocked driver hooks
#define create_raw_block           disabled_create_raw_block
#define destroy_raw_block          disabled_destroy_raw_block
#define vkGetSemaphoreCounterValue mock_vkGetSemaphoreCounterValue
#define vkWaitSemaphores           mock_vkWaitSemaphores
#define vkQueueSubmit2             mock_vkQueueSubmit2
#define vkAllocateCommandBuffers   mock_vkAllocateCommandBuffers
#define vkResetCommandBuffer       mock_vkResetCommandBuffer
#define vkBeginCommandBuffer       mock_vkBeginCommandBuffer
#define vkEndCommandBuffer         mock_vkEndCommandBuffer
#define vkCmdBeginRendering        mock_vkCmdBeginRendering
#define vkCmdEndRendering          mock_vkCmdEndRendering
#define vkCmdSetViewport           mock_vkCmdSetViewport
#define vkCmdSetScissor            mock_vkCmdSetScissor
#define vkCmdPipelineBarrier2      mock_vkCmdPipelineBarrier2
#define vkCmdCopyImage2            mock_vkCmdCopyImage2

#include "../src/vulkan/memory/bda.c"
#include "../src/frame/present/orphan.c"
#include "../src/frame/present/barrier.c"
#include "../src/frame/present/record.c"

#undef vkCmdCopyImage2
#undef vkCmdPipelineBarrier2
#undef vkCmdSetScissor
#undef vkCmdSetViewport
#undef vkCmdEndRendering
#undef vkCmdBeginRendering
#undef vkEndCommandBuffer
#undef vkBeginCommandBuffer
#undef vkResetCommandBuffer
#undef vkAllocateCommandBuffers
#undef vkQueueSubmit2
#undef vkWaitSemaphores
#undef vkGetSemaphoreCounterValue
#undef destroy_raw_block
#undef create_raw_block

// Test EngineApp whose draw hook requests an oversized transient slice and
// verifies a null slice is skipped while the frame still presents cleanly.
static VulkanContext *s_app_vk               = nullptr;
static VkDeviceSize   s_app_req_transient    = 0u;
static uint32_t       s_app_draw_invocations = 0u;
static uint32_t       s_app_recorded_draws   = 0u;
static VkDeviceAddress s_app_last_gpu_addr   = 0u;

static int app_init(VulkanContext *vk, void (*relay)(const struct io_uring_cqe *, void *), void *user) {
    (void)vk; (void)relay; (void)user;
    return 0;
}

static void app_shutdown(VulkanContext *vk) { (void)vk; }

static void app_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool draw_live) {
    (void)cmd;
    (void)width;
    (void)height;
    (void)draw_live;
    s_app_draw_invocations++;
    BdaSlice slice = bda_alloc_transient(s_app_vk, s_app_req_transient, 64u);
    if (slice.cpu == nullptr || slice.gpu == 0ull) {
        return; // Null slice must skip recording any draw with a null device address!
    }
    s_app_last_gpu_addr = slice.gpu;
    s_app_recorded_draws++;
}

static bool app_advance(uint32_t time_ms, const FrameStep *step) {
    (void)time_ms; (void)step;
    return false;
}

static bool app_busy(void) { return false; }
static bool app_click(float x, float y) { (void)x; (void)y; return false; }
static bool app_key(uint32_t key, EngineKeyContext *ctx) { (void)key; (void)ctx; return false; }

static const EngineApp k_test_app = {
    .init     = app_init,
    .shutdown = app_shutdown,
    .draw     = app_draw,
    .advance  = app_advance,
    .busy     = app_busy,
    .click    = app_click,
    .key      = app_key,
};

const EngineApp *engine_app(void) {
    return &k_test_app;
}

static void test_transient_overflow_skips_draw_and_still_presents(void) {
    frame_state = (FrameState){
        .drm_fd       = -1,
        .acquire      = (VkSemaphore)(uintptr_t)0x55,
        .acquire_sync = (VkSemaphore)(uintptr_t)0x56,
    };
    VulkanContext vk = {
        .device   = (VkDevice)(uintptr_t)0x1,
        .cmd_pool = (VkCommandPool)(uintptr_t)0x2,
        .queues   = { .graphics_queue = (VkQueue)(uintptr_t)0x3 },
    };

    s_bda_sys = (BdaSystem){0};
    s_bda_sys.is_initialized          = true;
    s_bda_sys.vk                      = &vk;
    s_bda_sys.transient_ring.buffer   = (VkBuffer)(uintptr_t)0x77;
    s_bda_sys.transient_ring.capacity = 4096u; // 4 slices x 1024 B each
    s_bda_sys.transient_ring.base_cpu = malloc(4096u);
    s_bda_sys.transient_ring.base_gpu = 0x800000ull;
    bda_transient_reset();

    FrameSlot *slot = &frame_state.slot[0];
    blank_frame(slot);
    slot->image = (VulkanExportableImage){
        .image     = (VkImage)(uintptr_t)0x101,
        .width     = 1280u,
        .height    = 688u,
        .format    = VK_FORMAT_B8G8R8A8_SRGB,
        .dmabuf_fd = -1,
    };
    slot->image_view  = (VkImageView)(uintptr_t)0x202;
    slot->view_format = VK_FORMAT_B8G8R8A8_SRGB;

    s_app_vk               = &vk;
    s_app_req_transient    = 2048u; // Exceeds 1024-byte slice -> must return null BdaSlice!
    s_app_draw_invocations = 0u;
    s_app_recorded_draws   = 0u;
    s_app_last_gpu_addr    = 0u;
    s_mock_sem_counter     = 0u;
    s_mock_submit_result   = VK_SUCCESS;
    s_sem_wait_calls       = 0u;
    s_queue_submit_calls   = 0u;
    s_begin_render_calls   = 0u;
    s_end_render_calls     = 0u;
    s_end_cmd_calls        = 0u;
    s_publish_acq_calls    = 0u;

    uint64_t t0 = mono_ns_now();
    int rc = record_and_submit_clear(&vk, slot, frame_state.acquire, 1u, true, 8u);
    uint64_t elapsed_ns = mono_ns_now() - t0;

    CHECK(rc == 0);
    CHECK(elapsed_ns < 5000000ull);
    CHECK(s_sem_wait_calls == 0u);
    CHECK(s_app_draw_invocations == 1u);
    CHECK(s_app_recorded_draws == 0u); // Skipped null slice — zero null BDA draw!
    CHECK(s_app_last_gpu_addr == 0u);
    CHECK(s_begin_render_calls == 1u);
    CHECK(s_end_render_calls == 1u);
    CHECK(s_end_cmd_calls == 1u);
    CHECK(s_queue_submit_calls == 1u);
    CHECK(s_publish_acq_calls == 1u);
    CHECK(frame_state.gpu_submits == 1u);
    CHECK(slot->acquire_point == 1u);
    CHECK(frame_state.sync_point == 1u);
    CHECK(slot->has_valid_history);

    // Next frame with a fitting 256-byte slice records the draw and presents cleanly.
    s_app_req_transient = 256u;
    rc = record_and_submit_clear(&vk, slot, frame_state.acquire, 2u, true, 8u);
    CHECK(rc == 0);
    CHECK(s_app_draw_invocations == 2u);
    CHECK(s_app_recorded_draws == 1u);
    CHECK(s_app_last_gpu_addr == 0x800000ull + 2048ull);
    CHECK(frame_state.gpu_submits == 2u);

    free(s_bda_sys.transient_ring.base_cpu);
    s_bda_sys = (BdaSystem){0};
}

static void test_orphan_cap_refuses_slot_without_sliding_handles(void) {
    frame_state = (FrameState){
        .drm_fd     = -1,
        .orphans    = frame_state.orphans_inline,
        .orphan_cap = k_orphan_init_cap,
    };
    VulkanContext vk = { .device = (VkDevice)(uintptr_t)0x1 };
    s_mock_sem_counter  = 0u;
    s_released_mask     = 0ull;
    s_sem_wait_calls    = 0u;
    s_destroy_gpu_calls = 0u;

    // Retire 64 held frames (filling k_orphan_max_cap == 64 via 8 -> 16 -> 32 -> 64).
    for (uint32_t i = 0u; i < k_orphan_max_cap; ++i) {
        FrameSlot *src = &frame_state.slot[0];
        blank_frame(src);
        src->image = (VulkanExportableImage){
            .image     = (VkImage)(uintptr_t)(0x4000u + i),
            .width     = 1280u,
            .height    = 688u,
            .dmabuf_fd = -1,
        };
        src->image_view      = (VkImageView)(uintptr_t)(0x8000u + i);
        src->acquire_point   = 10u + i;
        src->release_point   = 100u + i;
        src->release_pending = true;

        CHECK(retire_pixels(nullptr, &vk, src) == 0);
    }

    CHECK(frame_state.orphan_count == k_orphan_max_cap);
    CHECK(frame_state.orphan_cap == k_orphan_max_cap);
    CHECK(frame_state.orphan_mask == UINT64_MAX);
    CHECK(s_sem_wait_calls == 0u);

    SlotRef refs[k_orphan_max_cap];
    uint32_t gens[k_orphan_max_cap];
    for (uint32_t i = 0u; i < k_orphan_max_cap; ++i) {
        refs[i] = slot_ref(&frame_state.orphans[i]);
        gens[i] = frame_state.orphans[i].generation;
        CHECK(refs[i] != 0u);
        CHECK(slot_from_ref(refs[i]) == &frame_state.orphans[i]);
        CHECK(frame_state.orphans[i].image.image == (VkImage)(uintptr_t)(0x4000u + i));
    }

    // Attempt a 65th orphan while all 64 are still held: must refuse (-1) immediately
    // with zero waits and MUST NOT slide or invalidate any of the 64 SlotRef handles!
    FrameSlot *extra = &frame_state.slot[1];
    blank_frame(extra);
    extra->image = (VulkanExportableImage){
        .image     = (VkImage)(uintptr_t)0x9999u,
        .width     = 1280u,
        .height    = 688u,
        .dmabuf_fd = -1,
    };
    extra->image_view      = (VkImageView)(uintptr_t)0x999au;
    extra->acquire_point   = 500u;
    extra->release_point   = 999u;
    extra->release_pending = true;

    uint64_t t0 = mono_ns_now();
    CHECK(make_orphan_room(nullptr, &vk) == -1);
    CHECK(retire_pixels(nullptr, &vk, extra) == -1);
    uint64_t elapsed_ns = mono_ns_now() - t0;
    CHECK(elapsed_ns < 5000000ull);
    CHECK(s_sem_wait_calls == 0u);
    CHECK(frame_state.orphan_count == k_orphan_max_cap);
    // Refused slot must keep its pixels intact rather than corrupting the orphan table
    CHECK(extra->image.image == (VkImage)(uintptr_t)0x9999u);

    for (uint32_t i = 0u; i < k_orphan_max_cap; ++i) {
        FrameSlot *resolved = slot_from_ref(refs[i]);
        CHECK(resolved == &frame_state.orphans[i]);
        CHECK(resolved->generation == gens[i]);
        CHECK(resolved->image.image == (VkImage)(uintptr_t)(0x4000u + i));
    }

    // Release & retire two middle slots (17 and 42): sweeping reaps ONLY those two
    // and every surviving handle [0..63] \ {17, 42} stays at its fixed index!
    s_released_mask    = (1ull << 17u) | (1ull << 42u);
    s_mock_sem_counter = 200u;
    CHECK(make_orphan_room(nullptr, &vk) == 0);
    CHECK(frame_state.orphan_count == k_orphan_max_cap - 2u);
    CHECK(slot_from_ref(refs[17]) == nullptr);
    CHECK(slot_from_ref(refs[42]) == nullptr);
    for (uint32_t i = 0u; i < k_orphan_max_cap; ++i) {
        if (i == 17u || i == 42u) {
            continue;
        }
        CHECK(slot_from_ref(refs[i]) == &frame_state.orphans[i]);
        CHECK(frame_state.orphans[i].image.image == (VkImage)(uintptr_t)(0x4000u + i));
    }

    if (frame_state.orphans != nullptr && frame_state.orphans != frame_state.orphans_inline) {
        free(frame_state.orphans);
        frame_state.orphans = nullptr;
    }
}

static uint32_t s_fault_query_calls = 0u;

static VKAPI_ATTR VkResult VKAPI_CALL mock_vkGetDeviceFaultInfoEXT(
    VkDevice                device,
    VkDeviceFaultCountsEXT *pFaultCounts,
    VkDeviceFaultInfoEXT   *pFaultInfo
) {
    (void)device;
    s_fault_query_calls++;
    if (pFaultCounts == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pFaultInfo == nullptr) {
        pFaultCounts->addressInfoCount = 1u;
        pFaultCounts->vendorInfoCount  = 1u;
        pFaultCounts->vendorBinarySize = 64u;
        return VK_SUCCESS;
    }
    snprintf(pFaultInfo->description, sizeof(pFaultInfo->description), "Mock page fault in unit test");
    if (pFaultInfo->pAddressInfos != nullptr && pFaultCounts->addressInfoCount >= 1u) {
        pFaultInfo->pAddressInfos[0] = (VkDeviceFaultAddressInfoEXT){
            .addressType      = VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT,
            .reportedAddress  = 0xdeadbeef0000ull,
            .addressPrecision = 4096ull,
        };
    }
    if (pFaultInfo->pVendorInfos != nullptr && pFaultCounts->vendorInfoCount >= 1u) {
        pFaultInfo->pVendorInfos[0] = (VkDeviceFaultVendorInfoEXT){
            .vendorFaultCode = 0x42ull,
            .vendorFaultData = 0x99ull,
        };
        snprintf(pFaultInfo->pVendorInfos[0].description,
                 sizeof(pFaultInfo->pVendorInfos[0].description),
                 "Mock EU hang");
    }
    return VK_SUCCESS;
}

static void test_device_lost_reports_and_returns_nonblocking(void) {
    // 1. Null / uninitialized contexts return immediately without crashing
    vulkan_report_device_fault(nullptr, "null_ctx");
    VulkanContext empty_vk = {0};
    vulkan_report_device_fault(&empty_vk, "null_device");

    // 2. VK_ERROR_DEVICE_LOST during frame_submit_slot_commands invokes
    //    vulkan_report_device_fault (querying VK_EXT_device_fault counts + info)
    //    and returns -1 cleanly in < 5 ms without incrementing gpu_submits.
    frame_state = (FrameState){
        .drm_fd       = -1,
        .acquire      = (VkSemaphore)(uintptr_t)0x55,
        .acquire_sync = (VkSemaphore)(uintptr_t)0x56,
    };
    VulkanContext vk = {
        .device = (VkDevice)(uintptr_t)0x1,
        .queues = { .graphics_queue = (VkQueue)(uintptr_t)0x3 },
        .device_info = {
            .supports_device_fault = true,
        },
        .dispatch = {
            .vkGetDeviceFaultInfoEXT = mock_vkGetDeviceFaultInfoEXT,
        },
    };
    snprintf(vk.device_info.name, sizeof(vk.device_info.name), "Mock GPU (Fault Test)");

    FrameSlot slot = {
        .cmd_buf         = (VkCommandBuffer)(uintptr_t)0xabc0,
        .release_eventfd = -1,
        .image           = { .dmabuf_fd = -1 },
    };

    s_fault_query_calls  = 0u;
    s_sem_wait_calls     = 0u;
    s_queue_submit_calls = 0u;
    s_mock_submit_result = VK_ERROR_DEVICE_LOST;

    uint64_t t0 = mono_ns_now();
    int rc = frame_submit_slot_commands(&vk, &slot, frame_state.acquire,
                                        1u, 0u, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    uint64_t elapsed_ns = mono_ns_now() - t0;

    CHECK(rc == -1);
    CHECK(elapsed_ns < 5000000ull);
    CHECK(s_sem_wait_calls == 0u);
    CHECK(s_queue_submit_calls == 1u);
    CHECK(s_fault_query_calls == 2u); // 1 counts call + 1 info call
    CHECK(frame_state.gpu_submits == 0u);
    CHECK(strcmp(vulkan_vkresult_to_str(VK_ERROR_DEVICE_LOST), "VK_ERROR_DEVICE_LOST") == 0);
}

int main(void) {
    test_transient_overflow_skips_draw_and_still_presents();
    test_orphan_cap_refuses_slot_without_sliding_handles();
    test_device_lost_reports_and_returns_nonblocking();

    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_failure_paths: all checks passed\n");
    return 0;
}
