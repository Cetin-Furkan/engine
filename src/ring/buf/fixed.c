#include "ring/internal.h"
#include <errno.h>
#include <sys/mman.h>
#include <sys/uio.h>

size_t ring_fixed_region_bytes(bool include_pager) {
    size_t core = (size_t)ENG_FBUF_INOTIFY_LEN + ENG_FBUF_SIGNAL_LEN + ENG_FBUF_RELEASE_LEN;
    if (!include_pager) {
        return core;
    }
    return ring_align_up(core, 4096u) + (size_t)ENG_FBUF_PAGER_COUNT * (size_t)ENG_FBUF_PAGER_LEN;
}

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
    at = (uint32_t)ring_align_up(at, 4096u);
    for (uint32_t i = 0; i < ENG_FBUF_PAGER_COUNT; ++i) {
        uint32_t slot = ENG_FBUF_PAGER_BASE + i;
        r->fixed_at[slot]  = at;
        r->fixed_len[slot] = ENG_FBUF_PAGER_LEN;
        at += ENG_FBUF_PAGER_LEN;
    }
}

void ring_release_pager_fixed_buffers(RingContext *r) {
    if (r == nullptr || !r->is_initialized || !r->fixed_on || !r->pager_fixed_on) {
        return;
    }
    long rc = ring_register(r, IORING_UNREGISTER_BUFFERS, 0, 0);
    if (rc < 0) {
        return;
    }
    r->pager_fixed_on     = false;
    r->uma_buf_registered = false;
    r->uma_buf_base       = nullptr;
    r->uma_buf_len        = 0u;
    r->fixed_on           = false;
    r->fixed_count        = 0;
    r->fixed_table_cap    = 0;
    if (ring_fixed_register(r) == 0 && r->fixed_base != nullptr) {
        size_t pager_bytes = (size_t)ENG_FBUF_PAGER_COUNT * (size_t)ENG_FBUF_PAGER_LEN;
        (void)madvise(r->fixed_base + r->fixed_at[ENG_FBUF_PAGER_BASE], pager_bytes, MADV_DONTNEED);
    }
}

uint8_t *ring_fixed_ptr(const RingContext *r, uint32_t buf_index) {
    if (r == nullptr || r->fixed_base == nullptr || buf_index >= r->fixed_count) {
        return nullptr;
    }
    return r->fixed_base + r->fixed_at[buf_index];
}

static long register_buffers_table(RingContext *r, const struct iovec *iov, uint32_t nr) {
    struct io_uring_rsrc_register reg = {
        .nr   = nr,
        .data = (uint64_t)(uintptr_t)iov,
    };
    long rc = ring_register(r, IORING_REGISTER_BUFFERS2, (long)&reg, (unsigned)sizeof reg);
    if (rc == -EINVAL || rc == -ENOSYS) {
        rc = ring_register(r, IORING_REGISTER_BUFFERS, (long)iov, nr);
    }
    return rc;
}

int ring_fixed_register(RingContext *r) {
    lay_out(r);
    struct iovec iov[ENG_FBUF_COUNT] = {0};
    uint32_t want    = r->pager_fixed_on ? ENG_FBUF_COUNT : ENG_FBUF_CORE_COUNT;
    uint32_t tbl_cap = r->pager_fixed_on ? ENG_FBUF_COUNT : (ENG_FBUF_CORE_COUNT + 1u);
    for (uint32_t i = 0; i < want; ++i) {
        iov[i].iov_base = r->fixed_base + r->fixed_at[i];
        iov[i].iov_len  = r->fixed_len[i];
    }
    long rc = register_buffers_table(r, iov, tbl_cap);
    if (rc < 0 && want > ENG_FBUF_CORE_COUNT) {
        // Graceful RLIMIT_MEMLOCK fallback: keep core buffers + sparse UMA slot registered.
        r->pager_fixed_on = false;
        want              = ENG_FBUF_CORE_COUNT;
        tbl_cap           = ENG_FBUF_CORE_COUNT + 1u;
        for (uint32_t i = want; i < ENG_FBUF_COUNT; ++i) {
            iov[i] = (struct iovec){0};
        }
        rc = register_buffers_table(r, iov, tbl_cap);
    }
    if (rc < 0) {
        r->pager_fixed_on  = false;
        r->fixed_count     = 0;
        r->fixed_table_cap = 0;
        return (int)rc;
    }
    r->fixed_count     = want;
    r->fixed_table_cap = tbl_cap;
    r->fixed_on        = true;
    return 0;
}

int ring_register_uma_buffer(RingContext *r, void *base, size_t len) {
    if (r == nullptr || !r->is_initialized || !r->fixed_on || r->pager_fixed_on ||
        r->fixed_table_cap <= ENG_FBUF_UMA_BDA || base == nullptr || len == 0u) {
        return -EINVAL;
    }
    struct iovec iov = {
        .iov_base = base,
        .iov_len  = len,
    };
    struct io_uring_rsrc_update2 up = {
        .offset = ENG_FBUF_UMA_BDA,
        .data   = (uint64_t)(uintptr_t)&iov,
        .nr     = 1u,
    };
    long rc = ring_register(r, IORING_REGISTER_BUFFERS_UPDATE, (long)&up, (unsigned)sizeof up);
    if (rc < 0) {
        r->uma_buf_registered = false;
        r->uma_buf_base       = nullptr;
        r->uma_buf_len        = 0u;
        return (int)rc;
    }
    r->uma_buf_registered = true;
    r->uma_buf_base       = base;
    r->uma_buf_len        = len;
    return 0;
}

void ring_unregister_uma_buffer(RingContext *r) {
    if (r == nullptr || !r->uma_buf_registered) {
        return;
    }
    if (r->ring_registered && r->enter_fd >= 0 && r->fixed_on) {
        struct iovec empty = {0};
        struct io_uring_rsrc_update2 up = {
            .offset = ENG_FBUF_UMA_BDA,
            .data   = (uint64_t)(uintptr_t)&empty,
            .nr     = 1u,
        };
        (void)ring_register(r, IORING_REGISTER_BUFFERS_UPDATE, (long)&up, (unsigned)sizeof up);
    }
    r->uma_buf_registered = false;
    r->uma_buf_base       = nullptr;
    r->uma_buf_len        = 0u;
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
