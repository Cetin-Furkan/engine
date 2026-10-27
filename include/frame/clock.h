#ifndef ENGINE_FRAME_CLOCK_H
#define ENGINE_FRAME_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Compositor-clock frame pacer. One 64-byte cache-line cell holds the
// whole pacing state (the stateful-cell idea: state that is always read
// together should live together, in one line, not scattered across the
// frame state). Pure logic, no io_uring, no Vulkan: unit-tested.
//
// The compositor's wl_surface.frame timestamp is the only clock that
// matters for animation. This cell learns the refresh period from those
// stamps, clamps pathological gaps, counts missed refreshes, and tells
// the turn when a stamp is a duplicate that must not paint again.

typedef struct FrameStep {
    uint32_t dt_ms;   // Clamped interval since the previous stamp.
    uint32_t missed;  // Whole refreshes skipped inside that interval.
    bool     paint;   // False for a duplicate stamp (dt == 0 after baseline).
    bool     first;   // True for the baseline stamp (establishes the clock).
} FrameStep;

typedef struct FrameClock {
    uint32_t last_stamp_ms;
    uint32_t period_q8;  // Learned period, ms * 256 (fixed point EMA).
    uint32_t frames;
    uint32_t missed_total;
    uint32_t stalls;
    bool     have_stamp;
    uint32_t accum_ms;     // Fixed-step accumulator (carried remainder).
    uint32_t sim_steps;    // Fixed steps issued since reset.
    uint8_t  reserved[31];
} FrameClock;

static_assert(sizeof(FrameClock) == 64, "frame clock is one cache line");

void frame_clock_reset(FrameClock *clock);

// Fold one compositor stamp in. The returned step drives the turn:
// paint decides whether a new image is recorded this callback.
FrameStep frame_clock_step(FrameClock *clock, uint32_t stamp_ms);

// Learned refresh period in whole ms (0 before the second stamp).
uint32_t frame_clock_period_ms(const FrameClock *clock);

// Absolute stamp deadline after which the turn is considered stalled:
// last stamp + 3 periods, clamped to [50, 500] ms of headroom.
uint32_t frame_clock_stall_after_ms(const FrameClock *clock);

void frame_clock_note_stall(FrameClock *clock);

// True when the age of the last consumed stamp (ms) proves the
// compositor stopped pacing: more than two stall windows old. The
// factor of two keeps a healthy callback chain (stamp consumed, next
// done already pending) from ever tripping the watchdog.
bool frame_clock_stalled(const FrameClock *clock, uint64_t stamp_age_ms);

// Fixed-timestep consumption: how many whole step_ms steps the carried
// accumulator plus dt_ms pays for, capped at max_steps. Remainder is
// carried, except when the cap is hit — then the debt is dropped so a
// hitch degrades to slow motion instead of a spiral of death.
uint32_t frame_clock_fixed_steps(FrameClock *clock, uint32_t dt_ms,
                                 uint32_t step_ms, uint32_t max_steps);

#endif /* ENGINE_FRAME_CLOCK_H */
