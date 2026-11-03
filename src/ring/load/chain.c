#include "ring/internal.h"
#include <errno.h>
#include <fcntl.h>

int ring_submit_read_file(RingContext *r, const char *path, void *buf, size_t cap,
                          uint64_t open_ud, uint64_t read_ud, uint64_t close_ud) {
    if (r == nullptr || !r->is_initialized || path == nullptr || path[0] == '\0' ||
        buf == nullptr || cap < 1u || cap > 0xffffffffu) {
        return -EINVAL;
    }

    uint32_t held = r->sq_pending;
    struct io_uring_sqe *open_sqe  = ring_sqe(r);
    struct io_uring_sqe *read_sqe  = ring_sqe(r);
    struct io_uring_sqe *close_sqe = ring_sqe(r);
    if (open_sqe == nullptr || read_sqe == nullptr || close_sqe == nullptr) {
        r->sq_pending = held;
        return -ENOSPC;
    }

    open_sqe->opcode     = IORING_OP_OPENAT;
    open_sqe->flags      = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
    open_sqe->fd         = AT_FDCWD;
    open_sqe->addr       = (uint64_t)(uintptr_t)path;
    open_sqe->len        = 0;
    // A direct descriptor is not a process fd. O_CLOEXEC is rejected.
    open_sqe->open_flags = O_RDONLY;
    open_sqe->file_index = ENG_SCRATCH_FD_SLOT + 1u;
    open_sqe->user_data  = open_ud;

    // HARDLINK: a failed read still runs the close, so the scratch slot is freed.
    read_sqe->opcode    = IORING_OP_READ;
    read_sqe->flags     = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK;
    read_sqe->fd        = ENG_SCRATCH_FD_SLOT;
    read_sqe->addr      = (uint64_t)(uintptr_t)buf;
    read_sqe->len       = (uint32_t)cap;
    read_sqe->off       = 0;
    read_sqe->user_data = read_ud;

    // Close of a direct descriptor is file_index, and fd must be 0.
    // IOSQE_FIXED_FILE is rejected with -EBADF. This CQE is the chain fence:
    // a successful close used to be skipped, which needed a following NOP.
    close_sqe->opcode     = IORING_OP_CLOSE;
    close_sqe->fd         = 0;
    close_sqe->file_index = ENG_SCRATCH_FD_SLOT + 1u;
    close_sqe->user_data  = close_ud;

    unsigned n = ring_submit(r);
    if (n > 0) {
        int rc = ring_enter(r, n, 0, -1);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}
