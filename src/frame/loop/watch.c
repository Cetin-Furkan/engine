#include "frame/frame_int.h"

// Chrome file watch. A reload hands stolen completions back to the idle loop.

alignas(struct inotify_event) static uint8_t s_inotify_buf[4096];

static void absorb_inotify(int32_t res);

void ensure_inotify_watch(void) {
    if (s_inotify_fd < 0 || s_inotify_wd >= 0) {
        return;
    }
    const ChromeSpec *spec = chrome_spec();
    if (spec == nullptr || spec->path[0] == '\0') {
        return;
    }
    int wd = inotify_add_watch(s_inotify_fd, spec->path, k_inotify_mask);
    if (wd < 0) {
        if (!s_inotify_warned) {
            fprintf(stderr, "[Chrome] inotify watch failed: %s\n", strerror(errno));
            s_inotify_warned = true;
        }
        return;
    }
    s_inotify_wd = wd;
}

static void absorb_inotify(int32_t res) {
    if (res <= 0) {
        return;
    }
    bool dropped = false;
    int off = 0;
    while (off + (int)sizeof(struct inotify_event) <= res) {
        const struct inotify_event *ev =
            (const struct inotify_event *)(s_inotify_buf + off);
        uint32_t step = (uint32_t)sizeof(*ev) + ev->len;
        if (off + (int)step > res) {
            break;
        }
        if ((ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW)) != 0) {
            dropped = true;
            s_inotify_wd = -1;
        }
        if ((ev->mask & (k_inotify_mask | IN_IGNORED | IN_Q_OVERFLOW)) != 0) {
            s_chrome_dirty = true;
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
        if (!s_inotify_warned) {
            fprintf(stderr, "[Chrome] inotify read: %s\n", strerror(-res));
            s_inotify_warned = true;
        }
        return;
    }
    if (res > 0) {
        absorb_inotify(res);
    }
    if (s_inotify_stop || s_inotify_fd < 0 || ring == nullptr) {
        return;
    }
    if (arm_slot_read(ring, ENG_INOTIFY_FD_SLOT, s_inotify_fixed, s_inotify_fd,
                      s_inotify_buf, (uint32_t)sizeof s_inotify_buf, ENG_UD_INOTIFY) == 0 &&
        armed != nullptr) {
        *armed = true;
    }
}

void start_inotify(RingContext *ring, bool *armed) {
    if (armed != nullptr) {
        *armed = false;
    }
    s_inotify_stop = false;
    const ChromeSpec *spec = chrome_spec();
    if (spec == nullptr || spec->path[0] == '\0') {
        return;
    }
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) {
        if (!s_inotify_warned) {
            fprintf(stderr, "[Chrome] inotify unavailable: %s\n", strerror(errno));
            s_inotify_warned = true;
        }
        return;
    }
    s_inotify_fd = fd;
    s_inotify_wd = -1;
    s_inotify_fixed = false;
    ensure_inotify_watch();
    if (s_inotify_wd < 0) {
        close(s_inotify_fd);
        s_inotify_fd = -1;
        return;
    }
    if (ring_update_file(ring, ENG_INOTIFY_FD_SLOT, s_inotify_fd) == 0) {
        s_inotify_fixed = true;
    }
    if (arm_slot_read(ring, ENG_INOTIFY_FD_SLOT, s_inotify_fixed, s_inotify_fd,
                      s_inotify_buf, (uint32_t)sizeof s_inotify_buf, ENG_UD_INOTIFY) != 0) {
        fprintf(stderr, "[Chrome] inotify read was not armed\n");
        if (s_inotify_fixed) {
            (void)ring_update_file(ring, ENG_INOTIFY_FD_SLOT, -1);
            s_inotify_fixed = false;
        }
        close(s_inotify_fd);
        s_inotify_fd = -1;
        s_inotify_wd = -1;
        return;
    }
    if (armed != nullptr) {
        *armed = true;
    }
}

void forward_reload_cqe(const struct io_uring_cqe *cqe, void *user) {
    (void)user;
    WaylandContext *wl = wayland_get_context();
    if (s_relay == nullptr) {
        if (wl != nullptr) {
            wayland_on_cqe(wl, cqe);
        }
        return;
    }
    if (cqe->user_data == ENG_UD_WL_SEND) {
        if (!s_relay->stolen_send) {
            s_relay->stolen_send = true;
            s_relay->stolen_cqe = *cqe;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_SIGNAL) {
        if (s_relay->signal_armed != nullptr) {
            *s_relay->signal_armed = false;
        }
        if (cqe->res == (int)sizeof s_siginfo && s_relay->signo != nullptr) {
            *s_relay->signo = (int)s_siginfo.ssi_signo;
        }
        if (s_relay->got_signal != nullptr) {
            *s_relay->got_signal = true;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_INOTIFY) {
        note_inotify_cqe(ring_get_context(), cqe->res, s_relay->inotify_armed);
        return;
    }
    if (wl != nullptr) {
        wayland_on_cqe(wl, cqe);
    }
}

void begin_file_relay(RunRelay *relay, bool *signal_armed, bool *got_signal,
                             int *signo, bool *inotify_armed) {
    *relay = (RunRelay){
        .signal_armed  = signal_armed,
        .got_signal    = got_signal,
        .signo         = signo,
        .inotify_armed = inotify_armed,
    };
    s_relay = relay;
}

// True when the idle loop must leave. A stolen send that failed, a signal,
// or a failed Wayland socket sets why. The two file reads share this so the
// send completion cannot be finished in one path and dropped in the other.
bool end_file_relay(RunRelay *relay, WaylandContext *wl, VulkanContext *vk,
                           uint32_t pending_serial, bool *send_in_flight, bool *resend_wire,
                           int *run_rc, const char **why) {
    s_relay = nullptr;
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
