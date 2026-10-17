#ifndef ENGINE_RING_H
#define ENGINE_RING_H

#include <linux/io_uring.h>
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    ENG_SQ_ENTRIES   = 64,
    ENG_CQ_ENTRIES   = 256,
    ENG_PBUF_ENTRIES = 32,
    ENG_PBUF_LEN     = 8192,
    ENG_PBUF_BGID    = 1,
    ENG_FIXED_FILES  = 16,
    ENG_WL_SOCK_SLOT = 0,
};

enum : uint64_t {
    ENG_UD_WL_SOCKET  = 1,
    ENG_UD_WL_CONNECT = 2,
    ENG_UD_WL_SEND    = 3,
    ENG_UD_WL_RECV    = 4,
    ENG_UD_SYNCOBJ_FD  = 5,
    ENG_UD_MSG_RING    = 6,
    ENG_UD_WL_PONG     = 7,
    ENG_UD_GPU_RELEASE = 8,
    ENG_UD_SIGNAL      = 9,
};

typedef struct RingContext {
    int      fd;
    char     name[16];
    uint32_t wait_flags;

    void    *arena;
    size_t   arena_len;
    size_t   sqes_off;
    size_t   pbuf_off;
    size_t   bufs_off;
    size_t   wait_off;

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

    uint32_t sq_mask;
    uint32_t cq_mask;
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t sq_tail;
    uint32_t sq_pending;
    uint32_t cq_head;
    uint32_t features;

    struct io_uring_buf_ring *pbuf;
    uint8_t                  *bufs;
    uint16_t                  pbuf_tail;
    uint16_t                  pbuf_mask;
    bool                      pbuf_on;
    bool                      is_initialized;
} RingContext;

[[nodiscard]] uint64_t ring_mono_ns(void);

[[nodiscard]] int ring_context_init(RingContext *r, const char *name, bool with_pbuf);
void ring_context_shutdown(RingContext *r);

[[nodiscard]] struct io_uring_sqe *ring_sqe(RingContext *r);
[[nodiscard]] int ring_arm_poll(RingContext *r, int fd, uint32_t poll_mask, uint64_t user_data);
unsigned ring_submit(RingContext *r);
[[nodiscard]] int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms);
[[nodiscard]] bool ring_peek(RingContext *r, struct io_uring_cqe *out);

[[nodiscard]] uint8_t *ring_buf(const RingContext *r, uint16_t bid);
void ring_pbuf_recycle(RingContext *r, uint16_t bid);

[[nodiscard]] int ring_init(void);
void ring_shutdown(void);
[[nodiscard]] RingContext *ring_get_context(void);

#endif /* ENGINE_RING_H */