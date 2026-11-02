// io_uring sends, round-trips, and the pong buffer.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>


int wl_arm_pong(WaylandContext *w) {
    if (w->pong_off > 12u) {
        return -EIO;
    }
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_SEND;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)(w->pong_bytes + w->pong_off);
    sqe->len       = 12u - w->pong_off;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_PONG;
    w->pong_in_flight = true;
    return 0;
}


void wl_queue_xdg_pong(WaylandContext *w, uint32_t serial) {
    if (w->xdg_wm_base_id == 0) {
        return;
    }
    if (w->pong_in_flight) {
        wl_fail(w, "xdg_wm_base.ping received while pong is already in flight");
        return;
    }
    wl_put_u32(w->pong_bytes + 0, w->xdg_wm_base_id);
    wl_put_u16(w->pong_bytes + 4, 3u); // xdg_wm_base.pong opcode = 3
    wl_put_u16(w->pong_bytes + 6, 12u);
    wl_put_u32(w->pong_bytes + 8, serial);
    w->pong_off = 0;
    if (wl_arm_pong(w) != 0) {
        wl_fail(w, "SQ full while queuing xdg_wm_base.pong");
    }
}


int wl_wait_tag(WaylandContext *w, uint64_t tag, struct io_uring_cqe *out) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(w->ring, &cqe)) {
            if (cqe.user_data == tag) {
                *out = cqe;
                return 0;
            }
            wayland_on_cqe(w, &cqe);
            if (w->failed) return -EIO;
        }
        if (ring_mono_ns() > deadline) {
            wl_fail(w, "wayland io_uring operation timed out");
            return -ETIMEDOUT;
        }
        if (w->recv_msg.msg_iov != nullptr && !w->recv_armed) {
            (void)wayland_ensure_recv(w);
        }
        unsigned n = ring_submit(w->ring);
        int rc = ring_enter(w->ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME) {
            wl_fail_err(w, "io_uring_enter", rc);
            return rc;
        }
    }
}


int wl_note_send_progress(WaylandContext *w, int32_t res) {
    if (w->send_off > w->send_len) {
        wl_fail(w, "wayland send cursor past the buffer");
        w->send_len = 0;
        w->send_off = 0;
        w->send_tracks_ack = false;
        return -EIO;
    }
    uint32_t remaining = w->send_len - w->send_off;
    if (remaining == 0) {
        w->send_len = 0;
        w->send_off = 0;
        return 0;
    }
    if (res < 0) {
        if (res == -EAGAIN || res == -EINTR) {
            return 1;
        }
        wl_fail_err(w, "wayland send", res);
        w->send_len = 0;
        w->send_off = 0;
        w->send_tracks_ack = false;
        return -EIO;
    }
    if ((uint32_t)res > remaining) {
        wl_fail(w, "wayland send wrote past the buffer");
        w->send_len = 0;
        w->send_off = 0;
        w->send_tracks_ack = false;
        return -EIO;
    }
    if (res == 0) {
        return 1;
    }
    w->send_off += (uint32_t)res;
    if (w->send_off < w->send_len) {
        return 1;
    }
    w->send_len = 0;
    w->send_off = 0;
    return 0;
}


bool wl_send_blocked(const WaylandContext *w) {
    return w->send_queued || w->noted_send;
}


int wl_send_with_fd(WaylandContext *w, int fd_to_send) {
    if (w->send_off >= w->send_len) {
        w->send_len = 0;
        w->send_off = 0;
        return 0;
    }
    if (w->failed) {
        return -EIO;
    }
    if (wl_send_blocked(w)) {
        return -EBUSY;
    }

    for (;;) {
        uint32_t remaining = w->send_len - w->send_off;
        struct io_uring_sqe *sqe = ring_sqe(w->ring);
        if (sqe == nullptr) {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            wl_fail(w, "SQ full while sending wire batch");
            return -ENOSPC;
        }

        // SCM_RIGHTS rides the first accepted byte. A retry after that byte
        // is a plain send. A retry before any byte still carries the caller's fd.
        bool with_cmsg = fd_to_send >= 0 && w->send_off == 0;
        if (with_cmsg) {
            w->send_iov.iov_base   = w->send_bytes + w->send_off;
            w->send_iov.iov_len    = remaining;
            w->send_msg.msg_iov    = &w->send_iov;
            w->send_msg.msg_iovlen = 1u;

            memset(w->send_control, 0, sizeof(w->send_control));
            w->send_msg.msg_control    = w->send_control;
            w->send_msg.msg_controllen = sizeof(w->send_control);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&w->send_msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

            sqe->opcode    = IORING_OP_SENDMSG;
            sqe->flags     = IOSQE_FIXED_FILE;
            sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
            sqe->fd        = ENG_WL_SOCK_SLOT;
            sqe->addr      = (uint64_t)(uintptr_t)&w->send_msg;
            sqe->msg_flags = MSG_NOSIGNAL;
            sqe->user_data = ENG_UD_WL_SEND;
        } else {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;

            sqe->opcode    = IORING_OP_SEND;
            sqe->flags     = IOSQE_FIXED_FILE;
            sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
            sqe->fd        = ENG_WL_SOCK_SLOT;
            sqe->addr      = (uint64_t)(uintptr_t)(w->send_bytes + w->send_off);
            sqe->len       = remaining;
            sqe->msg_flags = MSG_NOSIGNAL;
            sqe->user_data = ENG_UD_WL_SEND;
        }

        struct io_uring_cqe cqe = {0};
        w->send_queued = true;
        int rc = wl_wait_tag(w, ENG_UD_WL_SEND, &cqe);
        if (rc < 0) {
            // The SQE may still be in flight. Leave send_queued set so
            // nothing else submits ENG_UD_WL_SEND over this buffer.
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            return rc;
        }
        w->send_queued = false;
        int progress = wl_note_send_progress(w, cqe.res);
        if (progress < 0) {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            return progress;
        }
        if (progress == 0) {
            break;
        }
    }

    w->send_msg.msg_control    = nullptr;
    w->send_msg.msg_controllen = 0u;
    return 0;
}


int wl_send_batch(WaylandContext *w) {
    return wl_send_with_fd(w, -1);
}


int wl_flush_pending(WaylandContext *w) {
    if (w->noted_send) {
        w->noted_send = false;
        int prog = wl_note_send_progress(w, w->noted_send_res);
        if (prog < 0) {
            return prog;
        }
    }
    if (w->send_len == 0) {
        return 0;
    }
    return wl_send_batch(w);
}


int wayland_send_batch(WaylandContext *w) {
    return wl_send_batch(w);
}


int wayland_roundtrip_batch(WaylandContext *w, int fd_to_send) {
    return wl_finish_roundtrip(w, fd_to_send);
}


int wl_finish_roundtrip(WaylandContext *w, int fd_to_send) {
    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    if (w->sync_id == 0 || !wl_append_display_sync(w, w->sync_id)) {
        return -EIO;
    }
    if (wl_send_with_fd(w, fd_to_send) < 0) {
        return -EIO;
    }
    struct io_uring_cqe cqe = {0};
    while (!w->synced && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }
    if (w->failed || !w->synced) {
        return -EIO;
    }
    return 0;
}


int wayland_queue_send(WaylandContext *w, bool tracks_ack) {
    if (w == nullptr || !w->is_initialized || w->failed || w->send_len == 0) {
        return -EINVAL;
    }
    if (w->send_off >= w->send_len) {
        return -EINVAL;
    }
    if (wl_send_blocked(w)) {
        return -EBUSY;
    }
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while queuing wayland send");
        return -ENOSPC;
    }
    w->send_tracks_ack = tracks_ack;
    w->send_msg.msg_control    = nullptr;
    w->send_msg.msg_controllen = 0u;

    sqe->opcode    = IORING_OP_SEND;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)(w->send_bytes + w->send_off);
    sqe->len       = w->send_len - w->send_off;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_SEND;
    w->send_queued = true;
    return 0;
}


bool wayland_defer_destroy(WaylandContext *w, uint32_t id) {
    if (w == nullptr || id == 0 || w->destroy_later_n >= ENG_WL_DESTROY_LATER) {
        return false;
    }
    for (uint32_t i = 0; i < w->destroy_later_n; ++i) {
        if (w->destroy_later[i] == id) {
            return true;
        }
    }
    w->destroy_later[w->destroy_later_n++] = id;
    return true;
}


int wayland_queue_destroys(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized || w->failed) {
        return -EINVAL;
    }
    if (w->destroy_later_n == 0) {
        return 0;
    }
    if (wl_send_blocked(w) || w->send_len != 0) {
        return -EBUSY;
    }
    uint32_t mark = w->send_len;
    bool ok = true;
    for (uint32_t i = 0; i < w->destroy_later_n && ok; ++i) {
        ok = wayland_put_raw(w, w->destroy_later[i], 0u, nullptr, 0u);
    }
    if (!ok) {
        wayland_rewind_send(w, mark);
        return -EIO;
    }
    int queued = wayland_queue_send(w, false);
    if (queued != 0) {
        wayland_rewind_send(w, mark);
        return queued;
    }
    w->destroy_later_n = 0;
    return 1;
}


int wayland_note_send_cqe(
    WaylandContext            *w,
    const struct io_uring_cqe *cqe,
    uint32_t                   acked_serial
) {
    if (w == nullptr || cqe == nullptr) {
        return -EINVAL;
    }
    w->send_queued = false;
    int progress = wl_note_send_progress(w, cqe->res);
    if (progress != 0) {
        return progress;
    }
    bool track = w->send_tracks_ack;
    w->send_tracks_ack = false;
    if (track) {
        w->acked_configure_serial = acked_serial;
        w->configure_ack_valid = true;
    }
    return 0;
}

