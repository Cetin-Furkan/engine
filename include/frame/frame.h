#ifndef ENGINE_FRAME_H
#define ENGINE_FRAME_H

[[nodiscard]] int engine_subsystem_init(void);

/**
 * @brief Leaves the presented buffer on screen. The CPU blocks in io_uring
 * until the toplevel is closed or SIGINT, SIGTERM, or SIGHUP arrives.
 * When the chrome spec allows resize, a configure whose size differs
 * records a new buffer, including while the drag is active. The bar stays
 * at its spec thickness. The toplevel minimum is that chrome size.
 * The cube is drawn into that buffer at a fixed 240px size, centered.
 * Its edges and corners are a GPU fillet; the face interiors stay flat.
 * The first present does not arm a frame callback. The run loop arms one
 * for a single turn, then drops it.
 */
[[nodiscard]] int engine_subsystem_run(void);

/**
 * @brief Drops the presented buffer, waits for the compositor release point
 * through io_uring, then frees the frame. Returns 0 when that wait
 * was not required or the release point was signalled.
 */
[[nodiscard]] int engine_subsystem_shutdown(void);

#endif /* ENGINE_FRAME_H */