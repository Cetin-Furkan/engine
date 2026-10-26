#include "frame/frame_int.h"

// Idle present. The wait is io_uring. Keys and clicks enter here.

int frame_run(void) {
    if (!s_presented || !shown()->release_pending) {
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
    int read_rc = arm_slot_read(ring, ENG_SIGNAL_FD_SLOT, signal_fixed, signal_fd,
                                &s_siginfo, (uint32_t)sizeof s_siginfo, ENG_UD_SIGNAL);
    if (read_rc != 0) {
        fprintf(stderr, "[Engine Error] Failed to arm io_uring read on signalfd\n");
        if (signal_fixed) {
            (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
        }
        close(signal_fd);
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
        return 1;
    }

    printf("[Engine] Window %ux%u is up. One turn, then io_uring idle. Clicks on empty space place points. F closes those points into a face. A click on a mesh selects it. The cube click also plants heat for five seconds, and it can be selected while other meshes are up. Its colors sweep on that clock, then hold. Keys 1-6 raise a tetrahedron, octahedron, icosahedron, cylinder, cone, and torus. R spins until R again. S smooths edges. G opens or closes glow. Esc clears, Backspace removes, F5 turns the selection again. M cycles the math plot. L reloads the CSV chart and the OBJ mesh. Home or numpad 1 snaps the front view. Arrows move on the screen axes. Q and E yaw, C and V pitch. Minus zooms out. Plus is numpad plus, shift+4, or equals.\n",
           shown()->image.width, shown()->image.height);
    fflush(stdout);

    const ChromeSpec *spec = chrome_spec();
    VulkanContext *vk = vulkan_get_context();
    bool     signal_poll_armed = true;
    bool     inotify_armed     = false;
    bool     inotify_cancel    = false;
    bool     cancel_in_flight  = false;
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
            hud_note_cqe();
            if (cqe.user_data == ENG_UD_SIGNAL) {
                signal_poll_armed = false;
                if (cqe.res == (int)sizeof s_siginfo) {
                    signo = (int)s_siginfo.ssi_signo;
                }
                got_signal = true;
            } else if (cqe.user_data == ENG_UD_INOTIFY) {
                note_inotify_cqe(ring, cqe.res, &inotify_armed);
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

        if (consume_noted_signal(wl, &signo)) {
            signal_poll_armed = false;
            got_signal = true;
        }
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

        sweep_ready_orphans(wl, vk, false);

        if (!send_in_flight && shown()->image.width != 0 && shown()->image.height != 0) {
            int32_t sx = 0;
            int32_t sy = 0;
            while (pointer_take_spawn(&sx, &sy)) {
                float ox = (float)sx - (float)shown()->image.width * 0.5f;
                float oy = (float)shown()->image.height * 0.5f - (float)sy;
                WorldClick hit = world_click(ox, oy);
                if (hit.changed && s_turn_phase == k_turn_done) {
                    s_turn_phase = k_turn_arm;
                }
            }
            uint32_t key = 0;
            bool stop_run = false;
            while (pointer_take_key(&key)) {
                bool changed = false;
                if (key == KEY_ESC) {
                    changed = world_cancel();
                } else if (key == KEY_BACKSPACE) {
                    changed = world_delete();
                } else if (key == KEY_F) {
                    changed = world_close_face();
                } else if (key == KEY_F5) {
                    changed = world_respin_selected();
                } else if (key == KEY_R) {
                    changed = world_toggle_spin();
                } else if (key == KEY_S) {
                    changed = world_toggle_smooth();
                } else if (key == KEY_G) {
                    changed = world_toggle_glow();
                } else if (key == KEY_M) {
                    changed = plot_cycle();
                } else if (key == KEY_L) {
                    RunRelay relay;
                    begin_file_relay(&relay, &signal_poll_armed, &got_signal, &signo, &inotify_armed);
                    changed = plot_reload(forward_reload_cqe, nullptr);
                    bool mesh_has = false;
                    bool mesh_bytes = mesh_reload(forward_reload_cqe, nullptr, &mesh_has);
                    if (world_apply_mesh(mesh_has, mesh_bytes)) {
                        changed = true;
                    }
                    if (end_file_relay(&relay, wl, vk, pending_serial, &send_in_flight,
                                       &resend_wire, &run_rc, &why)) {
                        stop_run = true;
                        break;
                    }
                } else if (key == KEY_HOME || key == KEY_KP1) {
                    changed = camera_snap_front();
                } else if (key >= KEY_1 && key <= KEY_6) {
                    changed = world_raise_solid(key - KEY_1);
                }
                if (changed && s_turn_phase == k_turn_done) {
                    s_turn_phase = k_turn_arm;
                }
            }
            if (stop_run) {
                break;
            }
            if (s_turn_phase == k_turn_done && (camera_motion_down() || camera_busy())) {
                s_turn_phase = k_turn_arm;
            }
        }

        if (s_inotify_fd >= 0 && s_inotify_wd < 0) {
            ensure_inotify_watch();
        }
        if (!send_in_flight && !s_resize_pending && s_chrome_dirty) {
            s_chrome_dirty = false;
            RunRelay relay;
            begin_file_relay(&relay, &signal_poll_armed, &got_signal, &signo, &inotify_armed);
            int reloaded = chrome_reload(forward_reload_cqe, nullptr);
            if (reloaded == -ENOSPC || reloaded == -ETIMEDOUT || reloaded == -EINTR ||
                reloaded == -EAGAIN || reloaded == -EBUSY) {
                s_chrome_dirty = true;
            }
            if (end_file_relay(&relay, wl, vk, pending_serial, &send_in_flight,
                               &resend_wire, &run_rc, &why)) {
                break;
            }
            if (reloaded == 0 && !send_in_flight && !resend_wire &&
                s_turn_phase == k_turn_done && !s_spare_live && vk != nullptr) {
                if (queue_chrome_repaint(wl, vk) == 0) {
                    pending_serial = 0;
                    send_in_flight = true;
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
            printf("[Engine] Resize buffer %ux%u -> %ux%u\n",
                   shown()->image.width, shown()->image.height, next_w, next_h);
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
            pending_serial = s_resize_serial;
            send_in_flight = true;
            if (consume_noted_signal(wl, &signo)) {
                signal_poll_armed = false;
                got_signal = true;
                why = "signal";
                break;
            }
        }

        if (!send_in_flight && configure_needs_ack(wl)) {
            uint32_t width  = shown()->image.width;
            uint32_t height = shown()->image.height;
            // The buffer is already the clamped configure size, bar included.
            // Scaling it would stretch the bar. A leftover destination is cleared.
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
        } else if (!send_in_flight && turn_wants_step(wl)) {
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
        } else if (!send_in_flight) {
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
            }
        }

        if (!wl->recv_armed && wayland_ensure_recv(wl) < 0) {
            run_rc = 1;
            why    = wl->err;
            break;
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

    if (consume_noted_signal(wl, &signo)) {
        signal_poll_armed = false;
        got_signal = true;
        why = "signal";
    }

    s_inotify_stop = true;
    if (signal_poll_armed &&
        prep_cancel_fd(ring, signal_fd, ENG_SIGNAL_FD_SLOT, signal_fixed, k_ud_signal_cancel) == 0) {
        cancel_in_flight = true;
    }
    if (inotify_armed &&
        prep_cancel_fd(ring, s_inotify_fd, ENG_INOTIFY_FD_SLOT, s_inotify_fixed,
                       k_ud_inotify_cancel) == 0) {
        inotify_cancel = true;
    }

    uint64_t drain_deadline = ring_mono_ns() + 3000000000ull;
    while ((send_in_flight || wl->pong_in_flight || signal_poll_armed || cancel_in_flight ||
            inotify_armed || inotify_cancel) &&
           !wl->failed) {
        if (ring_mono_ns() > drain_deadline) {
            fprintf(stderr, "[Engine Error] Idle teardown drain timed out\n");
            run_rc = 1;
            break;
        }
        struct io_uring_cqe cqe;
        if (!ring_peek(ring, &cqe)) {
            unsigned submitted = ring_submit(ring);
            int rc = ring_enter(ring, submitted, 1, 200);
            if (rc < 0 && rc != -ETIME && rc != -EINTR) {
                run_rc = 1;
                break;
            }
            continue;
        }
        hud_note_cqe();
        if (cqe.user_data == ENG_UD_WL_SEND) {
            int noted = finish_idle_send(wl, vk, &cqe, pending_serial);
            if (noted < 0) {
                send_in_flight = false;
                run_rc = 1;
            } else if (noted == 1) {
                if (wayland_queue_send(wl, wl->send_tracks_ack) == 0) {
                    send_in_flight = true;
                } else {
                    send_in_flight = false;
                    run_rc = 1;
                }
            } else {
                send_in_flight = false;
            }
        } else if (cqe.user_data == ENG_UD_SIGNAL) {
            signal_poll_armed = false;
            if (cqe.res == (int)sizeof s_siginfo) {
                signo = (int)s_siginfo.ssi_signo;
                got_signal = true;
                why = "signal";
            }
        } else if (cqe.user_data == k_ud_signal_cancel) {
            cancel_in_flight = false;
        } else if (cqe.user_data == ENG_UD_INOTIFY) {
            inotify_armed = false;
        } else if (cqe.user_data == k_ud_inotify_cancel) {
            inotify_cancel = false;
        } else {
            wayland_on_cqe(wl, &cqe);
        }
    }

    if (signal_fixed) {
        (void)ring_update_file(ring, ENG_SIGNAL_FD_SLOT, -1);
    }
    close(signal_fd);
    if (s_inotify_fixed) {
        (void)ring_update_file(ring, ENG_INOTIFY_FD_SLOT, -1);
        s_inotify_fixed = false;
    }
    if (s_inotify_fd >= 0) {
        close(s_inotify_fd);
        s_inotify_fd = -1;
        s_inotify_wd = -1;
    }
    (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);

    if (got_signal && signo != 0) {
        printf("[Engine] Idle ended (signal %d)\n", signo);
    } else {
        printf("[Engine] Idle ended (%s)\n", why);
    }
    fflush(stdout);
    return run_rc;
}
