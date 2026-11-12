#ifndef ENGINE_FRAME_CLOCK_H
#define ENGINE_FRAME_CLOCK_H

#include <stdint.h>

// Present pacing, the simulation cursor, and the GPU pass times share
// this 64-byte cell. Pure logic, no io_uring, no Vulkan: unit-tested.
//
// A wl_surface.frame stamp teaches the refresh period and the stall
// watchdog. It does not advance the simulation. frame_clock_issue does
// that from the 8 ms timer. Pose and the solver both integrate the
// issued total. A hitch past the cap is dropped, not replayed.

// One simulation quantum. The timer issues at most FRAME_SIM_MAX_STEPS
// of these per wake.
enum : uint32_t {
    FRAME_SIM_STEP_MS   = 8u,
    FRAME_SIM_MAX_STEPS = 5u,
};

typedef struct FrameStep {
    uint32_t dt_ms;     // Clamped interval since the previous stamp.
    uint32_t missed;    // Whole refreshes skipped inside that interval.
    bool     paint;     // False for a duplicate stamp (dt == 0 after baseline).
    bool     first;     // True for the baseline stamp (establishes the clock).
    uint32_t sim_dt_ms; // Fixed steps actually issued. Pose and solver share it.
} FrameStep;

typedef struct FrameClock {
    uint32_t last_stamp_ms;
    uint32_t period_q8;  // Learned period, ms * 256 (fixed point EMA).
    uint32_t frames;
    uint32_t missed_total;
    uint32_t stalls;
    bool     have_stamp;
    bool     prev_long;
    uint32_t accum_ms;     // Fixed-step accumulator (carried remainder).
    uint32_t sim_steps;    // Fixed steps issued since reset.
    uint32_t sim_ms;       // Monotonic simulation cursor. Advances by issued steps only.
    uint32_t gpu_compute_us; // Hardware compute interval, EMA, microseconds.
    uint32_t gpu_render_us;  // Hardware color pass interval, EMA, microseconds.
    uint32_t gpu_total_us;   // Hardware submit interval, EMA, microseconds.
    uint8_t  reserved[16];
} FrameClock;

static_assert(sizeof(FrameClock) == 64, "frame clock is one cache line");

void frame_clock_reset(FrameClock *clock);

// Fold one compositor stamp in. paint is false for a duplicate. The
// simulation cursor does not move here.
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

// Simulation cursor. It moves only when frame_clock_issue accepts time.
uint32_t frame_clock_sim_stamp(const FrameClock *clock);

// Consume dt_ms from the simulation timer as FRAME_SIM_STEP_MS quanta,
// at most FRAME_SIM_MAX_STEPS. Writes the absolute sim stamps for those
// quanta, ending on the new cursor, and returns the count. A null
// out_stamps still advances the cursor. Dropped debt is not in the stamps.
uint32_t frame_clock_issue(FrameClock *clock, uint32_t dt_ms, uint32_t *out_stamps);

// Fold one retired frame's timestamp-query intervals into the EMA.
// The first sample is stored as-is. Later samples keep seven eighths
// of the previous value.
void frame_clock_note_gpu(FrameClock *clock, uint32_t compute_us,
                          uint32_t render_us, uint32_t total_us);

#endif /* ENGINE_FRAME_CLOCK_H */
