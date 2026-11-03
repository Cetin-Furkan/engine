#include "frame/internal.h"

// Images the compositor still holds. A SlotRef names a fixed slot in this
// table, so sweeping one orphan does not move the others. The last copy of
// a release syncobj destroys it. Shutdown may block in a send. The running
// loop must not: a second ENG_UD_WL_SEND steals the completion the idle
// loop is waiting on.

static bool s_sync_destroy = false;

static void destroy_object_wire(WaylandContext *wl, uint32_t id) {
    if (wl == nullptr || !wl->is_initialized || wl->failed || id == 0) {
        return;
    }
    bool idle = s_sync_destroy && !wl->send_queued && !wl->noted_send && wl->send_len == 0;
    if (!idle) {
        if (!wayland_defer_destroy(wl, id)) {
            fprintf(stderr, "[Engine Error] Wayland object %u destroy was not queued\n", id);
        }
        return;
    }
    if (!wayland_put_raw(wl, id, 0u, nullptr, 0u)) {
        (void)wayland_defer_destroy(wl, id);
        return;
    }
    if (wayland_send_batch(wl) < 0 && wl->send_len == 0) {
        (void)wayland_defer_destroy(wl, id);
    }
}

static void destroy_wl_buffer_now(WaylandContext *wl, uint32_t buffer_id) {
    destroy_object_wire(wl, buffer_id);
    if (wl != nullptr && wl->buffer_id == buffer_id) {
        wl->buffer_id = 0;
    }
}

static bool same_release(const FrameSlot *a, const FrameSlot *b) {
    if (a->release_semaphore != VK_NULL_HANDLE &&
        a->release_semaphore == b->release_semaphore) {
        return true;
    }
    if (a->release_timeline_id != 0 && a->release_timeline_id == b->release_timeline_id) {
        return true;
    }
    if (a->release_handle != 0 && a->release_handle == b->release_handle) {
        return true;
    }
    return false;
}

static bool release_held_elsewhere(const FrameSlot *frame) {
    for (int i = 0; i < 2; ++i) {
        if (&frame_state.slot[i] != frame && same_release(frame, &frame_state.slot[i])) {
            return true;
        }
        // A resized bar stashes the old image here and keeps the same
        // release syncobj on the live bar slot. Reaping the orphan must
        // not destroy that semaphore.
        if (&frame_state.bar_slot[i] != frame && same_release(frame, &frame_state.bar_slot[i])) {
            return true;
        }
    }
    if (&frame_state.resize_fresh != frame && same_release(frame, &frame_state.resize_fresh)) {
        return true;
    }
    uint32_t mask = frame_state.orphan_mask;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        if (&frame_state.orphans[i] != frame && same_release(frame, &frame_state.orphans[i])) {
            return true;
        }
    }
    return false;
}

void reap_frame_resources(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return;
    }
    if (frame->release_eventfd >= 0) {
        close(frame->release_eventfd);
        frame->release_eventfd = -1;
    }
    if (frame->wl_buffer_id != 0) {
        destroy_wl_buffer_now(wl, frame->wl_buffer_id);
        frame->wl_buffer_id = 0;
    }
    // Several copies poll one semaphore: the slot, and each resized image
    // still on screen. The last copy destroys the Wayland timeline, the DRM
    // syncobj, and the Vulkan semaphore.
    bool last_release = (frame->release_semaphore != VK_NULL_HANDLE ||
                         frame->release_timeline_id != 0 ||
                         frame->release_handle != 0) &&
                        !release_held_elsewhere(frame);
    if (last_release) {
        if (frame->release_timeline_id != 0 && wl != nullptr && wl->is_initialized && !wl->failed) {
            uint32_t id = frame->release_timeline_id;
            destroy_object_wire(wl, id);
            if (wl->release_timeline_id == id) {
                wl->release_timeline_id = 0;
            }
        }
        drm_syncobj_destroy(frame_state.drm_fd, &frame->release_handle);
        if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
            frame->release_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk->device, frame->release_semaphore, nullptr);
        }
    }
    frame->release_timeline_id = 0;
    frame->release_handle = 0;
    frame->release_semaphore = VK_NULL_HANDLE;
    destroy_frame_gpu(vk, frame);
    frame->release_point = 0;
    frame->release_pending = false;
}

static void reap_orphan_slot(WaylandContext *wl, VulkanContext *vk, uint32_t idx) {
    FrameSlot *orb = &frame_state.orphans[idx];
    reap_frame_resources(wl, vk, orb);
    *orb = (FrameSlot){
        .image           = { .dmabuf_fd = -1 },
        .generation      = 0,
        .release_eventfd = -1,
    };
    frame_state.orphan_mask &= ~(1u << idx);
    frame_state.orphan_count = (uint32_t)__builtin_popcount(frame_state.orphan_mask);
}

static int insert_orphan(const FrameSlot *src) {
    uint32_t free_mask = (~frame_state.orphan_mask) & ((1u << k_orphan_cap) - 1u);
    if (free_mask == 0u) {
        return -1;
    }
    uint32_t idx = (uint32_t)__builtin_ctz(free_mask);
    uint32_t gen = slot_next_generation(frame_state.orphan_gen[idx]);
    if (src->generation >= gen) {
        gen = slot_next_generation(src->generation);
    }
    frame_state.orphan_gen[idx] = gen;
    frame_state.orphans[idx] = *src;
    frame_state.orphans[idx].generation = gen;
    frame_state.orphan_mask |= (1u << idx);
    frame_state.orphan_count = (uint32_t)__builtin_popcount(frame_state.orphan_mask);
    return (int)idx;
}

void sweep_ready_orphans(WaylandContext *wl, VulkanContext *vk, bool force_all) {
    bool previous = s_sync_destroy;
    s_sync_destroy = force_all;
    uint32_t mask = frame_state.orphan_mask;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        // Resolve through the generation-tagged handle: if this orphan
        // was replaced since the sweep started, the ref is stale and
        // the entry must not be reaped under its new image.
        FrameSlot *orb = slot_from_ref(slot_ref(&frame_state.orphans[i]));
        if (orb == nullptr) {
            continue;
        }
        bool ready = poll_release_semaphore(vk, orb->release_semaphore, orb->release_point);
        if (!ready && force_all) {
            (void)wait_own_release(vk, orb, 3000000000ull);
            ready = poll_release_semaphore(vk, orb->release_semaphore, orb->release_point);
            if (!ready) {
                fprintf(stderr,
                        "[Engine Error] Release point %llu was not signalled before shutdown\n",
                        (unsigned long long)orb->release_point);
            }
        }
        if (ready) {
            reap_orphan_slot(wl, vk, i);
        }
    }
    s_sync_destroy = previous;
}

int make_orphan_room(WaylandContext *wl, VulkanContext *vk) {
    sweep_ready_orphans(wl, vk, false);
    if (frame_state.orphan_count < k_orphan_cap) {
        return 0;
    }
    uint32_t mask = frame_state.orphan_mask;
    if (mask == 0u) {
        return 0;
    }
    uint32_t oldest_idx = (uint32_t)__builtin_ctz(mask);
    FrameSlot *oldest = &frame_state.orphans[oldest_idx];
    mask &= mask - 1u;
    while (mask != 0u) {
        uint32_t i = (uint32_t)__builtin_ctz(mask);
        mask &= mask - 1u;
        if (frame_state.orphans[i].acquire_point < oldest->acquire_point ||
            (frame_state.orphans[i].acquire_point == oldest->acquire_point &&
             frame_state.orphans[i].release_point < oldest->release_point)) {
            oldest_idx = i;
            oldest = &frame_state.orphans[i];
        }
    }
    (void)wait_own_release(vk, oldest, 500000000ull);
    if (!poll_release_semaphore(vk, oldest->release_semaphore, oldest->release_point)) {
        return -1;
    }
    reap_orphan_slot(wl, vk, oldest_idx);
    return 0;
}

static int stash_orphan(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return 0;
    }
    if (make_orphan_room(wl, vk) != 0) {
        return -1;
    }
    // The copy keeps the release syncobj. The source must not destroy it.
    if (insert_orphan(frame) < 0) {
        return -1;
    }
    frame->release_handle = 0;
    frame->release_semaphore = VK_NULL_HANDLE;
    frame->release_timeline_id = 0;
    frame->release_point = 0;
    frame->release_eventfd = -1;
    frame->release_pending = false;
    clear_pixels(frame);
    return 0;
}

// Drop the images and the wl_buffer. The release syncobj stays on the slot.
// A buffer the compositor still holds is orphaned at the old point and polls
// the same semaphore. It must not destroy that semaphore. A pending release
// moves this slot's point forward by one.
int retire_pixels(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame) {
    if (frame == nullptr) {
        return 0;
    }
    bool pending = frame->release_pending;
    uint64_t old_point = frame->release_point;
    if (!slot_has_pixels(frame)) {
        if (pending && old_point != 0) {
            frame->release_point = old_point + 1u;
        }
        frame->release_pending = false;
        return 0;
    }

    bool held = pending &&
                !poll_release_semaphore(vk, frame->release_semaphore, old_point);
    if (held && make_orphan_room(wl, vk) != 0) {
        return -1;
    }

    FrameSlot pixels = *frame;
    pixels.release_eventfd = -1;
    pixels.release_pending = false;

    clear_pixels(frame);
    frame->release_pending = false;
    if (pending && old_point != 0) {
        frame->release_point = old_point + 1u;
    }

    if (!held) {
        pixels.release_handle = 0;
        pixels.release_semaphore = VK_NULL_HANDLE;
        pixels.release_timeline_id = 0;
        pixels.release_point = 0;
        reap_frame_resources(wl, vk, &pixels);
        return 0;
    }

    pixels.release_point = old_point;
    if (insert_orphan(&pixels) < 0) {
        return -1;
    }
    return 0;
}

void retire_spare(WaylandContext *wl, VulkanContext *vk) {
    if (!frame_state.spare_live) {
        return;
    }
    bool held = spare()->release_pending &&
                !poll_release_semaphore(vk, spare()->release_semaphore, spare()->release_point);
    if (held) {
        if (stash_orphan(wl, vk, spare()) != 0) {
            fprintf(stderr,
                    "[Engine Error] Spare image left alive. Eight release points are still held.\n");
            return;
        }
    } else {
        reap_frame_resources(wl, vk, spare());
    }
    blank_frame(spare());
    frame_state.spare_live = false;
}
