#include "input/internal.h"
#include "input/page.h"
#include "ring/ring.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Two lines. The seat publishes the first. The scene dispatch writes the
// second with the sequence it loaded, and frame_stop reads that after the
// device idle. Word 0 is the sequence: odd while the fields are in flux.

typedef struct InputPage {
    alignas(64) uint32_t seq;
    float                x;
    float                y;
    float                drag_x;
    float                drag_y;
    uint32_t             buttons;
    uint32_t             focus;
    uint32_t             time_ms;
    uint32_t             flags;
    uint32_t             held_n;
    uint32_t             held0;
    uint32_t             mono_lo;
    uint32_t             mono_hi;
    float                scroll;
    uint32_t             drag_buttons;
    // Low 16: shape on the wire (inflight, else last, else wanted).
    // High 16: xdg resize edge of the latch encode just sent or is holding.
    uint32_t             commit;
    uint32_t             gpu_seq;
    uint32_t             gpu_time_ms;
    uint32_t             gpu_flags;
    uint32_t             gpu_mono_lo;
    uint32_t             gpu_mono_hi;
    uint32_t             gpu_pad[11];
} InputPage;

static_assert(sizeof(InputPage) == 128u, "input page is two cache lines");
static_assert(offsetof(InputPage, time_ms) == 28u, "time word");
static_assert(offsetof(InputPage, flags) == 32u, "flags word");
static_assert(offsetof(InputPage, mono_lo) == 44u, "mono word");
static_assert(offsetof(InputPage, commit) == 60u, "commit word");
static_assert(offsetof(InputPage, gpu_seq) == 64u, "gpu line");
static_assert(_Alignof(InputPage) >= 64u, "page align");

typedef struct InputPush {
    uint64_t page;
    uint32_t pad0;
    uint32_t pad1;
} InputPush;

static_assert(sizeof(InputPush) == 16u, "input push constant");

alignas(uint32_t) static const unsigned char k_input_spv[] = {
#embed "input.comp.spv"
};
static_assert(sizeof k_input_spv % 4u == 0u, "input spirv");

enum : uint32_t {
    k_flag_pointer  = 1u << 0,
    k_flag_key      = 1u << 1,
    k_flag_sealed   = 1u << 2,
    k_flag_dirty    = 1u << 3,
    k_flag_resize   = 1u << 4,
    k_flag_move     = 1u << 5,
    k_flag_inflight = 1u << 6,
};

static DaBuffer   s_buf;
static InputPage *s_page;
static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipe   = VK_NULL_HANDLE;
static uint32_t         s_seen;

void input_page_publish(void) {
    InputPage *page = s_page;
    if (page == nullptr) {
        return;
    }
    uint32_t seq = __atomic_load_n(&page->seq, __ATOMIC_RELAXED);
    uint32_t odd = seq + 1u;
    if ((odd & 1u) == 0u) {
        odd += 1u;
    }
    __atomic_store_n(&page->seq, odd, __ATOMIC_RELAXED);
    float x = s_ptr.x;
    float y = s_ptr.y;
    float drag_x = s_drag_dx;
    float drag_y = s_drag_dy;
    uint32_t buttons = s_ptr.buttons;
    uint32_t focus = s_ptr.focus;
    uint32_t sample_ms = s_ptr.sample_ms;
    uint32_t held_n = s_held_n;
    uint32_t held0 = held_n > 0u ? s_held[0] : 0u;
    uint32_t drag_buttons = s_drag_buttons;
    double scroll = s_scroll;
    bool key_focus = s_key_focus;
    page->x = x;
    page->y = y;
    page->drag_x = drag_x;
    page->drag_y = drag_y;
    page->buttons = buttons;
    page->focus = focus;
    page->time_ms = sample_ms;
    uint32_t flags = 0;
    if (focus != 0u) {
        flags |= k_flag_pointer;
    }
    if (key_focus) {
        flags |= k_flag_key;
    }
    if (s_ptr.cursor_sealed) {
        flags |= k_flag_sealed;
    }
    if (s_ptr.cursor_dirty) {
        flags |= k_flag_dirty;
    }
    if (s_ptr.resize_pending) {
        flags |= k_flag_resize;
    }
    if (s_ptr.move_pending) {
        flags |= k_flag_move;
    }
    if (s_ptr.cursor_inflight) {
        flags |= k_flag_inflight;
    }
    page->flags = flags;
    page->held_n = held_n;
    page->held0 = held0;
    uint64_t mono = ring_mono_ns();
    page->mono_lo = (uint32_t)mono;
    page->mono_hi = (uint32_t)(mono >> 32);
    page->scroll = (float)(-scroll / 10.0);
    page->drag_buttons = drag_buttons;
    uint32_t shape = s_ptr.cursor_inflight ? s_ptr.inflight_shape
                   : (s_ptr.have_last ? s_ptr.last_shape : s_ptr.wanted_shape);
    page->commit = ((s_ptr.resize_edge & 0xffffu) << 16) | (shape & 0xffffu);
    __atomic_store_n(&page->seq, odd + 1u, __ATOMIC_RELEASE);
}

void input_page_record(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || s_pipe == VK_NULL_HANDLE || s_page == nullptr || s_buf.address == 0) {
        return;
    }
    // Host writes landed before this submit. The second barrier publishes
    // the dispatch's store to the host for the idle harvest.
    VkMemoryBarrier2 host_to_shader = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
        .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo before = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &host_to_shader,
    };
    vkCmdPipelineBarrier2(cmd, &before);

    InputPush push = { .page = s_buf.address };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdDispatch(cmd, 1, 1, 1);

    VkMemoryBarrier2 shader_to_host = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo after = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &shader_to_host,
    };
    vkCmdPipelineBarrier2(cmd, &after);
}

void input_page_harvest(void) {
    if (s_page == nullptr) {
        return;
    }
    s_seen = __atomic_load_n(&s_page->gpu_seq, __ATOMIC_ACQUIRE);
}

uint32_t input_page_seen(void) {
    return s_seen;
}

static void destroy_pipe(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_pipe = VK_NULL_HANDLE;
        s_layout = VK_NULL_HANDLE;
        return;
    }
    if (s_pipe != VK_NULL_HANDLE) {
        vulkan_pipe_release(vk, s_pipe);
        s_pipe = VK_NULL_HANDLE;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
}

int input_gpu_init(VulkanContext *vk) {
    s_page = nullptr;
    s_seen = 0;
    s_buf = (DaBuffer){0};
    s_pipe = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (da_buffer_create(vk, sizeof(InputPage), usage, true, &s_buf) != 0 ||
        s_buf.map == nullptr || s_buf.address == 0) {
        fprintf(stderr, "[Pointer Error] Input page was not allocated\n");
        da_buffer_destroy(vk, &s_buf);
        return -1;
    }
    if (((uintptr_t)s_buf.map & 63u) != 0u) {
        fprintf(stderr, "[Pointer Error] Input page is not cache-line aligned\n");
        da_buffer_destroy(vk, &s_buf);
        return -1;
    }
    memset(s_buf.map, 0, sizeof(InputPage));
    s_page = s_buf.map;

    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(InputPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Pointer Error] Input pipeline layout failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        da_buffer_destroy(vk, &s_buf);
        s_page = nullptr;
        return -1;
    }
    s_pipe = vulkan_compute_pipeline(vk, k_input_spv, sizeof k_input_spv, s_layout);
    if (s_pipe == VK_NULL_HANDLE) {
        destroy_pipe(vk);
        da_buffer_destroy(vk, &s_buf);
        s_page = nullptr;
        return -1;
    }
    input_page_publish();
    return 0;
}

void input_gpu_shutdown(VulkanContext *vk) {
    s_page = nullptr;
    destroy_pipe(vk);
    da_buffer_destroy(vk, &s_buf);
}
