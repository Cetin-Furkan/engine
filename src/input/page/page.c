#include "input/internal.h"
#include "input/page.h"
#include "ring/ring.h"
#include "vulkan/bda.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Lock-free 4-slot published-index input ring in host-coherent BDA memory.
// Line 0: head_tag = (seq << 2) | slot_idx, stored with __ATOMIC_RELEASE after
// the CPU finishes populating slots[slot_idx]. Lines 1..4: 4 immutable-while-live
// 64-byte input snapshots readable directly by any GPU shader via BDA (zero
// per-frame compute dispatch and zero HOST<->COMPUTE queue stalls).

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
    uint32_t             commit;
} InputSlot;

typedef struct InputPageRing {
    alignas(64) uint32_t head_tag;
    uint32_t             ctrl_pad[15];
    InputSlot            slots[4];
} InputPageRing;

static_assert(sizeof(InputSlot) == 64u, "input slot is one cache line");
static_assert(sizeof(InputPageRing) == 320u, "input ring is five cache lines");
static_assert(offsetof(InputSlot, time_ms) == 28u, "time word");
static_assert(offsetof(InputSlot, flags) == 32u, "flags word");
static_assert(offsetof(InputSlot, mono_lo) == 44u, "mono word");
static_assert(offsetof(InputSlot, commit) == 60u, "commit word");
static_assert(offsetof(InputPageRing, slots) == 64u, "slots offset");
static_assert(alignof(InputPageRing) >= 64u, "page ring align");

enum : uint32_t {
    k_flag_pointer  = 1u << 0,
    k_flag_key      = 1u << 1,
    k_flag_sealed   = 1u << 2,
    k_flag_dirty    = 1u << 3,
    k_flag_resize   = 1u << 4,
    k_flag_move     = 1u << 5,
    k_flag_inflight = 1u << 6,
};

static DaBuffer       s_buf;
static InputPageRing *s_ring;
static uint32_t       s_pub_seq;
static uint32_t       s_seen;

void input_page_publish(void) {
    InputPageRing *ring = s_ring;
    if (ring == nullptr) {
        return;
    }
    uint32_t seq = (s_pub_seq + 2u) & 0x3ffffffeu;
    if (seq == 0u) {
        seq = 2u;
    }
    s_pub_seq = seq;
    uint32_t slot_idx = (seq >> 1u) & 3u;
    InputSlot *slot = &ring->slots[slot_idx];

    uint32_t flags = 0u;
    if (s_ptr.focus != 0u)      flags |= k_flag_pointer;
    if (s_key_focus)            flags |= k_flag_key;
    if (s_ptr.cursor_sealed)    flags |= k_flag_sealed;
    if (s_ptr.cursor_dirty)     flags |= k_flag_dirty;
    if (s_ptr.resize_pending)   flags |= k_flag_resize;
    if (s_ptr.move_pending)     flags |= k_flag_move;
    if (s_ptr.cursor_inflight)  flags |= k_flag_inflight;

    uint64_t mono = ring_mono_ns();
    uint32_t shape = s_ptr.cursor_inflight ? s_ptr.inflight_shape
                   : (s_ptr.have_last ? s_ptr.last_shape : s_ptr.wanted_shape);

    *slot = (InputSlot){
        .seq          = 0u,
        .x            = s_ptr.x,
        .y            = s_ptr.y,
        .drag_x       = s_drag_dx,
        .drag_y       = s_drag_dy,
        .buttons      = s_ptr.buttons,
        .focus        = s_ptr.focus,
        .time_ms      = s_ptr.sample_ms,
        .flags        = flags,
        .held_n       = s_held_n,
        .held0        = s_held_n > 0u ? s_held[0] : 0u,
        .mono_lo      = (uint32_t)mono,
        .mono_hi      = (uint32_t)(mono >> 32u),
        .scroll       = (float)(-s_scroll / 10.0),
        .drag_buttons = s_drag_buttons,
        .commit       = ((s_ptr.resize_edge & 0xffffu) << 16u) | (shape & 0xffffu),
    };
    __atomic_store_n(&slot->seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&ring->head_tag, (seq << 2u) | slot_idx, __ATOMIC_RELEASE);
}

void input_page_harvest(void) {
    if (s_ring == nullptr) {
        return;
    }
    uint32_t tag = __atomic_load_n(&s_ring->head_tag, __ATOMIC_ACQUIRE);
    s_seen = tag >> 2u;
}

uint32_t input_page_seen(void) {
    return s_seen;
}

int input_gpu_init(VulkanContext *vk) {
    s_ring = nullptr;
    s_pub_seq = 0u;
    s_seen = 0u;
    s_buf = (DaBuffer){0};
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (da_buffer_create(vk, sizeof(InputPageRing), usage, true, &s_buf) != 0 ||
        s_buf.map == nullptr || s_buf.address == 0u) {
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
    input_page_publish();
    return 0;
}

void input_gpu_shutdown(VulkanContext *vk) {
    s_ring = nullptr;
    da_buffer_destroy(vk, &s_buf);
}
