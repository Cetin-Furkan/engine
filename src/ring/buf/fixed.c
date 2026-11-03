#include "ring/internal.h"
#include <errno.h>
#include <sys/uio.h>

static void lay_out(RingContext *r) {
    uint32_t at = 0;
    r->fixed_len[ENG_FBUF_INOTIFY] = ENG_FBUF_INOTIFY_LEN;
    r->fixed_len[ENG_FBUF_SIGNAL]  = ENG_FBUF_SIGNAL_LEN;
    r->fixed_len[ENG_FBUF_RELEASE] = ENG_FBUF_RELEASE_LEN;
    r->fixed_at[ENG_FBUF_INOTIFY] = at;
    at += ENG_FBUF_INOTIFY_LEN;
    r->fixed_at[ENG_FBUF_SIGNAL] = at;
    at += ENG_FBUF_SIGNAL_LEN;
    r->fixed_at[ENG_FBUF_RELEASE] = at;
}

uint8_t *ring_fixed_ptr(const RingContext *r, uint32_t buf_index) {
    if (r == nullptr || r->fixed_base == nullptr || buf_index >= ENG_FBUF_COUNT) {
        return nullptr;
    }
    return r->fixed_base + r->fixed_at[buf_index];
}

int ring_fixed_register(RingContext *r) {
    lay_out(r);
    struct iovec iov[ENG_FBUF_COUNT];
    for (uint32_t i = 0; i < ENG_FBUF_COUNT; ++i) {
        iov[i].iov_base = r->fixed_base + r->fixed_at[i];
        iov[i].iov_len  = r->fixed_len[i];
    }
    long rc = ring_register(r, IORING_REGISTER_BUFFERS, (long)iov, ENG_FBUF_COUNT);
    if (rc < 0) {
        return (int)rc;
    }
    r->fixed_on = true;
    return 0;
}

int ring_arm_read_fixed(RingContext *r, uint32_t slot, bool fixed_file, int raw_fd,
                        uint32_t buf_index, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized || !r->fixed_on || buf_index >= ENG_FBUF_COUNT ||
        (!fixed_file && raw_fd < 0)) {
        return -EINVAL;
    }
    if (fixed_file && slot >= ENG_FIXED_FILES) {
        return -EINVAL;
    }
    uint8_t *p = ring_fixed_ptr(r, buf_index);
    if (p == nullptr) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_READ_FIXED;
    sqe->fd        = fixed_file ? (int)slot : raw_fd;
    sqe->flags     = fixed_file ? IOSQE_FIXED_FILE : 0;
    sqe->addr      = (uint64_t)(uintptr_t)p;
    sqe->len       = r->fixed_len[buf_index];
    sqe->off       = 0;
    sqe->buf_index = (uint16_t)buf_index;
    sqe->user_data = user_data;
    return 0;
}
