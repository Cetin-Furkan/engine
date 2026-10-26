#ifndef ENGINE_RING_H
#define ENGINE_RING_H

#include <linux/io_uring.h>
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    ENG_SQ_ENTRIES       = 64,
    ENG_CQ_ENTRIES       = 256,
    ENG_PBUF_ENTRIES     = 32,
    ENG_PBUF_LEN         = 8192,
    ENG_PBUF_BGID        = 1,
    ENG_FIXED_FILES      = 16,
    ENG_WL_SOCK_SLOT     = 0,
    ENG_RELEASE_FD_SLOT  = 1,
    ENG_SIGNAL_FD_SLOT   = 2,
    ENG_SCRATCH_FD_SLOT  = 3,
    ENG_INOTIFY_FD_SLOT  = 4,
};

enum : uint64_t {
    ENG_UD_WL_SOCKET   = 1,
    ENG_UD_WL_CONNECT  = 2,
    ENG_UD_WL_SEND     = 3,
    ENG_UD_WL_RECV     = 4,
    ENG_UD_WL_PONG     = 7,
    ENG_UD_GPU_RELEASE = 8,
    ENG_UD_SIGNAL      = 9,
    ENG_UD_FILE_OPEN   = 10,
    ENG_UD_FILE_READ   = 11,
    ENG_UD_FILE_CLOSE  = 12,
    ENG_UD_INOTIFY     = 13,
    ENG_UD_FILE_FENCE  = 14,
    ENG_UD_FILE_CANCEL = 15,
};

typedef struct RingContext {
    int                       fd;
    char                      name[16];
    uint32_t                  wait_flags;
    void                     *arena;
    size_t                    arena_len;
    size_t                    sqes_off;
    size_t                    pbuf_off;
    size_t                    bufs_off;
    size_t                    wait_off;

    void                     *rings;
    void                     *sqes_mem;
    struct io_uring_reg_wait *wait;

    struct io_uring_sqe      *sqes;
    struct io_uring_cqe      *cqes;
    uint32_t                 *sq_khead;
    uint32_t                 *sq_ktail;
    uint32_t                 *sq_kflags;
    uint32_t                 *sq_kdropped;
    uint32_t                 *cq_khead;
    uint32_t                 *cq_ktail;
    uint32_t                 *cq_koverflow;

    uint32_t                  sq_mask;
    uint32_t                  cq_mask;
    uint32_t                  sq_entries;
    uint32_t                  cq_entries;
    uint32_t                  sq_tail;
    uint32_t                  sq_pending;
    uint32_t                  cq_head;
    uint32_t                  features;

    struct io_uring_buf_ring *pbuf;
    uint8_t                  *bufs;
    uint16_t                  pbuf_tail;
    uint16_t                  pbuf_mask;
    bool                      pbuf_on;
    bool                      pbuf_inc;
    bool                      ring_registered;
    int                       enter_fd;
    uint32_t                  pbuf_used[ENG_PBUF_ENTRIES];
    bool                      wait_reg_on;
    bool                      is_initialized;
} RingContext;

typedef void (*RingCqeFn)(const struct io_uring_cqe *cqe, void *user);

[[nodiscard]] uint64_t ring_mono_ns(void);
[[nodiscard]] int ring_context_init(RingContext *r, const char *name, bool with_pbuf);
void ring_context_shutdown(RingContext *r);
[[nodiscard]] struct io_uring_sqe *ring_sqe(RingContext *r);
[[nodiscard]] int ring_update_file(RingContext *r, uint32_t slot, int fd);
[[nodiscard]] int ring_arm_poll(RingContext *r, int fd, uint32_t poll_mask, uint64_t user_data);
[[nodiscard]] int ring_arm_poll_fixed(RingContext *r, uint32_t slot, uint32_t poll_mask, uint64_t user_data);
unsigned ring_submit(RingContext *r);
[[nodiscard]] int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms);
[[nodiscard]] bool ring_peek(RingContext *r, struct io_uring_cqe *out);
[[nodiscard]] uint8_t *ring_buf(const RingContext *r, uint16_t bid);
[[nodiscard]] uint8_t *ring_pbuf_chunk(const RingContext *r, uint16_t bid, uint32_t nbytes);
void ring_pbuf_finish(RingContext *r, uint16_t bid, uint32_t nbytes, bool more);
[[nodiscard]] int ring_pbuf_drop_inc(RingContext *r);
void ring_pbuf_recycle(RingContext *r, uint16_t bid);
void ring_sqe_unget(RingContext *r, uint32_t n);
[[nodiscard]] int ring_read_file_all(RingContext *r, const char *path, void *buf, size_t cap,
                                     size_t *out_len, RingCqeFn on_cqe, void *on_user);
[[nodiscard]] int ring_init(void);
void ring_shutdown(void);
[[nodiscard]] RingContext *ring_get_context(void);

#endif /* ENGINE_RING_H */