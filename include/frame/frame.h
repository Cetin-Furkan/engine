#ifndef ENGINE_FRAME_H
#define ENGINE_FRAME_H

[[nodiscard]] int engine_subsystem_init(void);

/**
 * @brief Runs the continuous frame loop. A wl_surface.frame callback is
 * always outstanding; each compositor stamp steps the FrameClock,
 * consumes fixed 8 ms simulation steps, and presents only when the
 * picture changed. The CPU blocks in io_uring until the toplevel is
 * closed or SIGINT, SIGTERM, or SIGHUP arrives. An absolute io_uring
 * timeout watchdog re-arms the callback if the compositor stalls.
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

#endif /* ENGINE_FRAME_H */