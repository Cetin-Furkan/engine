#ifndef ENGINE_FRAME_LOOP_LANE_H
#define ENGINE_FRAME_LOOP_LANE_H

#include "frame/internal.h"

// Cancels the frame timeout, which has no fd. prep_cancel_fd cannot name it.
enum : uint64_t {
    k_ud_timer_cancel = UINT64_MAX - 4u,
};

[[nodiscard]] int frame_prep_cancel_tag(RingContext *ring, uint64_t target_ud, uint64_t cancel_ud);

// A blocking Wayland round-trip records timer, inotify, simulation, signal,
// and send completions instead of dropping them. Apply those notes here,
// never inside the wait: the simulation must not run while a send buffer
// is still being filled.
// Pointer, key, text, scroll, and drag. True when a key asked the loop to leave.
[[nodiscard]] bool loop_take_input(WaylandContext *wl, VulkanContext *vk,
                                   bool *signal_poll_armed, bool *got_signal, int *signo,
                                   bool *inotify_armed, uint32_t pending_serial,
                                   bool *send_in_flight, bool *resend_wire,
                                   int *run_rc, const char **why);

void absorb_side_cqes(RingContext *ring, WaylandContext *wl, VulkanContext *vk,
                      bool *signal_poll_armed, bool *got_signal, int *signo,
                      bool *send_in_flight, bool *resend_wire,
                      uint32_t pending_serial, bool *inotify_armed,
                      int *run_rc, bool rearm_sim);

// The flags the idle loop keeps on the stack. Teardown takes the struct
// so the drain does not chase a dozen separate addresses.
typedef struct LoopLane {
    bool     send_in_flight;
    bool     resend_wire;
    bool     signal_poll_armed;
    bool     inotify_armed;
    bool     got_signal;
    bool     signal_fixed;
    int      signo;
    int      run_rc;
    int      signal_fd;
    uint32_t pending_serial;
    const char *why;
} LoopLane;

// Unmap, cancel the polls the loop armed, and close their fds.
[[nodiscard]] int frame_loop_finish(RingContext *ring, WaylandContext *wl, VulkanContext *vk,
                                    LoopLane *lane, const sigset_t *old_mask);

#endif /* ENGINE_FRAME_LOOP_LANE_H */
