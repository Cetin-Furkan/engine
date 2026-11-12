#include "input/internal.h"
#include "input/page.h"
#include "ring/ring.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Lock-free 4-slot published-index input ring + 2 per-frame GPU readback slots.
// Line 0: head_tag = (seq << 2) | slot_idx, stored with __ATOMIC_RELEASE after
// the CPU finishes populating slots[slot_idx]. Lines 1..4: 4 immutable-while-live
// 64-byte input snapshots. Line 5: 2 alternating 32-byte GPU readback slots so
// double-buffered frames in flight never race writing the same readback words.

typedef struct InputSlot {
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
} InputSlot;

typedef struct InputReadback {
    uint32_t gpu_seq;
    uint32_t gpu_time_ms;
    uint32_t gpu_flags;
    uint32_t gpu_mono_lo;
    uint32_t gpu_mono_hi;
    uint32_t gpu_commit;
    uint32_t gpu_pad[2];
} InputReadback;

typedef struct InputPageRing {
    alignas(64) uint32_t head_tag;
    uint32_t             ctrl_pad[15];
    InputSlot            slots[4];
    alignas(64) InputReadback readback[2];
} InputPageRing;

static_assert(sizeof(InputSlot) == 64u, "input slot is one cache line");
static_assert(sizeof(InputReadback) == 32u, "input readback is half a cache line");
static_assert(sizeof(InputPageRing) == 384u, "input ring is six cache lines");
static_assert(offsetof(InputSlot, time_ms) == 28u, "time word");
static_assert(offsetof(InputSlot, flags) == 32u, "flags word");
static_assert(offsetof(InputSlot, mono_lo) == 44u, "mono word");
static_assert(offsetof(InputSlot, commit) == 60u, "commit word");
static_assert(offsetof(InputPageRing, slots) == 64u, "slots offset");
static_assert(offsetof(InputPageRing, readback) == 320u, "readback offset");
static_assert(_Alignof(InputPageRing) >= 64u, "page ring align");

typedef struct InputPush {
    uint64_t page;
    uint32_t readback_slot;
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

static DaBuffer         s_buf;
static InputPageRing   *s_ring;
static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipe   = VK_NULL_HANDLE;
static uint32_t         s_pub_seq;
static uint32_t         s_rb_cursor;
static uint32_t         s_seen;

void input_page_publish(void) {
    InputPageRing *ring = s_ring;
    if (ring == nullptr) {
        return;
    }
    // Keep even non-zero sequence numbers in [2, 0x3ffffffe] so (seq << 2) | slot
    // fits in 32 bits and preserves the existing even-sequence telemetry contract.
    uint32_t seq = (s_pub_seq + 2u) & 0x3ffffffeu;
    if (seq == 0u) {
        seq = 2u;
    }
    s_pub_seq = seq;
    uint32_t slot_idx = (seq >> 1u) & 3u;
    InputSlot *slot = &ring->slots[slot_idx];

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

    slot->x = x;
    slot->y = y;
    slot->drag_x = drag_x;
    slot->drag_y = drag_y;
    slot->buttons = buttons;
    slot->focus = focus;
    slot->time_ms = sample_ms;
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
    slot->flags = flags;
    slot->held_n = held_n;
    slot->held0 = held0;
    uint64_t mono = ring_mono_ns();
    slot->mono_lo = (uint32_t)mono;
    slot->mono_hi = (uint32_t)(mono >> 32);
    slot->scroll = (float)(-scroll / 10.0);
    slot->drag_buttons = drag_buttons;
    uint32_t shape = s_ptr.cursor_inflight ? s_ptr.inflight_shape
                   : (s_ptr.have_last ? s_ptr.last_shape : s_ptr.wanted_shape);
    slot->commit = ((s_ptr.resize_edge & 0xffffu) << 16) | (shape & 0xffffu);
    __atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->head_tag, (seq << 2u) | slot_idx, __ATOMIC_RELEASE);
}

void input_page_record(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || s_pipe == VK_NULL_HANDLE || s_ring == nullptr || s_buf.address == 0) {
        return;
    }
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

    uint32_t rb_slot = s_rb_cursor++ & 1u;
    InputPush push = {
        .page          = s_buf.address,
        .readback_slot = rb_slot,
        .pad1          = 0u,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
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
    if (s_ring == nullptr) {
        return;
    }
    uint32_t s0 = __atomic_load_n(&s_ring->readback[0].gpu_seq, __ATOMIC_ACQUIRE);
    uint32_t s1 = __atomic_load_n(&s_ring->readback[1].gpu_seq, __ATOMIC_ACQUIRE);
    s_seen = (s1 > s0) ? s1 : s0;
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
    s_ring = nullptr;
    s_pub_seq = 0;
    s_rb_cursor = 0;
    s_seen = 0;
    s_buf = (DaBuffer){0};
    s_pipe = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (da_buffer_create(vk, sizeof(InputPageRing), usage, true, &s_buf) != 0 ||
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
    memset(s_buf.map, 0, sizeof(InputPageRing));
    s_ring = s_buf.map;

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
        s_ring = nullptr;
        return -1;
    }
    s_pipe = vulkan_compute_pipeline(vk, k_input_spv, sizeof k_input_spv, s_layout);
    if (s_pipe == VK_NULL_HANDLE) {
        destroy_pipe(vk);
        da_buffer_destroy(vk, &s_buf);
        s_ring = nullptr;
        return -1;
    }
    input_page_publish();
    return 0;
}

void input_gpu_shutdown(VulkanContext *vk) {
    s_ring = nullptr;
    destroy_pipe(vk);
    da_buffer_destroy(vk, &s_buf);
}
