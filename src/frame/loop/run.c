#include "frame/internal.h"
#include "frame/loop/lane.h"
#include <sys/timerfd.h>

// The wait is io_uring. The 8 ms timer advances the simulation. A frame
// callback is armed only while a picture is owed or the solver is live.

static bool s_was_suspended = false;

int frame_run(void) {
    if (!frame_state.presented || !shown()->release_pending) {
        fprintf(stderr, "[Engine Error] Idle run requires a presented frame\n");
        return 1;
    }
    RingContext    *ring = ring_get_context();
    WaylandContext *wl   = wayland_get_context();
    if (ring == nullptr || !ring->is_initialized || wl == nullptr || !wl->is_initialized) {
        fprintf(stderr, "[Engine Error] Idle run requires the ring and Wayland\n");
        return 1;
    }

    sigset_t mask;
    sigset_t old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) != 0) {
        fprintf(stderr, "[Engine Error] sigprocmask: %s\n", strerror(errno));
        return 1;
    }

    (void)ensure_release_fd(ring);

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd < 0) {
        fprintf(stderr, "[Engine Error] signalfd: %s\n", strerror(errno));
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }

    bool signal_fixed = (ring_update_file(ring, ENG_SIGNAL_FD_SLOT, signal_fd) == 0);
    int sim_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sim_fd < 0) {
        fprintf(stderr, "[Engine Error] timerfd: %s\n", strerror(errno));
        if (signal_fixed) {
            (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
        }
        close(signal_fd);
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }
    frame_state.sim_fd = sim_fd;
    frame_state.sim_fixed = (ring_update_file(ring, ENG_SIM_FD_SLOT, sim_fd) == 0);
    frame_state.sim_mshot_armed = false;
    if (frame_arm_sim_mshot(ring) != 0) {
        fprintf(stderr, "[Engine Error] Failed to arm the simulation timer\n");
        if (frame_state.sim_fixed) {
            (void)ring_update_file(ring, ENG_SIM_FD_SLOT, -1);
            frame_state.sim_fixed = false;
        }
        close(sim_fd);
        frame_state.sim_fd = -1;
        if (signal_fixed) {
            (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
        }
        close(signal_fd);
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }
    int read_rc = arm_slot_read(ring, ENG_SIGNAL_FD_SLOT, signal_fixed, signal_fd,
                                ENG_FBUF_SIGNAL, ENG_UD_SIGNAL);
    if (read_rc != 0) {
        fprintf(stderr, "[Engine Error] Failed to arm io_uring read on signalfd\n");
        if (frame_state.sim_mshot_armed) {
            ring_sqe_unget(ring, 1);
            frame_state.sim_mshot_armed = false;
        }
        if (frame_state.sim_fixed) {
            (void)ring_update_file(ring, ENG_SIM_FD_SLOT, -1);
            frame_state.sim_fixed = false;
        }
        close(sim_fd);
        frame_state.sim_fd = -1;
        if (signal_fixed) {
            (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
        }
        close(signal_fd);
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }

    s_was_suspended = false;
    printf("[Engine] Window %ux%u, scene %ux%u is up. %s\n",
           frame_state.window_w, frame_state.window_h,
           shown()->image.width, shown()->image.height,
           engine_app()->help != nullptr ? engine_app()->help : "");
    fflush(stdout);

    const ChromeSpec *spec = chrome_spec();
    VulkanContext *vk = vulkan_get_context();
    bool     signal_poll_armed = true;
    bool     inotify_armed     = false;
    bool     send_in_flight    = false;
    bool     resend_wire       = false;
    uint32_t pending_serial    = 0;
    bool     got_signal        = false;
    int      signo             = 0;
    int      run_rc            = 0;
    const char *why            = "idle wait ended";
    start_inotify(ring, &inotify_armed);

    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(ring, &cqe)) {
            engine_app()->note_cqe();
            if (cqe.user_data == ENG_UD_SIGNAL) {
                signal_poll_armed = false;
                int got = frame_signal_number(cqe.res);
                if (got != 0) {
                    signo = got;
                }
                got_signal = true;
            } else if (cqe.user_data == ENG_UD_FRAME_TIMER) {
                frame_state.timer_armed = false;
                if (!send_in_flight && !resend_wire) {
                    bool queued = false;
                    if (frame_watchdog(wl, vk, &queued) == 0 && queued) {
                        send_in_flight = true;
                    }
                }
            } else if (cqe.user_data == ENG_UD_INOTIFY) {
                note_inotify_cqe(ring, cqe.res, &inotify_armed);
            } else if (frame_note_sim_cqe(ring, &cqe, true)) {
            } else if (cqe.user_data == ENG_UD_WL_SEND) {
                int noted = finish_idle_send(wl, vk, &cqe, pending_serial);
                if (noted < 0) {
                    send_in_flight = false;
                    run_rc = 1;
                } else if (noted == 1) {
                    send_in_flight = false;
                    resend_wire = true;
                } else {
                    send_in_flight = false;
                }
            } else {
                wayland_on_cqe(wl, &cqe);
            }
        }

        absorb_side_cqes(ring, wl, vk, &signal_poll_armed, &got_signal, &signo,
                         &send_in_flight, &resend_wire, pending_serial,
                         &inotify_armed, &run_rc, true);

        if (got_signal) {
            why = "signal";
            break;
        }
        if (wl->toplevel_closed) {
            why = "toplevel close";
            break;
        }
        if (wl->failed) {
            why    = wl->err;
            run_rc = 1;
            break;
        }
        if (run_rc != 0) {
            why = wl->err[0] != '\0' ? wl->err : "idle send or resize install failed";
            break;
        }

        if (!send_in_flight && !resend_wire) {
            sweep_ready_orphans(wl, vk, false);
        }

        if (!send_in_flight && !resend_wire &&
            shown()->image.width != 0 && shown()->image.height != 0 &&
            loop_take_input(wl, vk, &signal_poll_armed, &got_signal, &signo,
                            &inotify_armed, pending_serial, &send_in_flight,
                            &resend_wire, &run_rc, &why)) {
            break;
        }

        absorb_side_cqes(ring, wl, vk, &signal_poll_armed, &got_signal, &signo,
                         &send_in_flight, &resend_wire, pending_serial,
                         &inotify_armed, &run_rc, true);
        if (got_signal) {
            why = "signal";
            break;
        }
        if (wl->toplevel_closed) {
            why = "toplevel close";
            break;
        }
        if (wl->failed || run_rc != 0) {
            why    = wl->err[0] != '\0' ? wl->err : "key reload failed";
            run_rc = 1;
            break;
        }

        if (frame_state.inotify_fd >= 0) {
            ensure_inotify_watch();
        }
        if (!send_in_flight && !resend_wire && !frame_state.resize_pending &&
            frame_state.world_dirty) {
            frame_state.world_dirty = false;
            if (engine_app()->reload_watched != nullptr &&
                engine_app()->reload_watched()) {
                frame_note_picture();
            }
            if (frame_state.inotify_fd >= 0) {
                ensure_inotify_watch();
            }
        }
        if (!send_in_flight && !resend_wire && !frame_state.resize_pending &&
            frame_state.chrome_dirty) {
            frame_state.chrome_dirty = false;
            RunRelay relay;
            begin_file_relay(&relay, &signal_poll_armed, &got_signal, &signo, &inotify_armed);
            int reloaded = chrome_reload(forward_reload_cqe, nullptr);
            if (reloaded == -ENOSPC || reloaded == -ETIMEDOUT || reloaded == -EINTR ||
                reloaded == -EAGAIN || reloaded == -EBUSY) {
                frame_state.chrome_dirty = true;
            }
            if (end_file_relay(&relay, wl, vk, pending_serial, &send_in_flight,
                               &resend_wire, &run_rc, &why)) {
                break;
            }
            absorb_side_cqes(ring, wl, vk, &signal_poll_armed, &got_signal, &signo,
                             &send_in_flight, &resend_wire, pending_serial,
                             &inotify_armed, &run_rc, true);
            if (got_signal) {
                why = "signal";
                break;
            }
            if (wl->failed || run_rc != 0) {
                why    = wl->err[0] != '\0' ? wl->err : "chrome reload failed";
                run_rc = 1;
                break;
            }
            if (reloaded == 0 && !send_in_flight && !resend_wire &&
                !frame_state.turn_swap_pending && vk != nullptr) {
                int painted = queue_chrome_repaint(wl, vk);
                if (painted == 0) {
                    pending_serial = 0;
                    send_in_flight = true;
                } else if (painted == -EAGAIN || painted == -EBUSY) {
                    // The spare bar image is still on screen, or nothing new
                    // was armed. bar_dirty asks for another try on the next wake.
                    if (frame_state.bar_dirty) {
                        frame_state.chrome_dirty = true;
                    }
                } else {
                    fprintf(stderr, "[Chrome] Repaint was not queued\n");
                }
            }
        }

        if (!send_in_flight && resend_wire) {
            resend_wire = false;
            int queued = wayland_queue_send(wl, wl->send_tracks_ack);
            if (queued == 0) {
                send_in_flight = true;
            } else {
                run_rc = 1;
                why = wl->err[0] != '\0' ? wl->err : "wayland send retry failed";
                break;
            }
        } else if (!send_in_flight && content_size_pending(spec, wl)) {
            uint32_t next_w = 0;
            uint32_t next_h = 0;
            (void)pending_content_size(wl, &next_w, &next_h);
            printf("[Engine] Resize window %ux%u -> %ux%u\n",
                   frame_state.window_w, frame_state.window_h, next_w, next_h);
            fflush(stdout);
            if (vk == nullptr || queue_resize_frame(wl, vk) != 0) {
                run_rc = 1;
                why = wl->err[0] != '\0' ? wl->err : "buffer replace failed";
                if (consume_noted_signal(wl, &signo)) {
                    signal_poll_armed = false;
                    got_signal = true;
                    why = "signal";
                    run_rc = 0;
                }
                break;
            }
            pending_serial = frame_state.resize_serial;
            send_in_flight = true;
        }

        absorb_side_cqes(ring, wl, vk, &signal_poll_armed, &got_signal, &signo,
                         &send_in_flight, &resend_wire, pending_serial,
                         &inotify_armed, &run_rc, true);
        if (got_signal) {
            why = "signal";
            break;
        }
        if (wl->toplevel_closed) {
            why = "toplevel close";
            break;
        }
        if (wl->failed || run_rc != 0) {
            why    = wl->err[0] != '\0' ? wl->err : "resize send failed";
            run_rc = 1;
            break;
        }

        if (!send_in_flight && !resend_wire && configure_needs_ack(wl)) {
            uint32_t width  = frame_state.window_w;
            uint32_t height = frame_state.window_h;
            // The window geometry is the full window. The scene buffer is the
            // part under the bar, so the image size is not the ack size.
            int32_t viewport_mode = ENG_VIEWPORT_LEAVE;
            pending_serial = wl->configure_serial;
            int queued = wayland_queue_configure_ack(wl, pending_serial, width, height, viewport_mode);
            if (queued == 0) {
                send_in_flight = true;
            } else if (queued != -EBUSY) {
                run_rc = 1;
                why    = wl->err[0] != '\0' ? wl->err : "configure ack failed";
                break;
            }
        } else if (!send_in_flight && !resend_wire && turn_wants_step(wl)) {
            bool queued = false;
            int stepped = turn_step(wl, vk, &queued);
            if (stepped != 0 && stepped != -EBUSY) {
                run_rc = 1;
                why = wl->err[0] != '\0' ? wl->err : "turn step failed";
                break;
            }
            if (queued) {
                send_in_flight = true;
            }
        } else if (!send_in_flight && !resend_wire) {
            int enc = pointer_encode(wl);
            if (enc < 0) {
                pointer_on_send_done(false);
                run_rc = 1;
                why = "pointer encode failed";
                break;
            }
            if (enc == 1) {
                int queued = wayland_queue_send(wl, false);
                if (queued == 0) {
                    send_in_flight = true;
                } else {
                    pointer_on_send_done(false);
                    run_rc = 1;
                    why = wl->err[0] != '\0' ? wl->err : "pointer send failed";
                    break;
                }
            } else {
                int destroyed = wayland_queue_destroys(wl);
                if (destroyed == 1) {
                    pending_serial = 0;
                    send_in_flight = true;
                } else if (destroyed < 0 && destroyed != -EBUSY) {
                    fprintf(stderr, "[Engine Error] Deferred Wayland destroy was not queued\n");
                }
            }
        }

        if (!wl->recv_armed && wayland_ensure_recv(wl) < 0) {
            run_rc = 1;
            why    = wl->err;
            break;
        }

        // Body round-trips can take the timer completion off the ring.
        // Put it back before the next sleep, or the solver stops forever.
        absorb_side_cqes(ring, wl, vk, &signal_poll_armed, &got_signal, &signo,
                         &send_in_flight, &resend_wire, pending_serial,
                         &inotify_armed, &run_rc, true);
        if (got_signal) {
            why = "signal";
            break;
        }
        if (wl->toplevel_closed) {
            why = "toplevel close";
            break;
        }
        if (wl->failed || run_rc != 0) {
            why    = wl->err[0] != '\0' ? wl->err : "turn step roundtrip failed";
            run_rc = 1;
            break;
        }

        // The timer runs only while the solver wants steps, including a held
        // camera key. An idle window does not wake at 125 Hz, and it does
        // not keep a frame callback.
        // Suspended means the compositor is not scanning this window out.
        bool suspended = (wl->toplevel_states & ENG_TOP_SUSPENDED) != 0;
        if (suspended != s_was_suspended) {
            s_was_suspended = suspended;
            if (!suspended) {
                frame_note_picture();
            }
        }
        if (!frame_state.sim_mshot_armed) {
            (void)frame_arm_sim_mshot(ring);
        }
        sim_timer_set(ring, !suspended && (frame_sim_wanted() || frame_state.sim_hot));
        // Watchdog only while a callback is outstanding. It re-arms a stall.
        // It does not step the pose or the solver.
        if (!suspended && frame_state.turn_phase == k_turn_run && wl->frame_cb_id != 0 &&
            !wl->frame_stamp_valid && !frame_state.timer_armed && !send_in_flight) {
            uint64_t deadline = ring_mono_ns() +
                (uint64_t)frame_clock_stall_after_ms(&frame_state.clock) * 1000000ull;
            if (ring_arm_timeout_abs(ring, deadline, ENG_UD_FRAME_TIMER) == 0) {
                frame_state.timer_armed = true;
            }
        } else if (!suspended && frame_state.turn_phase == k_turn_run &&
                   !frame_state.timer_armed && !send_in_flight && wl->frame_cb_id == 0 &&
                   turn_wants_step(wl)) {
            // A picture is owed and no callback will wake us. The spare may
            // still be held. A frameless wl_surface.frame does not attach a
            // buffer, so it cannot free that image. Retry on the sim quanta.
            uint64_t deadline = ring_mono_ns() + (uint64_t)FRAME_SIM_STEP_MS * 1000000ull;
            if (ring_arm_timeout_abs(ring, deadline, ENG_UD_FRAME_TIMER) == 0) {
                frame_state.timer_armed = true;
            }
        }

        unsigned submitted = ring_submit(ring);
        int rc = ring_enter(ring, submitted, 1, -1);
        if (rc < 0) {
            fprintf(stderr, "[Engine Error] io_uring_enter while idle: %s\n", strerror(-rc));
            run_rc = 1;
            why    = "io_uring_enter failed";
            break;
        }
    }

    LoopLane lane = {
        .send_in_flight    = send_in_flight,
        .resend_wire       = resend_wire,
        .signal_poll_armed = signal_poll_armed,
        .inotify_armed     = inotify_armed,
        .got_signal        = got_signal,
        .signal_fixed      = signal_fixed,
        .signo             = signo,
        .run_rc            = run_rc,
        .signal_fd         = signal_fd,
        .pending_serial    = pending_serial,
        .why               = why,
    };
    return frame_loop_finish(ring, wl, vk, &lane, &old_mask);
}
