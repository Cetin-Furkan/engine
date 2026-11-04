#include "frame/internal.h"

// A compositor stamp paces the next present. The first picture after idle
// is committed with its frame callback. A frameless callback is not a
// present: after a resize Mutter can sit on it, and the dent stays on
// screen until that callback arrives. The 8 ms timer lives in sim.c.

static int queue_frame_only(WaylandContext *wl, bool *queued);

void commit_turn_swap(WaylandContext *wl) {
    if (!frame_state.turn_swap_pending) {
        return;
    }
    spare()->release_point = frame_state.pending_release;
    spare()->release_pending = true;
    swap_front_spare();
    engine_app()->commit();
    if (wl != nullptr) {
        wl->buffer_id = shown()->wl_buffer_id;
        wl->release_timeline_id = shown()->release_timeline_id;
    }
    frame_state.turn_swap_pending = false;
}

static int queue_frame_only(WaylandContext *wl, bool *queued) {
    int q = wayland_queue_turn_commit(wl, 0, 0, 0, 0, 0, 0, 0, true, false, 0);
    if (q == 0) {
        *queued = true;
    }
    return q;
}

static bool spare_ready(VulkanContext *vk) {
    if (!acquire_point_retired(vk, spare()->acquire_point)) {
        return false;
    }
    // The acquire signal is ALL_COMMANDS, after the timestamp writes.
    // Read them before this slot is recorded again.
    gpu_timer_harvest(vk, spare());
    return !spare()->release_pending ||
           poll_release_handle(spare()->release_handle, spare()->release_point);
}

static bool present_chain_wanted(void) {
    return frame_state.scene_dirty || frame_state.sim_hot || frame_sim_wanted();
}

bool turn_wants_step(const WaylandContext *wl) {
    if (frame_state.turn_phase == k_turn_done) {
        return false;
    }
    if (frame_state.turn_phase == k_turn_arm) {
        return true;
    }
    if (wl == nullptr) {
        return false;
    }
    if (wl->frame_stamp_valid) {
        return true;
    }
    return frame_state.scene_dirty && wl->frame_cb_id == 0;
}

int frame_watchdog(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    *queued = false;
    if (wl == nullptr || vk == nullptr || frame_state.turn_phase != k_turn_run) {
        return 0;
    }

    if (wl->frame_stamp_valid) {
        return 0;  // A stamp is already waiting; not stalled.
    }
    // The first callback is still with the compositor. There is no
    // consumed stamp to measure, so this is not a stall. A missing
    // callback (id 0) still falls through and is re-armed.
    if (frame_state.last_stamp_mono_ns == 0 && wl->frame_cb_id != 0) {
        return 0;
    }
    // Stall is measured from the last consumed stamp, not from when
    // the timer was armed: between callbacks stamp_valid is false most
    // of the time on a healthy chain, and that is not a stall.
    uint64_t age_ms = frame_state.last_stamp_mono_ns == 0 ? UINT64_MAX
        : (ring_mono_ns() - frame_state.last_stamp_mono_ns) / 1000000ull;
    if (!frame_clock_stalled(&frame_state.clock, age_ms)) {
        return 0;
    }
    if (!frame_state.stall_reported) {
        frame_state.stall_reported = true;
        frame_clock_note_stall(&frame_state.clock);
        fprintf(stderr, "[Engine] Frame callback stalled (%u total). Re-arming.\n",
                frame_state.clock.stalls);
    }
    // Only queue a new frame callback if none is outstanding. Zeroing a live
    // id would abandon the object. A dirty scene is painted by turn_step;
    // a frameless callback here would postpone that buffer.
    if (wl->frame_cb_id == 0) {
        if (frame_state.scene_dirty) {
            return 0;
        }
        int q = queue_frame_only(wl, queued);
        if (q == -EALREADY) {
            *queued = false;
            return 0;
        }
        return q;
    }
    return 0;
}

// Record the spare and commit it. again arms wl_surface.frame on that same
// commit when the simulation still owes frames.
static int present_spare(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    frame_state.loop_was_busy = true;
    frame_state.scene_dirty = false;
    if (engine_app()->record_poke_pending()) {
        frame_state.sim_hot = true;
    }

    uint64_t rel = spare()->release_point;
    if (spare()->release_pending) {
        rel += 1u;
    }
    uint32_t heat_dt = frame_state.pending_heat;
    uint32_t sim_stamp = frame_state.pending_sim_stamp != 0
        ? frame_state.pending_sim_stamp
        : sim_wall_ms();
    frame_state.pending_heat = 0;
    frame_state.pending_sim_stamp = 0;
    uint64_t next = frame_state.sync_point + 1u;
    if (record_and_submit_clear(vk, spare(), frame_state.acquire, next, true, heat_dt) != 0) {
        frame_state.scene_dirty = true;
        frame_state.pending_heat = heat_dt;
        frame_state.pending_sim_stamp = sim_stamp;
        return -1;
    }
    engine_app()->simulation_commit(sim_stamp, heat_dt);
    bool again = present_chain_wanted();
    int q = wayland_queue_turn_commit(
        wl,
        spare()->wl_buffer_id,
        spare()->image.width,
        spare()->image.height,
        wl->acquire_timeline_id,
        next,
        spare()->release_timeline_id,
        rel,
        again,
        false,
        0);
    if (q != 0) {
        return -1;
    }
    frame_state.pending_release = rel;
    frame_state.turn_swap_pending = true;
    *queued = true;
    return 0;
}

int turn_step(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    *queued = false;
    if (wl == nullptr || vk == nullptr) {
        return -EINVAL;
    }
    // A suspended toplevel is not on a scanout. Leave the timer and the
    // frame callback disarmed until the compositor configures it again.
    if ((wl->toplevel_states & ENG_TOP_SUSPENDED) != 0) {
        wl->frame_stamp_valid = false;
        return 0;
    }
    if (!frame_state.spare_live && alloc_spare(wl, vk) != 0) {
        return -1;
    }
    if (frame_state.turn_phase == k_turn_arm) {
        frame_state.turn_phase = k_turn_run;
    }
    if (wl->frame_stamp_valid) {
        if (!spare_ready(vk)) {
            // The stamp stays. Replacing it with a frameless callback never
            // attaches a buffer, so the held image is never released.
            return 0;
        }
        uint32_t stamp = wl->frame_time_ms;
        frame_state.last_stamp_mono_ns = ring_mono_ns();
        frame_state.stall_reported = false;
        // The stamp paces the present and the stall watchdog. It does not
        // integrate the pose or the solver; sim_on_timer does that.
        (void)frame_clock_step(&frame_state.clock, stamp);
        wl->frame_stamp_valid = false;
        if (!frame_state.scene_dirty) {
            note_sim_idle();
            if (present_chain_wanted() && wl->frame_cb_id == 0) {
                return queue_frame_only(wl, queued);
            }
            return 0;
        }
        return present_spare(wl, vk, queued);
    }
    // No callback is in flight. Commit the damaged buffer now, with the
    // callback on that same commit when another frame is owed.
    if (!frame_state.scene_dirty || wl->frame_cb_id != 0) {
        return 0;
    }
    if (!spare_ready(vk)) {
        return 0;
    }
    frame_state.last_stamp_mono_ns = ring_mono_ns();
    frame_state.stall_reported = false;
    return present_spare(wl, vk, queued);
}
