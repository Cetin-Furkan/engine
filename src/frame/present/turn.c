#include "frame/frame_int.h"
#include <poll.h>
#include <sys/timerfd.h>

// The 8 ms timer advances the simulation. A compositor stamp paces the next
// present. The first picture after idle is committed with its frame callback.
// A frameless callback is not a present: after a resize Mutter can sit on it,
// and the dent stays on screen until that callback arrives.

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
    return acquire_point_retired(vk, spare()->acquire_point) &&
        (!spare()->release_pending ||
         poll_release_semaphore(vk, spare()->release_semaphore, spare()->release_point));
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

static uint32_t sim_wall_ms(void) {
    uint64_t now = ring_mono_ns();
    if (frame_state.mono0_ns == 0) {
        frame_state.mono0_ns = now;
    }
    uint64_t ms = (now - frame_state.mono0_ns) / 1000000ull;
    if (ms > 0xffffffffull) {
        ms = 0xffffffffull;
    }
    return (uint32_t)ms;
}

static void note_sim_idle(void) {
    if (frame_sim_wanted() || frame_state.sim_hot || frame_state.scene_dirty) {
        return;
    }
    if (!frame_state.loop_was_busy) {
        return;
    }
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

void frame_note_picture(void) {
    frame_state.scene_dirty = true;
    // The click plants the poke before it returns. The present that follows
    // consumes that poke, and without this latch the same turn disarms the
    // 8 ms timer. The dent is then one frozen picture.
    if (engine_app()->record_poke_pending()) {
        frame_state.sim_hot = true;
    }
}

bool frame_sim_wanted(void) {
    return engine_app()->busy() || engine_app()->record_poke_pending();
}

void sim_timer_set(RingContext *ring, bool want) {
    (void)ring;
    if (frame_state.sim_fd < 0 || want == frame_state.sim_timer_on) {
        return;
    }
    struct itimerspec spec = {0};
    if (want) {
        spec.it_interval.tv_nsec = (long)FRAME_SIM_STEP_MS * 1000000L;
        spec.it_value.tv_nsec = (long)FRAME_SIM_STEP_MS * 1000000L;
    }
    if (timerfd_settime(frame_state.sim_fd, 0, &spec, nullptr) != 0) {
        fprintf(stderr, "[Engine Error] sim timerfd: %s\n", strerror(errno));
        return;
    }
    frame_state.sim_timer_on = want;
}

int frame_arm_sim_poll(RingContext *ring) {
    if (ring == nullptr || frame_state.sim_fd < 0) {
        return -EINVAL;
    }
    if (frame_state.sim_poll_armed) {
        return 0;
    }
    int rc = frame_state.sim_fixed
        ? ring_arm_poll_fixed(ring, ENG_SIM_FD_SLOT, POLLIN, ENG_UD_SIM_TIMER)
        : ring_arm_poll(ring, frame_state.sim_fd, POLLIN, ENG_UD_SIM_TIMER);
    if (rc == 0) {
        frame_state.sim_poll_armed = true;
    }
    return rc;
}

bool frame_note_sim_cqe(RingContext *ring, const struct io_uring_cqe *cqe, bool rearm) {
    if (cqe == nullptr || cqe->user_data != ENG_UD_SIM_TIMER) {
        return false;
    }
    frame_state.sim_poll_armed = false;
    if (cqe->res >= 0 && (cqe->res & POLLIN) != 0 && frame_state.sim_fd >= 0) {
        uint64_t expirations = 0;
        ssize_t n = -1;
        do {
            n = read(frame_state.sim_fd, &expirations, sizeof expirations);
        } while (n < 0 && errno == EINTR);
        if (n == (ssize_t)sizeof expirations && expirations > 0) {
            sim_on_timer(expirations);
        }
    }
    if (rearm && cqe->res >= 0) {
        (void)frame_arm_sim_poll(ring);
    }
    return true;
}

void sim_on_timer(uint64_t expirations) {
    if (!frame_state.sim_latched) {
        FrameStep first = { .first = true, .paint = true };
        if (engine_app()->advance(frame_clock_sim_stamp(&frame_state.clock), &first)) {
            frame_state.scene_dirty = true;
        }
        frame_state.sim_latched = true;
    }
    if (expirations == 0) {
        expirations = 1;
    }
    if (expirations > FRAME_SIM_MAX_STEPS) {
        expirations = FRAME_SIM_MAX_STEPS;
    }
    uint32_t dt = (uint32_t)expirations * FRAME_SIM_STEP_MS;
    uint32_t stamps[FRAME_SIM_MAX_STEPS];
    uint32_t steps = frame_clock_issue(&frame_state.clock, dt, stamps);
    FrameStep step = {
        .dt_ms     = dt,
        .paint     = steps > 0,
        .sim_dt_ms = steps * FRAME_SIM_STEP_MS,
    };
    bool pulse = false;
    if (steps > 0) {
        uint32_t heat = 0;
        pulse = engine_app()->simulation(sim_wall_ms(), &step, &heat);
        frame_state.pending_heat = heat;
        for (uint32_t i = 0; i < steps; ++i) {
            if (engine_app()->advance(stamps[i], &step)) {
                frame_state.scene_dirty = true;
            }
        }
    }
    if (pulse) {
        frame_state.scene_dirty = true;
    }
    frame_state.sim_hot = pulse;
    note_sim_idle();
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
    frame_state.pending_heat = 0;
    uint64_t next = frame_state.sync_point + 1u;
    if (record_and_submit_clear(vk, spare(), frame_state.acquire, next, true, heat_dt) != 0) {
        spare()->acquire_point = 0;
        frame_state.scene_dirty = true;
        frame_state.pending_heat = heat_dt;
        return -1;
    }
    engine_app()->simulation_commit(sim_wall_ms(), heat_dt);
    frame_state.sync_point = next;
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
