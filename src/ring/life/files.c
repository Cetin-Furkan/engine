#include "ring/internal.h"
#include <errno.h>

int ring_register_files(RingContext *r) {
    struct io_uring_rsrc_register reg = {
        .nr    = ENG_FIXED_FILES,
        .flags = IORING_RSRC_REGISTER_SPARSE,
    };
    long rc = ring_register(r, IORING_REGISTER_FILES2, (long)&reg, (unsigned)sizeof reg);
    return (rc < 0) ? (int)rc : 0;
}

int ring_update_file(RingContext *r, uint32_t slot, int fd) {
    if (r == nullptr || !r->is_initialized || slot >= ENG_FIXED_FILES) {
        return -EINVAL;
    }
    struct io_uring_files_update up = {
        .offset = slot,
        .resv   = 0,
        .fds    = (uint64_t)(uintptr_t)&fd,
    };
    long rc = ring_register(r, IORING_REGISTER_FILES_UPDATE, (long)&up, 1);
    return (rc < 0) ? (int)rc : 0;
}
