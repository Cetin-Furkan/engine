#include "ring/internal.h"
#include <errno.h>

static int cancel_file_chain(RingContext *r) {
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
        sqe->addr      = k_file_ops[i];
        sqe->user_data = ENG_UD_FILE_CANCEL;
    }
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

    bool got_read = false;
    bool got_close = false;
    bool cancel_sent = false;
    int32_t read_res = -EIO;
    int32_t root = 0;
    int pending_error = 0;
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(r, &cqe)) {
            if (cqe.user_data == ENG_UD_FILE_OPEN) {
                if (cqe.res != 0 && root == 0) {
                    root = cqe.res;
                }
            } else if (cqe.user_data == ENG_UD_FILE_READ) {
                got_read = true;
                read_res = cqe.res;
            } else if (cqe.user_data == ENG_UD_FILE_CLOSE) {
                got_close = true;
                if (cqe.res < 0 && cqe.res != -ECANCELED && root == 0) {
                    root = cqe.res;
                }
            } else if (cqe.user_data == ENG_UD_FILE_CANCEL) {
                // Cancel completions stay here. path and buf belong to the caller.
            } else if (on_cqe != nullptr) {
                on_cqe(&cqe, on_user);
            }
        }
        // Close posts its own CQE, including -ECANCELED when open fails.
        if (got_close && got_read) {
            break;
        }
        if (ring_mono_ns() > deadline) {
            if (!cancel_sent) {
                int crc = cancel_file_chain(r);
                cancel_sent = true;
                if (pending_error == 0) {
                    pending_error = (crc != 0) ? crc : -ETIMEDOUT;
                }
                deadline = ring_mono_ns() + 1000000000ull;
                continue;
            }
            break;
        }
        unsigned n = ring_submit(r);
        int rc = ring_enter(r, n, 1, 200);
        if (rc < 0 && rc != -ETIME && rc != -EINTR) {
            if (!cancel_sent) {
                int crc = cancel_file_chain(r);
                cancel_sent = true;
                pending_error = (crc != 0) ? crc : rc;
                deadline = ring_mono_ns() + 1000000000ull;
                continue;
            }
            if (pending_error == 0) {
                pending_error = rc;
            }
            break;
        }
    }

    if (!got_close) {
        return pending_error != 0 ? pending_error : -ETIMEDOUT;
    }
    if (root < 0) {
        return root;
    }
    if (!got_read || read_res < 0) {
        return got_read ? read_res : -EIO;
    }
    if ((size_t)read_res >= cap) {
        return -EFBIG;
    }
    ((uint8_t *)buf)[read_res] = 0;
    *out_len = (size_t)read_res;
    return 0;
}
