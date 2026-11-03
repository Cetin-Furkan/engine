#include "frame/loop/lane.h"

int frame_loop_finish(RingContext *ring, WaylandContext *wl, VulkanContext *vk,
                      LoopLane *lane, const sigset_t *old_mask) {
    bool inotify_cancel = false;
    bool cancel_in_flight = false;
    // Stop inotify and the simulation timer before any detach roundtrip so
    // side CQEs absorbed during detach never re-arm them.
    frame_state.inotify_stop = true;
    sim_timer_set(ring, false);

    absorb_side_cqes(ring, wl, vk, &lane->signal_poll_armed, &lane->got_signal, &lane->signo,
                     &lane->send_in_flight, &lane->resend_wire, lane->pending_serial,
                     &lane->inotify_armed, &lane->run_rc, false);
    if (lane->got_signal) {
        lane->why = "signal";
    }

    // Unmap before the drain and the GPU idle, so close does not sit on
    // a live window. A send that is still queued has to finish first.
    if (!lane->send_in_flight && !lane->resend_wire && !wl->failed && wayland_detach_buffer(wl) == 0) {
        absorb_side_cqes(ring, wl, vk, &lane->signal_poll_armed, &lane->got_signal, &lane->signo,
                         &lane->send_in_flight, &lane->resend_wire, lane->pending_serial,
                         &lane->inotify_armed, &lane->run_rc, false);
        if (lane->got_signal) {
            lane->why = "signal";
        }
    }

    bool sim_cancel = false;
    if (frame_state.sim_mshot_armed &&
        prep_cancel_fd(ring, frame_state.sim_fd, ENG_SIM_FD_SLOT, frame_state.sim_fixed,
                       k_ud_sim_cancel) == 0) {
        sim_cancel = true;
    }
    if (lane->signal_poll_armed &&
        prep_cancel_fd(ring, lane->signal_fd, ENG_SIGNAL_FD_SLOT, lane->signal_fixed, k_ud_signal_cancel) == 0) {
        cancel_in_flight = true;
    }
    if (lane->inotify_armed &&
        prep_cancel_fd(ring, frame_state.inotify_fd, ENG_INOTIFY_FD_SLOT, frame_state.inotify_fixed,
                       k_ud_inotify_cancel) == 0) {
        inotify_cancel = true;
    }
    bool timer_cancel = false;
    if (frame_state.timer_armed &&
        frame_prep_cancel_tag(ring, ENG_UD_FRAME_TIMER, k_ud_timer_cancel) == 0) {
        timer_cancel = true;
    }

    uint64_t drain_deadline = ring_mono_ns() + 3000000000ull;
    while ((lane->send_in_flight || lane->resend_wire || wl->pong_in_flight ||
            lane->signal_poll_armed || cancel_in_flight ||
            lane->inotify_armed || inotify_cancel ||
            frame_state.sim_mshot_armed || sim_cancel ||
            frame_state.timer_armed || timer_cancel) &&
           !wl->failed) {
        if (ring_mono_ns() > drain_deadline) {
            fprintf(stderr, "[Engine Error] Idle teardown drain timed out\n");
            lane->run_rc = 1;
            break;
        }
        absorb_side_cqes(ring, wl, vk, &lane->signal_poll_armed, &lane->got_signal, &lane->signo,
                         &lane->send_in_flight, &lane->resend_wire, lane->pending_serial,
                         &lane->inotify_armed, &lane->run_rc, false);
        if (lane->got_signal) {
            lane->why = "signal";
        }
        if (lane->resend_wire && !lane->send_in_flight) {
            lane->resend_wire = false;
            if (wayland_queue_send(wl, wl->send_tracks_ack) == 0) {
                lane->send_in_flight = true;
            } else {
                lane->send_in_flight = false;
                lane->run_rc = 1;
            }
        }
        struct io_uring_cqe cqe;
        if (!ring_peek(ring, &cqe)) {
            unsigned submitted = ring_submit(ring);
            int rc = ring_enter(ring, submitted, 1, 200);
            if (rc < 0 && rc != -ETIME && rc != -EINTR) {
                lane->run_rc = 1;
                break;
            }
            continue;
        }
        engine_app()->note_cqe();
        if (cqe.user_data == ENG_UD_WL_SEND) {
            int noted = finish_idle_send(wl, vk, &cqe, lane->pending_serial);
            if (noted < 0) {
                lane->send_in_flight = false;
                lane->run_rc = 1;
            } else if (noted == 1) {
                if (wayland_queue_send(wl, wl->send_tracks_ack) == 0) {
                    lane->send_in_flight = true;
                } else {
                    lane->send_in_flight = false;
                    lane->run_rc = 1;
                }
            } else {
                lane->send_in_flight = false;
            }
        } else if (cqe.user_data == ENG_UD_SIGNAL) {
            lane->signal_poll_armed = false;
            int got = frame_signal_number(cqe.res);
            if (got != 0) {
                lane->signo = got;
                lane->got_signal = true;
                lane->why = "signal";
            }
        } else if (cqe.user_data == ENG_UD_FRAME_TIMER) {
            frame_state.timer_armed = false;
        } else if (cqe.user_data == k_ud_timer_cancel) {
            timer_cancel = false;
        } else if (frame_note_sim_cqe(ring, &cqe, false)) {
        } else if (cqe.user_data == k_ud_sim_cancel) {
            sim_cancel = false;
        } else if (cqe.user_data == k_ud_signal_cancel) {
            cancel_in_flight = false;
        } else if (cqe.user_data == ENG_UD_INOTIFY) {
            lane->inotify_armed = false;
        } else if (cqe.user_data == k_ud_inotify_cancel) {
            inotify_cancel = false;
        } else {
            wayland_on_cqe(wl, &cqe);
        }
    }

    // If a send was in flight when the main loop exited, detach the surface now
    // that the wire is idle so the window unmaps before GPU teardown.
    if (!wl->buffers_detached && !lane->send_in_flight && !wl->failed) {
        (void)wayland_detach_buffer(wl);
    }

    if (lane->signal_fixed) {
        (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
    }
    close(lane->signal_fd);
    if (frame_state.sim_fixed) {
        (void)ring_update_file(ring, ENG_SIM_FD_SLOT, -1);
        frame_state.sim_fixed = false;
    }
    if (frame_state.sim_fd >= 0) {
        close(frame_state.sim_fd);
        frame_state.sim_fd = -1;
    }
    if (frame_state.inotify_fixed) {
        (void)ring_update_file(ring, ENG_INOTIFY_FD_SLOT, -1);
        frame_state.inotify_fixed = false;
    }
    if (frame_state.inotify_fd >= 0) {
        close(frame_state.inotify_fd);
        frame_state.inotify_fd = -1;
        frame_state.inotify_wd = -1;
    }
    (void)sigprocmask(SIG_SETMASK, old_mask, nullptr);

    if (lane->got_signal && lane->signo != 0) {
        printf("[Engine] Idle ended (signal %d)\n", lane->signo);
    } else {
        printf("[Engine] Idle ended (%s)\n", lane->why);
    }
    fflush(stdout);
    return lane->run_rc;
}
