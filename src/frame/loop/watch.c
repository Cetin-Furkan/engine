#include "frame/frame_int.h"

// Chrome file watch. A reload hands stolen completions back to the idle loop.
// The inotify bytes land in the ring's registered buffer.

static void absorb_inotify(int32_t res);

static uint8_t *inotify_bytes(void) {
    return ring_fixed_ptr(ring_get_context(), ENG_FBUF_INOTIFY);
}

void ensure_inotify_watch(void) {
    if (frame_state.inotify_fd < 0) {
        return;
    }
    if (frame_state.inotify_wd < 0) {
        const ChromeSpec *spec = chrome_spec();
        if (spec != nullptr && spec->path[0] != '\0') {
            int wd = inotify_add_watch(frame_state.inotify_fd, spec->path, k_inotify_mask);
            if (wd < 0) {
                if (!frame_state.inotify_warned) {
                    fprintf(stderr, "[Chrome] inotify watch failed: %s\n", strerror(errno));
                    frame_state.inotify_warned = true;
                }
            } else {
                frame_state.inotify_wd = wd;
            }
        }
    }
    const char *app_path = (engine_app()->watched_path != nullptr)
        ? engine_app()->watched_path()
        : nullptr;
    if (app_path == nullptr || app_path[0] == '\0') {
        if (frame_state.world_wd >= 0) {
            (void)inotify_rm_watch(frame_state.inotify_fd, frame_state.world_wd);
            frame_state.world_wd = -1;
        }
        frame_state.world_watch_path[0] = '\0';
    } else {
        if (frame_state.world_wd >= 0 &&
            strcmp(frame_state.world_watch_path, app_path) != 0) {
            (void)inotify_rm_watch(frame_state.inotify_fd, frame_state.world_wd);
            frame_state.world_wd = -1;
        }
        if (frame_state.world_wd < 0) {
            int wd = inotify_add_watch(frame_state.inotify_fd, app_path, k_inotify_mask);
            if (wd >= 0) {
                frame_state.world_wd = wd;
                snprintf(frame_state.world_watch_path, sizeof(frame_state.world_watch_path),
                         "%s", app_path);
            }
        }
    }
}

static void absorb_inotify(int32_t res) {
    if (res <= 0 || inotify_bytes() == nullptr) {
        return;
    }
    bool dropped = false;
    int off = 0;
    while (off + (int)sizeof(struct inotify_event) <= res) {
        const struct inotify_event *ev =
            (const struct inotify_event *)(inotify_bytes() + off);
        uint32_t step = (uint32_t)sizeof(*ev) + ev->len;
        if (off + (int)step > res) {
            break;
        }
        if (ev->wd == frame_state.world_wd && frame_state.world_wd >= 0) {
            if ((ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                dropped = true;
                frame_state.world_wd = -1;
            }
            if ((ev->mask & (k_inotify_mask | IN_IGNORED)) != 0) {
                frame_state.world_dirty = true;
            }
        } else {
            if ((ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW)) != 0) {
                dropped = true;
                frame_state.inotify_wd = -1;
            }
            if ((ev->mask & (k_inotify_mask | IN_IGNORED | IN_Q_OVERFLOW)) != 0) {
                frame_state.chrome_dirty = true;
            }
        }
        off += (int)step;
    }
    if (dropped) {
        ensure_inotify_watch();
    }
}

void note_inotify_cqe(RingContext *ring, int32_t res, bool *armed) {
    if (armed != nullptr) {
        *armed = false;
    }
    if (res < 0 && res != -ECANCELED && res != -EINTR) {
        if (!frame_state.inotify_warned) {
            fprintf(stderr, "[Chrome] inotify read: %s\n", strerror(-res));
            frame_state.inotify_warned = true;
        }
        return;
    }
    if (res > 0) {
        absorb_inotify(res);
    }
    if (frame_state.inotify_stop || frame_state.inotify_fd < 0 || ring == nullptr) {
        return;
    }
    if (arm_slot_read(ring, ENG_INOTIFY_FD_SLOT, frame_state.inotify_fixed, frame_state.inotify_fd,
                      ENG_FBUF_INOTIFY, ENG_UD_INOTIFY) == 0 &&
        armed != nullptr) {
        *armed = true;
    }
}

void start_inotify(RingContext *ring, bool *armed) {
    if (armed != nullptr) {
        *armed = false;
    }
    frame_state.inotify_stop = false;
    const ChromeSpec *spec = chrome_spec();
    if (spec == nullptr || spec->path[0] == '\0') {
        return;
    }
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) {
        if (!frame_state.inotify_warned) {
            fprintf(stderr, "[Chrome] inotify unavailable: %s\n", strerror(errno));
            frame_state.inotify_warned = true;
        }
        return;
    }
    frame_state.inotify_fd = fd;
    frame_state.inotify_wd = -1;
    frame_state.world_wd = -1;
    frame_state.world_dirty = false;
    frame_state.world_watch_path[0] = '\0';
    frame_state.inotify_fixed = false;
    ensure_inotify_watch();
    if (frame_state.inotify_wd < 0 && frame_state.world_wd < 0) {
        close(frame_state.inotify_fd);
        frame_state.inotify_fd = -1;
        return;
    }
    if (ring_update_file(ring, ENG_INOTIFY_FD_SLOT, frame_state.inotify_fd) == 0) {
        frame_state.inotify_fixed = true;
    }
    if (arm_slot_read(ring, ENG_INOTIFY_FD_SLOT, frame_state.inotify_fixed, frame_state.inotify_fd,
                      ENG_FBUF_INOTIFY, ENG_UD_INOTIFY) != 0) {
        fprintf(stderr, "[Chrome] inotify read was not armed\n");
        if (frame_state.inotify_fixed) {
            (void)ring_update_file(ring, ENG_INOTIFY_FD_SLOT, -1);
            frame_state.inotify_fixed = false;
        }
        close(frame_state.inotify_fd);
        frame_state.inotify_fd = -1;
        frame_state.inotify_wd = -1;
        return;
    }
    if (armed != nullptr) {
        *armed = true;
    }
}

void engine_reload_cqe(const struct io_uring_cqe *cqe, void *user) {
    (void)user;
    WaylandContext *wl = wayland_get_context();
    if (frame_state.relay == nullptr) {
        if (wl != nullptr) {
            wayland_on_cqe(wl, cqe);
        }
        return;
    }
    if (cqe->user_data == ENG_UD_WL_SEND) {
        if (!frame_state.relay->stolen_send) {
            frame_state.relay->stolen_send = true;
            frame_state.relay->stolen_cqe = *cqe;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_SIGNAL) {
        if (frame_state.relay->signal_armed != nullptr) {
            *frame_state.relay->signal_armed = false;
        }
        if (frame_state.relay->signo != nullptr) {
            int signo = frame_signal_number(cqe->res);
            if (signo != 0) {
                *frame_state.relay->signo = signo;
            }
        }
        if (frame_state.relay->got_signal != nullptr) {
            *frame_state.relay->got_signal = true;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_INOTIFY) {
        note_inotify_cqe(ring_get_context(), cqe->res, frame_state.relay->inotify_armed);
        return;
    }
    // The simulation timer and the frame watchdog share this ring. Running
    // either inside a file-load wait would encode Wayland mid-batch.
    if (cqe->user_data == ENG_UD_SIM_TIMER || cqe->user_data == ENG_UD_FRAME_TIMER) {
        if (wl != nullptr) {
            wayland_on_cqe(wl, cqe);
        }
        return;
    }
    if (wl != nullptr) {
        wayland_on_cqe(wl, cqe);
    } else if (ring_ud_is_pager(cqe->user_data) && engine_app()->on_cqe != nullptr) {
        if (engine_app()->on_cqe(cqe)) {
            frame_note_picture();
        }
    }
}

void engine_relay_begin(EngineRelay *relay, bool *signal_armed, bool *got_signal,
                               int *signo, bool *inotify_armed) {
    *relay = (RunRelay){
        .signal_armed  = signal_armed,
        .got_signal    = got_signal,
        .signo         = signo,
        .inotify_armed = inotify_armed,
    };
    frame_state.relay = relay;
}

// True when the idle loop must leave. A stolen send that failed, a signal,
// or a failed Wayland socket sets why. The two file reads share this so the
// send completion cannot be finished in one path and dropped in the other.
bool engine_relay_end(EngineRelay *relay, WaylandContext *wl, VulkanContext *vk,
                           uint32_t pending_serial, bool *send_in_flight, bool *resend_wire,
                           int *run_rc, const char **why) {
    frame_state.relay = nullptr;
    if (relay->stolen_send) {
        int noted = finish_idle_send(wl, vk, &relay->stolen_cqe, pending_serial);
        if (noted < 0) {
            *send_in_flight = false;
            *run_rc = 1;
            *why = wl->err[0] != '\0' ? wl->err : "wayland send failed";
            return true;
        }
        if (noted == 1) {
            *resend_wire = true;
        }
    }
    if (relay->got_signal != nullptr && *relay->got_signal) {
        *why = "signal";
        return true;
    }
    if (wl->failed) {
        *why = wl->err;
        *run_rc = 1;
        return true;
    }
    return false;
}

void forward_reload_cqe(const struct io_uring_cqe *cqe, void *user) {
    engine_reload_cqe(cqe, user);
}

void begin_file_relay(RunRelay *relay, bool *signal_armed, bool *got_signal,
                             int *signo, bool *inotify_armed) {
    engine_relay_begin(relay, signal_armed, got_signal, signo, inotify_armed);
}

bool end_file_relay(RunRelay *relay, WaylandContext *wl, VulkanContext *vk,
                           uint32_t pending_serial, bool *send_in_flight, bool *resend_wire,
                           int *run_rc, const char **why) {
    return engine_relay_end(relay, wl, vk, pending_serial, send_in_flight,
                            resend_wire, run_rc, why);
}
