#include "frame/frame_int.h"

// One callback step. The swap waits for the Wayland send completion.

static int queue_frame_only(WaylandContext *wl, bool *queued);

void commit_turn_swap(WaylandContext *wl) {
    if (!s_turn_swap_pending) {
        return;
    }
    spare()->release_point = s_pending_release;
    spare()->release_pending = true;
    swap_front_spare();
    world_commit();
    if (wl != nullptr) {
        wl->buffer_id = shown()->wl_buffer_id;
        wl->release_timeline_id = shown()->release_timeline_id;
    }
    s_turn_swap_pending = false;
}

static int queue_frame_only(WaylandContext *wl, bool *queued) {
    int q = wayland_queue_turn_commit(wl, 0, 0, 0, 0, 0, 0, 0, true, false, 0);
    if (q == 0) {
        *queued = true;
    }
    return q;
}

bool turn_wants_step(const WaylandContext *wl) {
    if (s_turn_phase == k_turn_done) {
        return false;
    }
    if (s_turn_phase == k_turn_arm) {
        return true;
    }
    return wl != nullptr && wl->frame_stamp_valid;
}

int turn_step(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    *queued = false;
    if (wl == nullptr || vk == nullptr) {
        return -EINVAL;
    }
    if (s_turn_phase == k_turn_done) {
        wl->frame_stamp_valid = false;
        return 0;
    }
    if (!s_spare_live && alloc_spare(wl, vk) != 0) {
        return -1;
    }
    if (s_turn_phase == k_turn_arm) {
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            s_turn_phase = k_turn_run;
            return 0;
        }
        return q;
    }
    if (!wl->frame_stamp_valid) {
        return 0;
    }
    uint32_t stamp = wl->frame_time_ms;
    uint32_t heat_dt = 0;
    bool pulse = world_simulation(stamp, &heat_dt);
    bool changed = world_advance(stamp);
    // The pose can stay still. An open pulse still presents, because the
    // spot and the diffusion change the picture. The cube's yaw does not move.
    bool paint = changed || (pulse && (heat_dt > 0u || world_record_poke_pending()));
    if (!paint) {
        if (!world_busy() && !pulse) {
            wl->frame_stamp_valid = false;
            s_turn_phase = k_turn_done;
            retire_spare(wl, vk);
            uint32_t frames = 0;
            uint32_t missed = 0;
            bool spun = world_consume_spin();
            hud_turn_report(&frames, &missed);
            if (spun) {
                printf("[Engine] Turn idle. Pose repeated. Frame callback dropped.\n");
                printf("[Engine] %u frames, %u missed.\n", frames, missed);
                fflush(stdout);
                world_simulation_hold();
            }
            return 0;
        }
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            wl->frame_stamp_valid = false;
            return 0;
        }
        return q;
    }

    bool spare_free = acquire_point_retired(vk, spare()->acquire_point) &&
        (!spare()->release_pending ||
         poll_release_semaphore(vk, spare()->release_semaphore, spare()->release_point));
    if (!spare_free) {
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            wl->frame_stamp_valid = false;
            return 0;
        }
        return q;
    }

    // A resize stores the point it presented and leaves release_pending false,
    // so this bump runs once per present.
    uint64_t rel = spare()->release_point;
    if (spare()->release_pending) {
        rel += 1u;
    }
    uint64_t next = s_sync_point + 1u;
    if (record_and_submit_clear(vk, spare(), s_acquire, next, true, heat_dt) != 0) {
        spare()->acquire_point = 0;
        return -1;
    }
    world_simulation_commit(stamp, heat_dt);
    s_sync_point = next;
    int q = wayland_queue_turn_commit(
        wl,
        spare()->wl_buffer_id,
        spare()->image.width,
        spare()->image.height,
        wl->acquire_timeline_id,
        next,
        spare()->release_timeline_id,
        rel,
        true,
        false,
        0);
    if (q != 0) {
        return -1;
    }
    wl->frame_stamp_valid = false;
    s_pending_release = rel;
    s_turn_swap_pending = true;
    *queued = true;
    return 0;
}
