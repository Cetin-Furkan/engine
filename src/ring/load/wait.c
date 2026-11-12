#include "ring/internal.h"
#include <errno.h>

static int cancel_file_chain(RingContext *r, uint32_t *inflight) {
    static const uint64_t k_file_ops[3] = {
        ENG_UD_FILE_OPEN,
        ENG_UD_FILE_READ,
        ENG_UD_FILE_CLOSE,
    };
    uint32_t held = r->sq_pending;
    for (uint32_t i = 0; i < 3u; ++i) {
        struct io_uring_sqe *sqe = ring_sqe(r);
        if (sqe == nullptr) {
            r->sq_pending = held;
            return -ENOSPC;
        }
        sqe->opcode    = IORING_OP_ASYNC_CANCEL;
        sqe->flags     = IORING_ASYNC_CANCEL_ALL;
        sqe->addr      = k_file_ops[i];
        sqe->user_data = ENG_UD_FILE_CANCEL;
    }
    *inflight += 3u;
    return 0;
}

int ring_read_file_all(RingContext *r, const char *path, void *buf, size_t cap, size_t *out_len,
                       RingCqeFn on_cqe, void *on_user) {
    if (r == nullptr || !r->is_initialized || path == nullptr || path[0] == '\0' ||
        buf == nullptr || out_len == nullptr || cap < 1u || cap > 0xffffffffu) {
        return -EINVAL;
    }

    int sub_rc = ring_submit_read_file(r, path, buf, cap,
                                       ENG_UD_FILE_OPEN, ENG_UD_FILE_READ, ENG_UD_FILE_CLOSE);
    if (sub_rc != 0) {
        return sub_rc;
    }

    RingCqeFn cb      = (on_cqe != nullptr) ? on_cqe : r->default_cqe_fn;
    void     *cb_user = (on_cqe != nullptr) ? on_user : r->default_cqe_user;

    uint32_t inflight      = 2u; // OPENAT + initial READ
    size_t   total         = 0u;
    bool     slot_open     = false;
    bool     read_done     = false;
    bool     close_sent    = false;
    bool     close_done    = false;
    bool     cancel_sent   = false;
    int32_t  root          = 0;
    int      pending_error = 0;
    uint64_t deadline      = ring_mono_ns() + 3000000000ull;

    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(r, &cqe)) {
            if (cqe.user_data == ENG_UD_FILE_OPEN) {
                if (inflight > 0u) {
                    inflight--;
                }
                if (cqe.res < 0) {
                    if (root == 0) {
                        root = cqe.res;
                    }
                } else {
                    slot_open = true;
                }
            } else if (cqe.user_data == ENG_UD_FILE_READ) {
                if (inflight > 0u) {
                    inflight--;
                }
                if (cqe.res < 0) {
                    if (cqe.res != -ECANCELED && root == 0) {
                        root = cqe.res;
                    }
                    read_done = true;
                } else if (cqe.res == 0) {
                    read_done = true;
                } else {
                    total += (size_t)cqe.res;
                    if (total >= cap) {
                        if (root == 0) {
                            root = -EFBIG;
                        }
                        read_done = true;
                    } else if (cancel_sent || pending_error != 0 || root < 0) {
                        read_done = true;
                    } else {
                        int mrc = ring_submit_read_more(r,
                                                        (uint8_t *)buf + total,
                                                        (uint32_t)(cap - total),
                                                        (uint64_t)total,
                                                        ENG_UD_FILE_READ);
                        if (mrc == 0) {
                            inflight++;
                        } else {
                            if (root == 0) {
                                root = mrc;
                            }
                            read_done = true;
                        }
                    }
                }
            } else if (cqe.user_data == ENG_UD_FILE_CLOSE) {
                if (inflight > 0u) {
                    inflight--;
                }
                slot_open  = false;
                close_done = true;
                if (cqe.res < 0 && cqe.res != -ECANCELED && root == 0) {
                    root = cqe.res;
                }
            } else if (cqe.user_data == ENG_UD_FILE_CANCEL) {
                if (inflight > 0u) {
                    inflight--;
                }
            } else if (cb != nullptr) {
                cb(&cqe, cb_user);
            }
        }

        if (read_done && slot_open && !close_sent && !cancel_sent) {
            int clrc = ring_submit_close_scratch(r, ENG_UD_FILE_CLOSE);
            if (clrc == 0) {
                close_sent = true;
                inflight++;
            } else {
                (void)ring_update_file(r, ENG_SCRATCH_FD_SLOT, -1);
                slot_open  = false;
                close_done = true;
            }
        }

        // Never return while any file/cancel SQE is still in flight in the kernel:
        // path and buf may live on the caller's stack.
        if (inflight == 0u && (read_done || cancel_sent)) {
            if (slot_open && !close_done) {
                (void)ring_update_file(r, ENG_SCRATCH_FD_SLOT, -1);
                slot_open  = false;
                close_done = true;
            }
            break;
        }

        if (!cancel_sent && ring_mono_ns() > deadline) {
            int crc = cancel_file_chain(r, &inflight);
            cancel_sent = true;
            if (pending_error == 0) {
                pending_error = (crc != 0) ? crc : -ETIMEDOUT;
            }
        }

        unsigned n = ring_submit(r);
        int rc = ring_enter(r, n, 1, 200);
        if (rc < 0 && rc != -ETIME && rc != -EINTR) {
            if (!cancel_sent) {
                int crc = cancel_file_chain(r, &inflight);
                cancel_sent   = true;
                pending_error = (crc != 0) ? crc : rc;
                continue;
            }
            if (pending_error == 0) {
                pending_error = rc;
            }
            if (inflight == 0u) {
                break;
            }
        }
    }

    if (pending_error != 0) {
        return pending_error;
    }
    if (root < 0) {
        return root;
    }
    if (!read_done) {
        return -EIO;
    }
    if (total >= cap) {
        return -EFBIG;
    }
    ((uint8_t *)buf)[total] = 0;
    *out_len = total;
    return 0;
}
