#include "frame/clock.h"

// A stall longer than this is not a refresh gap; it is a clock restart
// (occlusion, compositor switch). The interval is clamped so one stamp
// can never fast-forward animation by seconds.
enum : uint32_t { k_max_dt_ms = 100 };

void frame_clock_reset(FrameClock *clock) {
    *clock = (FrameClock){0};
}

uint32_t frame_clock_period_ms(const FrameClock *clock) {
    if (clock == nullptr || clock->period_q8 == 0) {
        return 0;
    }
    return (clock->period_q8 + 128u) >> 8;
}

FrameStep frame_clock_step(FrameClock *clock, uint32_t stamp_ms) {
    FrameStep step = {0};
    if (clock == nullptr) {
        return step;
    }
    if (!clock->have_stamp) {
        clock->have_stamp    = true;
        clock->last_stamp_ms = stamp_ms;
        clock->frames++;
        step.paint = true;
        step.first = true;
        return step;
    }
    // Unsigned subtraction is the wrap-correct interval on a 32-bit
    // millisecond clock (compositor stamps wrap every ~49 days).
    uint32_t raw_dt = stamp_ms - clock->last_stamp_ms;
    if (raw_dt == 0) {
        return step;  // Duplicate stamp: never paint the same pose twice.
    }
    clock->last_stamp_ms = stamp_ms;
    clock->frames++;

    // Missed refreshes and refresh-period learning only run within normal
    // cadence intervals. Multi-second occlusions or compositor switches
    // (> k_max_dt_ms) are clock restarts and must not poison period_q8 or
    // inflate missed_total.
    if (raw_dt <= k_max_dt_ms) {
        uint32_t period = frame_clock_period_ms(clock);
        if (period > 0 && raw_dt > period) {
            step.missed = (raw_dt + period / 2u) / period - 1u;
            clock->missed_total += step.missed;
        }
        if (period == 0) {
            clock->period_q8 = raw_dt << 8;
            clock->prev_long = false;
        } else if (step.missed == 0) {
            // Normal cadence (including 16/17 ms integer rounding jitter) or
            // refresh upshift (e.g. 60 Hz -> 120 Hz): update EMA directly.
            uint32_t sample = raw_dt << 8;
            clock->period_q8 = clock->period_q8 - (clock->period_q8 >> 3) + (sample >> 3);
            clock->prev_long = false;
        } else {
            // Whole refresh(es) missed: ignore a single isolated hitch so one
            // dropped VBlank never poisons period_q8, while adapting smoothly
            // if 2+ consecutive frames run at a slower cadence (120 -> 60 Hz).
            if (clock->prev_long) {
                uint32_t sample = raw_dt << 8;
                clock->period_q8 = clock->period_q8 - (clock->period_q8 >> 3) + (sample >> 3);
            }
            clock->prev_long = true;
        }
    } else {
        clock->prev_long = false;
    }
    step.dt_ms = raw_dt > k_max_dt_ms ? k_max_dt_ms : raw_dt;
    step.paint = true;
    return step;
}

uint32_t frame_clock_stall_after_ms(const FrameClock *clock) {
    uint32_t period = frame_clock_period_ms(clock);
    uint32_t after = period * 3u;
    if (after < 50u) {
        after = 50u;
    }
    if (after > 500u) {
        after = 500u;
    }
    return after;
}

bool frame_clock_stalled(const FrameClock *clock, uint64_t stamp_age_ms) {
    if (clock == nullptr || !clock->have_stamp) {
        return true;
    }
    return stamp_age_ms > 2ull * (uint64_t)frame_clock_stall_after_ms(clock);
}

void frame_clock_note_stall(FrameClock *clock) {
    if (clock != nullptr) {
        clock->stalls++;
    }
}

uint32_t frame_clock_sim_stamp(const FrameClock *clock) {
    if (clock == nullptr) {
        return 0;
    }
    return clock->sim_ms;
}

uint32_t frame_clock_issue(FrameClock *clock, uint32_t dt_ms, uint32_t *out_stamps) {
    uint32_t steps = frame_clock_fixed_steps(clock, dt_ms, FRAME_SIM_STEP_MS, FRAME_SIM_MAX_STEPS);
    uint32_t base = clock == nullptr ? 0u : clock->sim_ms;
    if (clock != nullptr) {
        clock->sim_ms = base + steps * FRAME_SIM_STEP_MS;
    }
    if (out_stamps != nullptr) {
        for (uint32_t i = 0; i < steps; ++i) {
            out_stamps[i] = base + (i + 1u) * FRAME_SIM_STEP_MS;
        }
    }
    return steps;
}

uint32_t frame_clock_fixed_steps(FrameClock *clock, uint32_t dt_ms,
                                 uint32_t step_ms, uint32_t max_steps) {
    if (clock == nullptr || step_ms == 0 || max_steps == 0) {
        return 0;
    }
    clock->accum_ms += dt_ms;
    uint32_t steps = clock->accum_ms / step_ms;
    if (steps > max_steps) {
        steps = max_steps;
        clock->accum_ms = 0;
    } else {
        clock->accum_ms -= steps * step_ms;
    }
    clock->sim_steps += steps;
    return steps;
}

void frame_clock_note_gpu(FrameClock *clock, uint32_t compute_us,
                          uint32_t render_us, uint32_t total_us) {
    if (clock == nullptr) {
        return;
    }
    if (clock->gpu_total_us == 0) {
        clock->gpu_compute_us = compute_us;
        clock->gpu_render_us  = render_us;
        clock->gpu_total_us   = total_us;
        return;
    }
    clock->gpu_compute_us = (clock->gpu_compute_us * 7u + compute_us) >> 3;
    clock->gpu_render_us  = (clock->gpu_render_us * 7u + render_us) >> 3;
    clock->gpu_total_us   = (clock->gpu_total_us * 7u + total_us) >> 3;
}
