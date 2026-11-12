#include "frame/frame.h"
#include "frame/internal.h"

// Native kernel multishot IORING_OP_TIMEOUT (8 ms period). A compositor stamp
// does not call into this file. Pose and the solver share the quanta
// frame_clock_issue writes.

uint32_t sim_wall_ms(void) {
    uint64_t now = ring_mono_ns();
    if (frame_state.mono0_ns == 0) {
        frame_state.mono0_ns = now;
    }
    // Offset by +1 ms so 0 is strictly reserved as the "unset" sentinel.
    uint64_t ms = (now - frame_state.mono0_ns) / 1000000ull + 1ull;
    if (ms > 0xffffffffull) {
        ms = 0xffffffffull;
    }
    return (uint32_t)ms;
}

void note_sim_idle(void) {
    if (frame_sim_wanted() || frame_state.sim_hot || frame_state.scene_dirty ||
        frame_state.sim_timer_on || frame_state.sim_mshot_armed ||
        frame_state.sim_inflight_count > 0u || frame_state.sim_cancel_inflight > 0u) {
        return;
    }
    if (!frame_state.loop_was_busy) {
        return;
    }
    frame_state.loop_was_busy = false;
    const EngineApp *app = engine_app();
    bool spun = (app != nullptr && app->consume_spin != nullptr) ? app->consume_spin() : false;
    uint32_t frames = frame_state.turn_frames;
    uint32_t missed = frame_state.clock.missed_total;
    frame_state.turn_frames = 0u;
    if (app != nullptr && app->turn_report != nullptr) {
        uint32_t app_frames = 0u;
        uint32_t app_missed = 0u;
        app->turn_report(&app_frames, &app_missed);
        if (app_frames > 0u) {
            frames = app_frames;
            missed = app_missed;
        }
    }
    uint32_t compute_us = 0;
    uint32_t render_us = 0;
    uint32_t total_us = 0;
    frame_gpu_timings(&compute_us, &render_us, &total_us);
    printf("[Engine] Loop idle. %u frames, %u missed, %u stalls, %llu submits, sim_timer=%u (GPU avg: %u us total, %u us compute, %u us render).\n",
           frames, missed, frame_state.clock.stalls,
           (unsigned long long)frame_state.gpu_submits,
           (frame_state.sim_timer_on || frame_state.sim_mshot_armed) ? 1u : 0u,
           total_us, compute_us, render_us);
    fflush(stdout);
    if (spun && app != nullptr && app->simulation_hold != nullptr) {
        app->simulation_hold();
    }
}

void frame_note_picture(void) {
    // The click plants the poke before it returns. The present that follows
    // consumes that poke, and without this latch the same turn disarms the
    // 8 ms timer. The dent is then one frozen picture.
    frame_note_damage_rect(0, 0, UINT32_MAX, UINT32_MAX);
}

// Hold keys are polled. A press does not note a picture, so the timer
// has to start from the key still being down. camera_active is that poll.
static bool camera_wants_steps(void) {
    const EngineApp *app = engine_app();
    return app != nullptr && app->camera_active != nullptr && app->camera_active();
}

bool frame_sim_wanted(void) {
    const EngineApp *app = engine_app();
    bool app_busy = app != nullptr && app->busy != nullptr && app->busy();
    bool app_poke = app != nullptr && app->record_poke_pending != nullptr && app->record_poke_pending();
    return app_busy || app_poke || camera_wants_steps();
}

static int arm_sim_timeout(RingContext *ring) {
    if (ring == nullptr || !ring->is_initialized) {
        return -EINVAL;
    }
    if (frame_state.sim_mshot_armed) {
        return 0;
    }
    uint32_t gen = ++frame_state.sim_gen;
    if (gen == 0u) {
        gen = ++frame_state.sim_gen;
    }
    uint64_t ud = ring_ud_pack((uint16_t)ENG_UD_SIM_TIMER, 0u, gen);
    int rc = ring_arm_timeout_multishot(ring, (uint64_t)FRAME_SIM_STEP_MS * 1000000ull, ud);
    if (rc == 0) {
        frame_state.sim_active_ud   = ud;
        frame_state.sim_mshot_armed = true;
        frame_state.sim_cancel_sent = false;
        frame_state.sim_inflight_count++;
    }
    return rc;
}

void sim_timer_set(RingContext *ring, bool want) {
    if (want) {
        frame_state.sim_timer_on = true;
        if (!frame_state.sim_mshot_armed) {
            (void)arm_sim_timeout(ring);
        }
        return;
    }
    frame_state.sim_timer_on = false;
    if (frame_state.sim_mshot_armed && !frame_state.sim_cancel_sent &&
        ring != nullptr && ring->is_initialized) {
        if (ring_remove_timeout(ring, frame_state.sim_active_ud, ENG_UD_SIM_CANCEL, false) == 0) {
            frame_state.sim_cancel_sent = true;
            frame_state.sim_cancel_inflight++;
        }
    }
}

bool frame_note_sim_cqe(RingContext *ring, const struct io_uring_cqe *cqe,
                        bool rearm, uint64_t *io_ticks) {
    if (cqe == nullptr) {
        return false;
    }
    if (cqe->user_data == ENG_UD_SIM_CANCEL) {
        if (frame_state.sim_cancel_inflight > 0u) {
            frame_state.sim_cancel_inflight--;
        }
        return true;
    }
    if (ring_ud_tag(cqe->user_data) != (uint16_t)ENG_UD_SIM_TIMER) {
        return false;
    }
    bool alive = (cqe->flags & IORING_CQE_F_MORE) != 0;
    if (!alive && frame_state.sim_inflight_count > 0u) {
        frame_state.sim_inflight_count--;
    }
    if (cqe->user_data != frame_state.sim_active_ud) {
        return true;
    }
    frame_state.sim_mshot_armed = alive;
    if (!alive) {
        frame_state.sim_cancel_sent = false;
    }
    if (frame_state.sim_timer_on && cqe->res == -ETIME) {
        if (io_ticks != nullptr) {
            (*io_ticks)++;
        } else {
            sim_on_timer(1u);
        }
    }
    if (rearm && frame_state.sim_timer_on && !alive &&
        cqe->res != -ECANCELED && cqe->res != -EINVAL) {
        (void)arm_sim_timeout(ring);
    }
    return true;
}

void frame_apply_noted_sim(RingContext *ring, uint64_t last_ud, uint64_t expirations,
                           uint32_t retired, int res, bool alive, bool rearm) {
    if (retired > 0u) {
        if (frame_state.sim_inflight_count >= retired) {
            frame_state.sim_inflight_count -= retired;
        } else {
            frame_state.sim_inflight_count = 0u;
        }
    }
    if (last_ud != 0ull && last_ud == frame_state.sim_active_ud) {
        frame_state.sim_mshot_armed = alive;
        if (!alive) {
            frame_state.sim_cancel_sent = false;
        }
        if (frame_state.sim_timer_on && expirations > 0u) {
            sim_on_timer(expirations);
        }
        if (rearm && frame_state.sim_timer_on && !alive &&
            res != -ECANCELED && res != -EINVAL) {
            (void)arm_sim_timeout(ring);
        }
    }
}

void sim_on_timer(uint64_t expirations) {
    const EngineApp *app = engine_app();
    frame_state.loop_was_busy = true;
    if (!frame_state.sim_latched) {
        FrameStep first = { .first = true, .paint = true };
        if (app != nullptr && app->advance != nullptr &&
            app->advance(frame_clock_sim_stamp(&frame_state.clock), &first)) {
            frame_state.scene_dirty = true;
            if (!frame_state.pending_damage_set) {
                frame_state.pending_damage_full = true;
                frame_state.pending_damage_set  = true;
            }
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
        if (app != nullptr && app->simulation != nullptr) {
            pulse = app->simulation(wall, &step, &heat);
        }
        // The world returns the whole interval still waiting for a present.
        // The cube already sums ticks into that value. Adding it here would
        // integrate the same milliseconds twice.
        frame_state.pending_heat = heat;
        if (heat > 0) {
            frame_state.pending_sim_stamp = wall;
            frame_note_picture();
        } else if (pulse && app != nullptr && app->record_poke_pending != nullptr &&
                   app->record_poke_pending()) {
            frame_state.pending_sim_stamp = wall;
            frame_note_picture();
        }
        for (uint32_t i = 0; i < steps; ++i) {
            if (app != nullptr && app->advance != nullptr &&
                app->advance(stamps[i], &step)) {
                frame_state.scene_dirty = true;
                if (!frame_state.pending_damage_set) {
                    frame_state.pending_damage_full = true;
                    frame_state.pending_damage_set  = true;
                }
            }
        }
    }
    frame_state.sim_hot = pulse;
    note_sim_idle();
}
