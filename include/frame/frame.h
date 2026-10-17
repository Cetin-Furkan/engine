#ifndef ENGINE_FRAME_H
#define ENGINE_FRAME_H

[[nodiscard]] int engine_subsystem_init(void);

/**
 * @brief Leaves the presented buffer on screen. The CPU blocks in io_uring
 *        until the toplevel is closed or SIGINT, SIGTERM, or SIGHUP arrives.
 *        No further frame callback is armed and no further GPU work is submitted.
 */
[[nodiscard]] int engine_subsystem_run(void);

/**
 * @brief Drops the presented buffer, waits for the compositor release point
 *        through io_uring, then frees the frame. Returns 0 when that wait
 *        was not required or the release point was signalled.
 */
[[nodiscard]] int engine_subsystem_shutdown(void);

#endif /* ENGINE_FRAME_H */