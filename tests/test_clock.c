#include "frame/clock.h"
#include <stdio.h>

// Focused regression test for the compositor frame clock: baseline,
// intervals, duplicate-stamp suppression, missed-refresh counting,
// pathological-gap clamping, and the stall deadline. Exercises the
// exact frame_clock_* code the turn loop runs.

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

int main(void) {
    FrameClock clock;
    frame_clock_reset(&clock);
    CHECK(frame_clock_period_ms(&clock) == 0);

    // Baseline stamp paints and establishes the clock.
    FrameStep first = frame_clock_step(&clock, 1000);
    CHECK(first.first && first.paint && first.dt_ms == 0);

    // Duplicate stamp never paints twice.
    FrameStep dup = frame_clock_step(&clock, 1000);
    CHECK(!dup.paint && !dup.first);

    // Steady 16 ms cadence learns the period, no misses.
    FrameStep a = frame_clock_step(&clock, 1016);
    FrameStep b = frame_clock_step(&clock, 1032);
    CHECK(a.paint && a.dt_ms == 16 && a.missed == 0);
    CHECK(b.paint && b.dt_ms == 16);
    CHECK(frame_clock_period_ms(&clock) == 16);

    // A 64 ms gap at 16 ms period skips three refreshes.
    FrameStep gap = frame_clock_step(&clock, 1096);
    CHECK(gap.paint && gap.missed == 3);
    CHECK(clock.missed_total == 3);

    // A huge gap is clamped so animation cannot fast-forward, and neither
    // the learned refresh period nor missed_total is corrupted by occlusion.
    FrameStep huge = frame_clock_step(&clock, 6096);
    CHECK(huge.paint && huge.dt_ms == 100 && huge.missed == 0);
    CHECK(frame_clock_period_ms(&clock) == 16);
    CHECK(clock.missed_total == 3);

    // Stall deadline is 3 periods clamped to [50, 500] ms.
    uint32_t stall = frame_clock_stall_after_ms(&clock);
    CHECK(stall >= 50 && stall <= 500);
    frame_clock_note_stall(&clock);
    CHECK(clock.stalls == 1);

    // Fixed steps: remainder carries, cap drops debt (no spiral).
    FrameClock fixed;
    frame_clock_reset(&fixed);
    CHECK(frame_clock_fixed_steps(&fixed, 20, 8, 5) == 2);
    CHECK(fixed.accum_ms == 4);
    CHECK(frame_clock_fixed_steps(&fixed, 4, 8, 5) == 1);
    CHECK(fixed.accum_ms == 0);
    CHECK(frame_clock_fixed_steps(&fixed, 100, 8, 5) == 5);
    CHECK(fixed.accum_ms == 0);
    CHECK(fixed.sim_steps == 8);
    CHECK(frame_clock_fixed_steps(&fixed, 16, 0, 5) == 0);

    // Issued stamps are the sim cursor, 8 ms apart, and a 100 ms hitch
    // stops at 40 ms. The debt is not appended to the last stamp.
    CHECK(FRAME_SIM_STEP_MS == 8u);
    CHECK(FRAME_SIM_MAX_STEPS == 5u);
    FrameClock issued;
    frame_clock_reset(&issued);
    uint32_t stamps[FRAME_SIM_MAX_STEPS] = {0};
    CHECK(frame_clock_issue(&issued, 16u, stamps) == 2u);
    CHECK(stamps[0] == 8u && stamps[1] == 16u);
    CHECK(frame_clock_sim_stamp(&issued) == 16u);
    CHECK(frame_clock_issue(&issued, 4u, stamps) == 0u);
    CHECK(frame_clock_sim_stamp(&issued) == 16u);
    CHECK(frame_clock_issue(&issued, 4u, stamps) == 1u);
    CHECK(stamps[0] == 24u);
    CHECK(frame_clock_sim_stamp(&issued) == 24u);
    CHECK(frame_clock_issue(&issued, 100u, stamps) == 5u);
    CHECK(stamps[0] == 32u && stamps[4] == 64u);
    CHECK(frame_clock_sim_stamp(&issued) == 64u);
    CHECK(issued.accum_ms == 0u);

    // Compositor refresh stamps (frame_clock_step) must NEVER advance the
    // issued simulation cursor, and a 12-expiration (96 ms) timer wake must
    // clamp to FRAME_SIM_MAX_STEPS == 5 (sim_dt_ms == 40 ms) with exact 8 ms
    // stamps ending on frame_clock_sim_stamp.
    (void)frame_clock_step(&issued, 5000u);
    (void)frame_clock_step(&issued, 5016u);
    (void)frame_clock_step(&issued, 5032u);
    CHECK(frame_clock_sim_stamp(&issued) == 64u);

    uint64_t raw_expirations = 12u;
    uint64_t clamped_exp = raw_expirations > FRAME_SIM_MAX_STEPS
        ? FRAME_SIM_MAX_STEPS
        : raw_expirations;
    CHECK(clamped_exp == 5u);
    uint32_t wake_dt = (uint32_t)clamped_exp * FRAME_SIM_STEP_MS;
    uint32_t wake_steps = frame_clock_issue(&issued, wake_dt, stamps);
    FrameStep sim_step = {
        .dt_ms     = wake_dt,
        .paint     = wake_steps > 0u,
        .sim_dt_ms = wake_steps * FRAME_SIM_STEP_MS,
    };
    CHECK(wake_steps == FRAME_SIM_MAX_STEPS);
    CHECK(sim_step.paint && sim_step.sim_dt_ms == 40u);
    for (uint32_t i = 0u; i < wake_steps; ++i) {
        CHECK(stamps[i] == 64u + (i + 1u) * FRAME_SIM_STEP_MS);
    }
    CHECK(stamps[wake_steps - 1u] == frame_clock_sim_stamp(&issued));
    CHECK(issued.accum_ms == 0u);

    // Stall decision: no stamp yet is stalled; a stamp older than two
    // stall windows (2 * 48 ms at 16 ms period) is stalled; fresher is not.
    FrameClock pacing;
    frame_clock_reset(&pacing);
    CHECK(frame_clock_stalled(&pacing, 0));
    (void)frame_clock_step(&pacing, 1000);
    (void)frame_clock_step(&pacing, 1016);
    (void)frame_clock_step(&pacing, 1032);
    CHECK(!frame_clock_stalled(&pacing, 50));
    CHECK(frame_clock_stalled(&pacing, 500));

    // The first GPU sample is stored. The next keeps seven eighths.
    frame_clock_note_gpu(&clock, 80u, 800u, 1000u);
    CHECK(clock.gpu_compute_us == 80u && clock.gpu_render_us == 800u && clock.gpu_total_us == 1000u);
    frame_clock_note_gpu(&clock, 80u, 800u, 1000u);
    CHECK(clock.gpu_total_us == 1000u);
    frame_clock_note_gpu(&clock, 160u, 800u, 1000u);
    CHECK(clock.gpu_compute_us == ((80u * 7u + 160u) >> 3));

    // Reset returns to baseline behaviour, and sustained 120 Hz -> 60 Hz
    // cadence shift still converges smoothly to 16 ms.
    frame_clock_reset(&clock);
    FrameStep again = frame_clock_step(&clock, 42);
    CHECK(again.first && again.paint);
    (void)frame_clock_step(&clock, 50);
    (void)frame_clock_step(&clock, 58);
    CHECK(frame_clock_period_ms(&clock) == 8);
    uint32_t t = 58;
    for (int i = 0; i < 24; ++i) {
        t += 16;
        (void)frame_clock_step(&clock, t);
    }
    CHECK(frame_clock_period_ms(&clock) == 16);

    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_clock: all checks passed\n");
    return 0;
}
