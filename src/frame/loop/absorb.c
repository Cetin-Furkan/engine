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
        frame_state.timer_armed       = false;
        frame_state.timer_deadline_ns = 0ull;
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
    if (wl->noted_sim_cancels > 0u) {
        if (frame_state.sim_cancel_inflight >= wl->noted_sim_cancels) {
            frame_state.sim_cancel_inflight -= wl->noted_sim_cancels;
        } else {
            frame_state.sim_cancel_inflight = 0u;
        }
        wl->noted_sim_cancels = 0u;
    }
    if (wl->noted_sim) {
        uint64_t expirations = wl->noted_sim_exp;
        uint64_t last_ud     = wl->noted_sim_last_ud;
        uint32_t retired     = wl->noted_sim_retired;
        int      res         = wl->noted_sim_res;
        bool     alive       = wl->noted_sim_alive;
        wl->noted_sim         = false;
        wl->noted_sim_res     = 0;
        wl->noted_sim_alive   = false;
        wl->noted_sim_retired = 0u;
        wl->noted_sim_last_ud = 0ull;
        wl->noted_sim_exp     = 0ull;
        frame_apply_noted_sim(ring, last_ud, expirations, retired, res, alive, rearm_sim);
    }
    if (wl->noted_pager_mask != 0u) {
        uint16_t pmask = wl->noted_pager_mask;
        wl->noted_pager_mask = 0u;
        while (pmask != 0u) {
            uint32_t idx = (uint32_t)__builtin_ctz((unsigned)pmask);
            pmask &= (uint16_t)(pmask - 1u);
            struct io_uring_cqe synth = {
                .user_data = ENG_UD_PAGER_BASE + (uint64_t)idx,
                .res       = wl->noted_pager_res[idx],
                .flags     = 0u,
            };
            wl->noted_pager_res[idx] = 0;
            if (engine_app() != nullptr && engine_app()->on_cqe != nullptr &&
                engine_app()->on_cqe(&synth)) {
                frame_note_picture();
            }
        }
    }
}
