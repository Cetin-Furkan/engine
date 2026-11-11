#ifndef ENGINE_FRAME_H
#define ENGINE_FRAME_H

#include <stdint.h>

[[nodiscard]] int engine_subsystem_init(void);

/**
 * @brief Runs the present loop. The CPU blocks in io_uring until the
 * toplevel is closed or SIGINT, SIGTERM, or SIGHUP arrives. The 8 ms
 * timer advances the simulation. A compositor stamp paces the next
 * present and does not step the solver. A callback is armed only while
 * a picture is owed or the solver is live. The first picture after idle
 * commits its buffer, full damage, and wl_surface.frame together.
 * A held spare keeps its stamp; the loop retries on the 8 ms timer.
 * When the chrome spec allows resize, a configure whose size differs
 * records a new buffer, including while the drag is active. The bar stays
 * at its spec thickness. The toplevel minimum is that chrome size.
 */
[[nodiscard]] int engine_subsystem_run(void);

/**
 * @brief Drops the presented buffer, waits for the compositor release point
 * through io_uring, then frees the frame. Returns 0 when that wait
 * was not required or the release point was signalled.
 */
[[nodiscard]] int engine_subsystem_shutdown(void);

// Latest EMA of the hardware timestamp queries, in microseconds.
// A null pointer skips that pass. Zero means no retired sample yet.
void frame_gpu_timings(uint32_t *compute_us, uint32_t *render_us, uint32_t *total_us);

// Exact nanosecond intervals from the most recently retired hardware query slot,
// plus monotonic count of retired timestamp samples.
void frame_gpu_raw_ns(uint64_t *compute_ns, uint64_t *render_ns, uint64_t *total_ns,
                      uint64_t *retired_samples);

typedef struct FrameRect {
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
} FrameRect;

// Enables uncapped simulation-timer presentation (no wl_surface.frame stall
// when the benchmark window is occluded behind a terminal or IDE).
void frame_set_bench_mode(bool enabled);

// Marks a screen-space content sub-rectangle dirty for cross-GPU PRIME DMA copy
// and Wayland wl_surface.damage_buffer. Pass UINT32_MAX width/height for full frame.
void frame_note_damage_rect(int32_t x, int32_t y, uint32_t width, uint32_t height);

#endif /* ENGINE_FRAME_H */