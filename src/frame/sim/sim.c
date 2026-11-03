#include "frame/frame.h"
#include "frame/internal.h"
#include <sys/timerfd.h>

// The 8 ms timerfd. A compositor stamp does not call into this file.
// Pose and the solver share the quanta frame_clock_issue writes.

uint32_t sim_wall_ms(void) {
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

void note_sim_idle(void) {
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
        uint32_t compute_us = 0;
        uint32_t render_us = 0;
        uint32_t total_us = 0;
        frame_gpu_timings(&compute_us, &render_us, &total_us);
        printf("[Engine] Loop idle. %u frames, %u missed, %u stalls (GPU avg: %u us total, %u us compute, %u us render).\n",
               frames, missed, frame_state.clock.stalls,
               total_us, compute_us, render_us);
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

// Hold keys are polled. A press does not note a picture, so the timer
// has to start from the key still being down. camera_active is that poll.
static bool camera_wants_steps(void) {
    const EngineApp *app = engine_app();
    return app->camera_active != nullptr && app->camera_active();
}

bool frame_sim_wanted(void) {
    return engine_app()->busy() || engine_app()->record_poke_pending() || camera_wants_steps();
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

static void rearm_sim_if_dead(RingContext *ring, bool alive, int res, bool rearm) {
    if (rearm && !alive && res != -ECANCELED && res != -EBADF) {
        (void)frame_arm_sim_mshot(ring);
    }
}

int frame_arm_sim_mshot(RingContext *ring) {
    if (ring == nullptr || frame_state.sim_fd < 0) {
        return -EINVAL;
    }
    if (frame_state.sim_mshot_armed) {
        return 0;
    }
    int rc = ring_arm_read_mshot(ring, ENG_SIM_FD_SLOT, frame_state.sim_fixed, frame_state.sim_fd,
                                 ENG_SIM_BGID, ENG_UD_SIM_TIMER);
    if (rc == 0) {
        frame_state.sim_mshot_armed = true;
    }
    return rc;
}

bool frame_note_sim_cqe(RingContext *ring, const struct io_uring_cqe *cqe, bool rearm) {
    if (cqe == nullptr || cqe->user_data != ENG_UD_SIM_TIMER) {
        return false;
    }
    uint64_t expirations = 0;
    bool have = ring_take_sim_count(ring, cqe, &expirations);
    bool alive = (cqe->flags & IORING_CQE_F_MORE) != 0;
    frame_state.sim_mshot_armed = alive;
    if (have && expirations > 0) {
        sim_on_timer(expirations);
    }
    rearm_sim_if_dead(ring, alive, cqe->res, rearm);
    return true;
}

void frame_apply_noted_sim(RingContext *ring, uint64_t expirations, int res, bool alive, bool rearm) {
    frame_state.sim_mshot_armed = alive;
    if (expirations > 0) {
        sim_on_timer(expirations);
    }
    rearm_sim_if_dead(ring, alive, res, rearm);
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
        uint32_t wall = sim_wall_ms();
        uint32_t heat = 0;
        pulse = engine_app()->simulation(wall, &step, &heat);
        // The world returns the whole interval still waiting for a present.
        // The cube already sums ticks into that value. Adding it here would
        // integrate the same milliseconds twice.
        frame_state.pending_heat = heat;
        if (heat > 0) {
            frame_state.pending_sim_stamp = wall;
            frame_state.scene_dirty = true;
        } else if (pulse && engine_app()->record_poke_pending()) {
            frame_state.pending_sim_stamp = wall;
            frame_state.scene_dirty = true;
        }
        for (uint32_t i = 0; i < steps; ++i) {
            if (engine_app()->advance(stamps[i], &step)) {
                frame_state.scene_dirty = true;
            }
        }
    }
    frame_state.sim_hot = pulse;
    note_sim_idle();
}
