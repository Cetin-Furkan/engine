#include "ring/internal.h"
#include <errno.h>
#include <fcntl.h>

int ring_submit_read_file(RingContext *r, const char *path, void *buf, size_t cap,
                          uint64_t open_ud, uint64_t read_ud, uint64_t close_ud) {
    (void)close_ud;
    if (r == nullptr || !r->is_initialized || path == nullptr || path[0] == '\0' ||
        buf == nullptr || cap < 1u || cap > 0xffffffffu) {
        return -EINVAL;
    }

    uint32_t held = r->sq_pending;
    struct io_uring_sqe *open_sqe = ring_sqe(r);
    struct io_uring_sqe *read_sqe = ring_sqe(r);
    if (open_sqe == nullptr || read_sqe == nullptr) {
        r->sq_pending = held;
        return -ENOSPC;
    }

    // Soft-link OPENAT -> first READ. Both post CQEs so ring_read_file_all
    // knows whether ENG_SCRATCH_FD_SLOT was actually opened and can loop on
    // short reads before issuing CLOSE.
    open_sqe->opcode     = IORING_OP_OPENAT;
    open_sqe->flags      = IOSQE_IO_LINK;
    open_sqe->fd         = AT_FDCWD;
    open_sqe->addr       = (uint64_t)(uintptr_t)path;
    open_sqe->len        = 0;
    // A direct descriptor is not a process fd. O_CLOEXEC is rejected.
    open_sqe->open_flags = O_RDONLY;
    open_sqe->file_index = ENG_SCRATCH_FD_SLOT + 1u;
    open_sqe->user_data  = open_ud;

    read_sqe->opcode    = IORING_OP_READ;
    read_sqe->flags     = IOSQE_FIXED_FILE;
    read_sqe->fd        = ENG_SCRATCH_FD_SLOT;
    read_sqe->addr      = (uint64_t)(uintptr_t)buf;
    read_sqe->len       = (uint32_t)cap;
    read_sqe->off       = 0;
    read_sqe->user_data = read_ud;

    unsigned n = ring_submit(r);
    if (n > 0) {
        int rc = ring_enter(r, n, 0, -1);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

int ring_submit_read_more(RingContext *r, void *buf, uint32_t len, uint64_t off, uint64_t read_ud) {
    if (r == nullptr || !r->is_initialized || buf == nullptr || len == 0u) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_READ;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_SCRATCH_FD_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)buf;
    sqe->len       = len;
    sqe->off       = off;
    sqe->user_data = read_ud;
    return 0;
}

int ring_submit_close_scratch(RingContext *r, uint64_t close_ud) {
    if (r == nullptr || !r->is_initialized) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    // Close of a direct descriptor uses file_index (slot + 1); fd must be 0.
    sqe->opcode     = IORING_OP_CLOSE;
    sqe->fd         = 0;
    sqe->file_index = ENG_SCRATCH_FD_SLOT + 1u;
    sqe->user_data  = close_ud;
    return 0;
}
