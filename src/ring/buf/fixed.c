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
    at += ENG_FBUF_RELEASE_LEN;
    at = (uint32_t)ring_align_up(at, 64u);
    for (uint32_t i = 0; i < ENG_FBUF_PAGER_COUNT; ++i) {
        uint32_t slot = ENG_FBUF_PAGER_BASE + i;
        r->fixed_at[slot]  = at;
        r->fixed_len[slot] = ENG_FBUF_PAGER_LEN;
        at += ENG_FBUF_PAGER_LEN;
    }
}

uint8_t *ring_fixed_ptr(const RingContext *r, uint32_t buf_index) {
    if (r == nullptr || r->fixed_base == nullptr || buf_index >= r->fixed_count) {
        return nullptr;
    }
    return r->fixed_base + r->fixed_at[buf_index];
}

int ring_fixed_register(RingContext *r) {
    lay_out(r);
    struct iovec iov[ENG_FBUF_COUNT];
    uint32_t want = r->pager_fixed_on ? ENG_FBUF_COUNT : ENG_FBUF_CORE_COUNT;
    for (uint32_t i = 0; i < want; ++i) {
        iov[i].iov_base = r->fixed_base + r->fixed_at[i];
        iov[i].iov_len  = r->fixed_len[i];
    }
    long rc = ring_register(r, IORING_REGISTER_BUFFERS, (long)iov, want);
    if (rc < 0 && want > ENG_FBUF_CORE_COUNT) {
        // Graceful RLIMIT_MEMLOCK fallback: keep core buffers registered even on tight ulimit -l.
        r->pager_fixed_on = false;
        want = ENG_FBUF_CORE_COUNT;
        rc = ring_register(r, IORING_REGISTER_BUFFERS, (long)iov, want);
    }
    if (rc < 0) {
        r->pager_fixed_on = false;
        r->fixed_count = 0;
        return (int)rc;
    }
    r->fixed_count = want;
    r->fixed_on = true;
    return 0;
}

int ring_arm_read_fixed(RingContext *r, uint32_t slot, bool fixed_file, int raw_fd,
                        uint32_t buf_index, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized || !r->fixed_on || buf_index >= r->fixed_count ||
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
