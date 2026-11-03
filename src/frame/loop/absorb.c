#include "frame/loop/lane.h"

[[nodiscard]] int frame_prep_cancel_tag(RingContext *ring, uint64_t target_ud, uint64_t cancel_ud) {
    if (ring == nullptr || !ring->is_initialized) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_ASYNC_CANCEL;
    sqe->addr      = target_ud;
    sqe->user_data = cancel_ud;
    return 0;
}

// A blocking Wayland round-trip shares the ring. It records timer, inotify,
// simulation, signal, and send completions instead of dropping them. Apply
// those notes here, never inside the wait: the simulation must not run while
// a send buffer is still being filled.
void absorb_side_cqes(RingContext *ring, WaylandContext *wl, VulkanContext *vk,
                             bool *signal_poll_armed, bool *got_signal, int *signo,
                             bool *send_in_flight, bool *resend_wire,
                             uint32_t pending_serial, bool *inotify_armed,
                             int *run_rc, bool rearm_sim) {
    if (consume_noted_signal(wl, signo)) {
        if (signal_poll_armed != nullptr) {
            *signal_poll_armed = false;
        }
        if (got_signal != nullptr) {
            *got_signal = true;
        }
    }
    if (wl->noted_send) {
        struct io_uring_cqe cqe = {
            .user_data = ENG_UD_WL_SEND,
            .res       = wl->noted_send_res,
        };
        wl->noted_send = false;
        wl->noted_send_res = 0;
        int noted = finish_idle_send(wl, vk, &cqe, pending_serial);
        if (noted < 0) {
            *send_in_flight = false;
            *run_rc = 1;
        } else if (noted == 1) {
            *send_in_flight = false;
            *resend_wire = true;
        } else {
            *send_in_flight = false;
        }
    }
    if (wl->noted_timer) {
        wl->noted_timer = false;
        frame_state.timer_armed = false;
        if (rearm_sim && !*send_in_flight && !*resend_wire) {
            bool queued = false;
            if (frame_watchdog(wl, vk, &queued) == 0 && queued) {
                *send_in_flight = true;
            }
        }
    }
    if (wl->noted_inotify) {
        int res = wl->noted_inotify_res;
        wl->noted_inotify = false;
        wl->noted_inotify_res = 0;
        note_inotify_cqe(ring, res, inotify_armed);
    }
    if (wl->noted_sim) {
        uint64_t expirations = wl->noted_sim_exp;
        int res = wl->noted_sim_res;
        bool alive = wl->noted_sim_alive;
        wl->noted_sim = false;
        wl->noted_sim_res = 0;
        wl->noted_sim_alive = false;
        wl->noted_sim_exp = 0;
        frame_apply_noted_sim(ring, expirations, res, alive, rearm_sim);
    }
}
