#include "frame/frame_int.h"

// One callback step. The swap waits for the Wayland send completion.

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

bool turn_wants_step(const WaylandContext *wl) {
    if (frame_state.turn_phase == k_turn_done) {
        return false;
    }
    if (frame_state.turn_phase == k_turn_arm) {
        return true;
    }
    return wl != nullptr && wl->frame_stamp_valid;
}

int frame_watchdog(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    *queued = false;
    if (wl == nullptr || vk == nullptr || frame_state.turn_phase != k_turn_run) {
        return 0;
    }

    if (engine_app()->busy()) {
        uint64_t now_ns = ring_mono_ns();
        if (frame_state.last_sim_mono_ns == 0) {
            frame_state.last_sim_mono_ns = now_ns;
        }
        uint32_t dt_ms = (uint32_t)((now_ns - frame_state.last_sim_mono_ns) / 1000000ull);
        if (dt_ms >= 8u) {
            frame_state.last_sim_mono_ns = now_ns;
            uint32_t synth_stamp = (uint32_t)(now_ns / 1000000ull);
            uint32_t steps = dt_ms / 8u;
            if (steps > 5u) {
                steps = 5u;
            }
            for (uint32_t s = 0; s < steps; ++s) {
                uint32_t sub = synth_stamp - (steps - 1u - s) * 8u;
                FrameStep synth_step = { .dt_ms = 8u, .missed = 0, .paint = false, .first = false };
                uint32_t heat_dt = 0;
                (void)engine_app()->simulation(sub, &synth_step, &heat_dt);
                (void)engine_app()->advance(sub, &synth_step);
            }
        }
    }

    if (wl->frame_stamp_valid) {
        return 0;  // A stamp is already waiting; not stalled.
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
    // Only queue a new frame callback if none is outstanding on the compositor.
    // If wl->frame_cb_id != 0, the compositor has the callback armed and will fire it
    // when visible; zeroing it would abandon the callback object and leak IDs!
    if (wl->frame_cb_id == 0) {
        int q = queue_frame_only(wl, queued);
        if (q == -EALREADY) {
            *queued = false;
            return 0;
        }
        return q;
    }
    return 0;
}

int turn_step(WaylandContext *wl, VulkanContext *vk, bool *queued) {
    *queued = false;
    if (wl == nullptr || vk == nullptr) {
        return -EINVAL;
    }
    // Continuous loop: a frame callback is always outstanding while the
    // window is presented. k_turn_done no longer parks the loop; it is
    // only the boot state before the first callback is armed.
    if (!frame_state.spare_live && alloc_spare(wl, vk) != 0) {
        return -1;
    }
    if (frame_state.turn_phase == k_turn_arm) {
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            frame_state.turn_phase = k_turn_run;
            return 0;
        }
        return q;
    }
    if (!wl->frame_stamp_valid) {
        return 0;
    }
    uint32_t stamp = wl->frame_time_ms;
    frame_state.last_stamp_mono_ns = ring_mono_ns();
    frame_state.last_sim_mono_ns   = frame_state.last_stamp_mono_ns;
    frame_state.stall_reported = false;
    FrameStep step = frame_clock_step(&frame_state.clock, stamp);
    if (!step.paint) {
        // Duplicate compositor stamp: the pose cannot have moved.
        // Keep the turn armed and wait for a real stamp.
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            wl->frame_stamp_valid = false;
            return 0;
        }
        return q;
    }
    uint32_t heat_dt = 0;
    bool pulse = engine_app()->simulation(stamp, &step, &heat_dt);
    // Fixed-timestep simulation: 8 ms steps (125 Hz) consumed from the
    // clock's accumulator, at most 5 per callback. Poses are evaluated
    // at synthetic sub-stamps ending exactly on the compositor stamp,
    // so the hosted app keeps its absolute-stamp math while the engine
    // owns step size, clamping, and overload behaviour.
    bool changed = false;
    uint32_t steps = frame_clock_fixed_steps(&frame_state.clock, step.dt_ms, 8u, 5u);
    if (step.first || steps == 0) {
        changed = engine_app()->advance(stamp, &step);
    } else {
        for (uint32_t i = 0; i < steps; ++i) {
            uint32_t sub = stamp - (steps - 1u - i) * 8u;
            if (engine_app()->advance(sub, &step)) {
                changed = true;
            }
        }
    }
    // The pose can stay still. An open pulse still presents, because the
    // spot and the diffusion change the picture. The cube's yaw does not move.
    bool paint = changed || step.first ||
                 (pulse && (heat_dt > 0u || engine_app()->record_poke_pending()));
    if (!paint) {
        if (!engine_app()->busy() && !pulse) {
            // Idle edge, not loop exit: report once, hold the pulse, and
            // keep the callback chain alive. The spare stays allocated;
            // retiring it here would realloc on the next interaction.
            wl->frame_stamp_valid = false;
            if (frame_state.loop_was_busy) {
                frame_state.loop_was_busy = false;
                uint32_t frames = 0;
                uint32_t missed = 0;
                bool spun = engine_app()->consume_spin();
                engine_app()->turn_report(&frames, &missed);
                if (spun) {
                    printf("[Engine] Loop idle. %u frames, %u missed, %u stalls.\n",
                           frames, missed, frame_state.clock.stalls);
                    fflush(stdout);
                    engine_app()->simulation_hold();
                }
            }
            int q = queue_frame_only(wl, queued);
            if (q == 0 || q == -EALREADY) {
                wl->frame_stamp_valid = false;
                return 0;
            }
            return q;
        }
        int q = queue_frame_only(wl, queued);
        if (q == 0 || q == -EALREADY) {
            wl->frame_stamp_valid = false;
            return 0;
        }
        return q;
    }

    frame_state.loop_was_busy = true;
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
    uint64_t next = frame_state.sync_point + 1u;
    if (record_and_submit_clear(vk, spare(), frame_state.acquire, next, true, heat_dt) != 0) {
        spare()->acquire_point = 0;
        return -1;
    }
    engine_app()->simulation_commit(stamp, heat_dt);
    frame_state.sync_point = next;
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
    frame_state.pending_release = rel;
    frame_state.turn_swap_pending = true;
    *queued = true;
    return 0;
}
