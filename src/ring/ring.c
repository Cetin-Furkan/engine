#include "ring/ring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static constexpr uint32_t k_ring_setup_mask =
    IORING_SETUP_SINGLE_ISSUER |
    IORING_SETUP_DEFER_TASKRUN |
    IORING_SETUP_TASKRUN_FLAG  |
    IORING_SETUP_NO_SQARRAY    |
    IORING_SETUP_CQSIZE        |
    IORING_SETUP_CLAMP         |
    IORING_SETUP_SUBMIT_ALL    |
    IORING_SETUP_NO_MMAP       |
    IORING_SETUP_R_DISABLED;

static RingContext s_primary_ring = { .fd = -1, .enter_fd = -1 };

uint64_t ring_mono_ns(void) {
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long sys6(long nr, long a, long b, long c, long d, long e, long f) {
    long rc = syscall(nr, a, b, c, d, e, f);
    return (rc < 0) ? -errno : rc;
}

static long ring_register(RingContext *r, unsigned opcode, long arg, unsigned nr) {
    int fd = r->fd;
    if (r->ring_registered) {
        fd = r->enter_fd;
        opcode |= IORING_REGISTER_USE_REGISTERED_RING;
    }
    return sys6(__NR_io_uring_register, fd, (long)opcode, arg, (long)nr, 0, 0);
}

static size_t query_page_size(void) {
    long p = sysconf(_SC_PAGESIZE);
    return (p > 0) ? (size_t)p : 4096u;
}

static size_t align_up(size_t n, size_t a) {
    return (n + a - 1u) & ~(a - 1u);
}

static int ring_enter_retry(RingContext *r, unsigned submit, unsigned min_complete,
                            unsigned flags, long arg, size_t argz) {
    int fd = r->fd;
    if (r->ring_registered) {
        fd = r->enter_fd;
        flags |= IORING_ENTER_REGISTERED_RING;
    }
    int again = 0;
    for (;;) {
        long rc = sys6(__NR_io_uring_enter, fd, submit, min_complete, flags, arg, (long)argz);
        if (rc >= 0) {
            return (int)rc;
        }
        if (rc == -EINTR) {
            continue;
        }
        if (rc == -EAGAIN && submit > 0 && again++ < 8) {
            continue;
        }
        return (int)rc;
    }
}

static void publish_pbufs(RingContext *r) {
    r->pbuf_mask = ENG_PBUF_ENTRIES - 1u;
    for (uint16_t i = 0; i < ENG_PBUF_ENTRIES; ++i) {
        struct io_uring_buf *b = &r->pbuf->bufs[i];
        b->addr = (uint64_t)(uintptr_t)(r->bufs + (size_t)i * ENG_PBUF_LEN);
        b->len  = ENG_PBUF_LEN;
        b->bid  = i;
        r->pbuf_used[i] = 0;
    }
    r->pbuf_tail = ENG_PBUF_ENTRIES;
    __atomic_store_n(&r->pbuf->tail, r->pbuf_tail, __ATOMIC_RELEASE);
}

static int register_pbuf(RingContext *r, bool inc) {
    struct io_uring_buf_reg breg = {
        .ring_addr    = (uint64_t)(uintptr_t)r->pbuf,
        .ring_entries = ENG_PBUF_ENTRIES,
        .bgid         = ENG_PBUF_BGID,
        .flags        = inc ? (uint16_t)IOU_PBUF_RING_INC : 0,
    };
    long rc = ring_register(r, IORING_REGISTER_PBUF_RING, (long)&breg, 1);
    return (rc < 0) ? (int)rc : 0;
}

int ring_context_init(RingContext *r, const char *name, bool with_pbuf) {
    if (r == nullptr) {
        return -EINVAL;
    }
    *r = (RingContext){ .fd = -1, .enter_fd = -1 };
    if (name != nullptr) {
        snprintf(r->name, sizeof r->name, "%s", name);
    }

    size_t page = query_page_size();
    size_t span = 64u * 1024u;
    if (span < page) {
        span = page;
    }

    r->sqes_off = span;
    size_t len = span * 2u;
    if (with_pbuf) {
        r->pbuf_off = len;
        r->bufs_off = r->pbuf_off + page;
        len = r->bufs_off + (size_t)ENG_PBUF_ENTRIES * ENG_PBUF_LEN;
    }
    r->wait_off = align_up(len, page);
    len = r->wait_off + page;

    void *arena = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) {
        fprintf(stderr, "[Ring Error] Arena mmap of %zu bytes failed: %s\n", len, strerror(errno));
        return -ENOMEM;
    }
    memset(arena, 0, len);

    r->arena     = arena;
    r->arena_len = len;
    r->rings     = arena;
    r->sqes_mem  = (char *)arena + r->sqes_off;
    if (with_pbuf) {
        r->pbuf = (struct io_uring_buf_ring *)((char *)arena + r->pbuf_off);
        r->bufs = (uint8_t *)arena + r->bufs_off;
    }
    r->wait = (struct io_uring_reg_wait *)((char *)arena + r->wait_off);

    struct io_uring_params p = {
        .flags      = k_ring_setup_mask,
        .cq_entries = ENG_CQ_ENTRIES,
        .cq_off     = { .user_addr = (uint64_t)(uintptr_t)r->rings },
        .sq_off     = { .user_addr = (uint64_t)(uintptr_t)r->sqes_mem },
    };

    long rc = sys6(__NR_io_uring_setup, ENG_SQ_ENTRIES, (long)&p, 0, 0, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] io_uring_setup failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    r->fd           = (int)rc;
    r->features     = p.features;
    r->sq_entries   = p.sq_entries;
    r->cq_entries   = p.cq_entries;
    r->sqes         = (struct io_uring_sqe *)r->sqes_mem;
    r->sq_khead     = (uint32_t *)((char *)r->rings + p.sq_off.head);
    r->sq_ktail     = (uint32_t *)((char *)r->rings + p.sq_off.tail);
    r->sq_kflags    = (uint32_t *)((char *)r->rings + p.sq_off.flags);
    r->sq_kdropped  = (uint32_t *)((char *)r->rings + p.sq_off.dropped);
    r->cq_khead     = (uint32_t *)((char *)r->rings + p.cq_off.head);
    r->cq_ktail     = (uint32_t *)((char *)r->rings + p.cq_off.tail);
    r->cq_koverflow = (uint32_t *)((char *)r->rings + p.cq_off.overflow);
    r->cqes         = (struct io_uring_cqe *)((char *)r->rings + p.cq_off.cqes);
    r->sq_mask      = *(uint32_t *)((char *)r->rings + p.sq_off.ring_mask);
    r->cq_mask      = *(uint32_t *)((char *)r->rings + p.cq_off.ring_mask);

    struct io_uring_clock_register clk = { .clockid = CLOCK_MONOTONIC };
    rc = ring_register(r, IORING_REGISTER_CLOCK, (long)&clk, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_CLOCK failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    struct io_uring_region_desc rd = {
        .user_addr = (uint64_t)(uintptr_t)r->wait,
        .size      = page,
        .flags     = IORING_MEM_REGION_TYPE_USER,
    };
    struct io_uring_mem_region_reg mreg = {
        .region_uptr = (uint64_t)(uintptr_t)&rd,
        .flags       = IORING_MEM_REGION_REG_WAIT_ARG,
    };
    rc = ring_register(r, IORING_REGISTER_MEM_REGION, (long)&mreg, 1);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_MEM_REGION failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }
    r->wait_reg_on = true;

    if (with_pbuf) {
        publish_pbufs(r);
        int prc = register_pbuf(r, true);
        r->pbuf_inc = prc == 0;
        if (prc < 0) {
            fprintf(stderr,
                    "[Ring] IOU_PBUF_RING_INC was rejected (%s). Provided buffers stay one message per slot.\n",
                    strerror(-prc));
            prc = register_pbuf(r, false);
            r->pbuf_inc = false;
        }
        if (prc < 0) {
            fprintf(stderr, "[Ring Error] IORING_REGISTER_PBUF_RING failed: %s\n", strerror(-prc));
            ring_context_shutdown(r);
            return prc;
        }
        r->pbuf_on = true;
    }

    rc = ring_register(r, IORING_REGISTER_ENABLE_RINGS, 0, 0);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_ENABLE_RINGS failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    int sparse_files[ENG_FIXED_FILES];
    for (uint32_t i = 0; i < ENG_FIXED_FILES; ++i) {
        sparse_files[i] = -1;
    }
    rc = ring_register(r, IORING_REGISTER_FILES, (long)sparse_files, ENG_FIXED_FILES);
    if (rc < 0) {
        fprintf(stderr, "[Ring Error] IORING_REGISTER_FILES failed: %s\n", strerror((int)-rc));
        ring_context_shutdown(r);
        return (int)rc;
    }

    struct io_uring_rsrc_update ring_fd = {
        .offset = ~0u,
        .data   = (uint64_t)r->fd,
    };
    rc = ring_register(r, IORING_REGISTER_RING_FDS, (long)&ring_fd, 1);
    if (rc < 0) {
        fprintf(stderr,
                "[Ring] IORING_REGISTER_RING_FDS failed (%s). io_uring_enter keeps the process fd.\n",
                strerror((int)-rc));
        r->enter_fd = r->fd;
        r->ring_registered = false;
    } else {
        r->enter_fd = (int)ring_fd.offset;
        r->ring_registered = true;
        if (close(r->fd) == 0) {
            r->fd = -1;
        }
    }

    if ((r->features & IORING_FEAT_NO_IOWAIT) != 0) {
        r->wait_flags = IORING_ENTER_NO_IOWAIT;
    }

    r->is_initialized = true;
    printf("[Ring] Initialized '%s' (%s, SQ=%u, CQ=%u, PBUF=%ux%uB%s, FixedFiles=%u, NO_MMAP+SINGLE_ISSUER+DEFER_TASKRUN+REG_WAIT)\n",
           r->name,
           r->ring_registered ? "registered ring fd" : "process fd",
           r->sq_entries,
           r->cq_entries,
           with_pbuf ? ENG_PBUF_ENTRIES : 0u,
           ENG_PBUF_LEN,
           r->pbuf_inc ? " incremental" : "",
           ENG_FIXED_FILES);
    return 0;
}

void ring_context_shutdown(RingContext *r) {
    if (r == nullptr) {
        return;
    }
    if (r->ring_registered) {
        struct io_uring_rsrc_update up = {
            .offset = (uint32_t)r->enter_fd,
        };
        (void)ring_register(r, IORING_UNREGISTER_RING_FDS, (long)&up, 1);
        r->ring_registered = false;
        r->enter_fd = -1;
    }
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
    if (r->arena != nullptr && r->arena_len > 0) {
        munmap(r->arena, r->arena_len);
    }
    *r = (RingContext){ .fd = -1, .enter_fd = -1 };
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

int ring_arm_poll(RingContext *r, int fd, uint32_t poll_mask, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized || fd < 0) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode        = IORING_OP_POLL_ADD;
    sqe->fd            = fd;
    sqe->poll32_events = poll_mask;
    sqe->user_data     = user_data;
    return 0;
}

int ring_arm_poll_fixed(RingContext *r, uint32_t slot, uint32_t poll_mask, uint64_t user_data) {
    if (r == nullptr || !r->is_initialized || slot >= ENG_FIXED_FILES) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(r);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode        = IORING_OP_POLL_ADD;
    sqe->flags         = IOSQE_FIXED_FILE;
    sqe->fd            = (int32_t)slot;
    sqe->poll32_events = poll_mask;
    sqe->user_data     = user_data;
    return 0;
}

void ring_sqe_unget(RingContext *r, uint32_t n) {
    if (r == nullptr) {
        return;
    }
    if (n > r->sq_pending) {
        r->sq_pending = 0;
        return;
    }
    r->sq_pending -= n;
}

struct io_uring_sqe *ring_sqe(RingContext *r) {
    uint32_t head = __atomic_load_n(r->sq_khead, __ATOMIC_ACQUIRE);
    uint32_t next = r->sq_tail + r->sq_pending;
    if (next - head >= r->sq_entries) {
        return nullptr;
    }
    struct io_uring_sqe *sqe = &r->sqes[next & r->sq_mask];
    memset(sqe, 0, sizeof(*sqe));
    r->sq_pending++;
    return sqe;
}

unsigned ring_submit(RingContext *r) {
    unsigned n = r->sq_pending;
    if (n == 0) {
        return 0;
    }
    uint32_t tail = r->sq_tail + n;
    __atomic_store_n(r->sq_ktail, tail, __ATOMIC_RELEASE);
    r->sq_tail    = tail;
    r->sq_pending = 0;
    return n;
}

int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms) {
    unsigned flags = 0;
    void *arg      = nullptr;
    size_t argz    = 0;
    struct __kernel_timespec fallback_ts = {0};
    struct io_uring_getevents_arg fallback_garg = {0};

    if (min_complete > 0) {
        flags |= IORING_ENTER_GETEVENTS;
    }
    uint32_t sq_flags = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
    if ((sq_flags & IORING_SQ_TASKRUN) != 0) {
        flags |= IORING_ENTER_GETEVENTS;
    }
    if (timeout_ms >= 0 && r->wait_reg_on && r->wait != nullptr) {
        r->wait[0] = (struct io_uring_reg_wait){
            .ts = {
                .tv_sec  = timeout_ms / 1000,
                .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
            },
            .min_wait_usec = 0,
            .flags         = IORING_REG_WAIT_TS,
            .sigmask       = 0,
            .sigmask_sz    = 0,
        };
        __atomic_thread_fence(__ATOMIC_RELEASE);
        flags |= IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG | IORING_ENTER_EXT_ARG_REG;
        arg    = (void *)(uintptr_t)0u;
        argz   = sizeof(struct io_uring_reg_wait);
    } else if (timeout_ms >= 0) {
        fallback_ts.tv_sec  = timeout_ms / 1000;
        fallback_ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        fallback_garg.ts    = (uint64_t)(uintptr_t)&fallback_ts;
        flags              |= IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
        arg                 = &fallback_garg;
        argz                = sizeof(fallback_garg);
    }
    if ((flags & IORING_ENTER_GETEVENTS) != 0) {
        flags |= r->wait_flags;
    }
    return ring_enter_retry(r, submit, min_complete, flags, (long)arg, argz);
}

bool ring_peek(RingContext *r, struct io_uring_cqe *out) {
    uint32_t tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
    uint32_t head = r->cq_head;
    if (head == tail) {
        uint32_t sq_flags = __atomic_load_n(r->sq_kflags, __ATOMIC_ACQUIRE);
        if ((sq_flags & IORING_SQ_TASKRUN) != 0) {
            (void)ring_enter_retry(r, 0, 0, IORING_ENTER_GETEVENTS | r->wait_flags, 0, 0);
            tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
        }
    }
    if (head == tail) {
        return false;
    }
    *out = r->cqes[head & r->cq_mask];
    head++;
    __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE);
    r->cq_head = head;
    return true;
}

uint8_t *ring_buf(const RingContext *r, uint16_t bid) {
    if (bid >= ENG_PBUF_ENTRIES) {
        return nullptr;
    }
    return r->bufs + (size_t)bid * ENG_PBUF_LEN;
}

void ring_pbuf_recycle(RingContext *r, uint16_t bid) {
    uint16_t slot = r->pbuf_tail & r->pbuf_mask;
    struct io_uring_buf *b = &r->pbuf->bufs[slot];
    b->addr = (uint64_t)(uintptr_t)ring_buf(r, bid);
    b->len  = ENG_PBUF_LEN;
    b->bid  = bid;
    r->pbuf_tail++;
    __atomic_store_n(&r->pbuf->tail, r->pbuf_tail, __ATOMIC_RELEASE);
}

uint8_t *ring_pbuf_chunk(const RingContext *r, uint16_t bid, uint32_t nbytes) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES || r->bufs == nullptr) {
        return nullptr;
    }
    uint32_t used = r->pbuf_inc ? r->pbuf_used[bid] : 0u;
    if (nbytes > ENG_PBUF_LEN || used > ENG_PBUF_LEN - nbytes) {
        return nullptr;
    }
    return r->bufs + (size_t)bid * ENG_PBUF_LEN + used;
}

void ring_pbuf_finish(RingContext *r, uint16_t bid, uint32_t nbytes, bool more) {
    if (r == nullptr || bid >= ENG_PBUF_ENTRIES) {
        return;
    }
    if (r->pbuf_inc && more) {
        uint32_t used = r->pbuf_used[bid];
        if (nbytes > ENG_PBUF_LEN - used) {
            nbytes = ENG_PBUF_LEN - used;
        }
        r->pbuf_used[bid] = used + nbytes;
        return;
    }
    r->pbuf_used[bid] = 0;
    ring_pbuf_recycle(r, bid);
}

int ring_pbuf_drop_inc(RingContext *r) {
    if (r == nullptr || !r->is_initialized || !r->pbuf_on) {
        return -EINVAL;
    }
    if (!r->pbuf_inc) {
        return 0;
    }
    struct io_uring_buf_reg unreg = { .bgid = ENG_PBUF_BGID };
    long rc = ring_register(r, IORING_UNREGISTER_PBUF_RING, (long)&unreg, 1);
    if (rc < 0) {
        return (int)rc;
    }
    r->pbuf_inc = false;
    publish_pbufs(r);
    int prc = register_pbuf(r, false);
    if (prc < 0) {
        r->pbuf_on = false;
        return prc;
    }
    return 0;
}

static int cancel_file_chain(RingContext *r) {
    static const uint64_t k_file_ops[4] = {
        ENG_UD_FILE_OPEN,
        ENG_UD_FILE_READ,
        ENG_UD_FILE_CLOSE,
        ENG_UD_FILE_FENCE,
    };
    uint32_t held = r->sq_pending;
    for (uint32_t i = 0; i < 4u; ++i) {
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

    uint32_t held = r->sq_pending;
    struct io_uring_sqe *open_sqe  = ring_sqe(r);
    struct io_uring_sqe *read_sqe  = ring_sqe(r);
    struct io_uring_sqe *close_sqe = ring_sqe(r);
    struct io_uring_sqe *fence_sqe = ring_sqe(r);
    if (open_sqe == nullptr || read_sqe == nullptr || close_sqe == nullptr || fence_sqe == nullptr) {
        r->sq_pending = held;
        return -ENOSPC;
    }

    open_sqe->opcode     = IORING_OP_OPENAT;
    open_sqe->flags      = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
    open_sqe->fd         = AT_FDCWD;
    open_sqe->addr       = (uint64_t)(uintptr_t)path;
    open_sqe->len        = 0;
    /* A direct descriptor is not a process fd. O_CLOEXEC is rejected. */
    open_sqe->open_flags = O_RDONLY;
    open_sqe->file_index = ENG_SCRATCH_FD_SLOT + 1u;
    open_sqe->user_data  = ENG_UD_FILE_OPEN;

    read_sqe->opcode    = IORING_OP_READ;
    read_sqe->flags     = IOSQE_FIXED_FILE | IOSQE_IO_LINK;
    read_sqe->fd        = ENG_SCRATCH_FD_SLOT;
    read_sqe->addr      = (uint64_t)(uintptr_t)buf;
    read_sqe->len       = (uint32_t)cap;
    read_sqe->off       = 0;
    read_sqe->user_data = ENG_UD_FILE_READ;

    close_sqe->opcode     = IORING_OP_CLOSE;
    close_sqe->flags      = IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
    close_sqe->fd         = 0;
    close_sqe->file_index = ENG_SCRATCH_FD_SLOT + 1u;
    close_sqe->user_data  = ENG_UD_FILE_CLOSE;

    fence_sqe->opcode    = IORING_OP_NOP;
    fence_sqe->fd        = -1;
    fence_sqe->user_data = ENG_UD_FILE_FENCE;

    bool got_read = false;
    bool got_close = false;
    bool got_fence = false;
    bool open_failed = false;
    bool cancel_sent = false;
    int32_t read_res = -EIO;
    int32_t root = 0;
    int pending_error = 0;
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(r, &cqe)) {
            if (cqe.user_data == ENG_UD_FILE_OPEN) {
                if (cqe.res != 0) {
                    open_failed = true;
                    if (root == 0) {
                        root = cqe.res;
                    }
                }
            } else if (cqe.user_data == ENG_UD_FILE_READ) {
                got_read = true;
                read_res = cqe.res;
            } else if (cqe.user_data == ENG_UD_FILE_CLOSE) {
                got_close = true;
                if (cqe.res < 0 && cqe.res != -ECANCELED && root == 0) {
                    root = cqe.res;
                }
            } else if (cqe.user_data == ENG_UD_FILE_FENCE) {
                got_fence = true;
                if (cqe.res < 0 && cqe.res != -ECANCELED && root == 0) {
                    root = cqe.res;
                }
            } else if (cqe.user_data == ENG_UD_FILE_CANCEL) {
                /* Cancel completions stay here. path and buf belong to the caller. */
            } else if (on_cqe != nullptr) {
                on_cqe(&cqe, on_user);
            }
        }
        /* CLOSE skips its success CQE. An OPEN error posts -ECANCELED for the rest. */
        if (got_fence && got_read && (!open_failed || got_close)) {
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

    if (!got_fence) {
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

int ring_init(void) {
    return (ring_context_init(&s_primary_ring, "primary", true) == 0) ? 0 : 1;
}

void ring_shutdown(void) {
    if (s_primary_ring.is_initialized) {
        ring_context_shutdown(&s_primary_ring);
        printf("goodbye from ring\n");
    }
}

RingContext *ring_get_context(void) {
    return &s_primary_ring;
}