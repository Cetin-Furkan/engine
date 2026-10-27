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

    // Missed refreshes are counted against the period learned before
    // this stamp; feeding the gap into the EMA first would excuse it.
    uint32_t period = frame_clock_period_ms(clock);
    if (period > 0 && raw_dt > period) {
        step.missed = (raw_dt + period / 2u) / period - 1u;
        clock->missed_total += step.missed;
    }
    if (period == 0) {
        clock->period_q8 = raw_dt << 8;
    } else {
        // EMA with alpha 1/8 in Q8: stable against one-off hitches,
        // still tracks a refresh-rate change within a few frames.
        uint32_t sample = raw_dt << 8;
        clock->period_q8 = clock->period_q8 - (clock->period_q8 >> 3) + (sample >> 3);
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
