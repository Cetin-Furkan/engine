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
    // Timerfd reads. A multishot read pops one buffer per completion, so this
    // group is separate from the Wayland recv group.
    ENG_SIM_PBUF_ENTRIES = 16,
    ENG_SIM_PBUF_LEN     = 8,
    ENG_SIM_BGID         = 2,
    ENG_FIXED_FILES      = 16,
    ENG_WL_SOCK_SLOT     = 0,
    ENG_RELEASE_FD_SLOT  = 1,
    ENG_SIGNAL_FD_SLOT   = 2,
    ENG_SCRATCH_FD_SLOT  = 3,
    ENG_INOTIFY_FD_SLOT  = 4,
    ENG_SIM_FD_SLOT      = 5,
};

// Slices of one arena slab, registered together for IORING_OP_READ_FIXED.
enum : uint32_t {
    ENG_FBUF_INOTIFY     = 0,
    ENG_FBUF_SIGNAL      = 1,
    ENG_FBUF_RELEASE     = 2,
    ENG_FBUF_COUNT       = 3,
    ENG_FBUF_INOTIFY_LEN = 4096,
    ENG_FBUF_SIGNAL_LEN  = 128,
    ENG_FBUF_RELEASE_LEN = 8,
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
    // The file chain no longer posts a NOP. Close is the last completion.
    // The value stays so the tags above it do not move.
    ENG_UD_FILE_FENCE  = 14,
    ENG_UD_FILE_CANCEL = 15,
    ENG_UD_FRAME_TIMER = 16,
    ENG_UD_SIM_TIMER   = 17,
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
    size_t                    sim_pbuf_off;
    size_t                    sim_bufs_off;
    size_t                    fixed_off;
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

    struct io_uring_buf_ring *sim_pbuf;
    uint8_t                  *sim_bufs;
    uint16_t                  sim_pbuf_tail;
    uint16_t                  sim_pbuf_mask;
    bool                      sim_pbuf_on;

    uint8_t                  *fixed_base;
    uint32_t                  fixed_at[ENG_FBUF_COUNT];
    uint32_t                  fixed_len[ENG_FBUF_COUNT];
    bool                      fixed_on;

    bool                      ring_registered;
    int                       enter_fd;
    bool                      wait_reg_on;
    bool                      min_timeout_on;
    uint32_t                  cq_overflow_total;
    uint32_t                  sq_dropped_total;
    bool                      overflow_warned;
    bool                      is_initialized;
} RingContext;

typedef void (*RingCqeFn)(const struct io_uring_cqe *cqe, void *user);

[[nodiscard]] uint64_t ring_mono_ns(void);
[[nodiscard]] int ring_context_init(RingContext *r, const char *name, bool with_pbuf);
void ring_context_shutdown(RingContext *r);
[[nodiscard]] struct io_uring_sqe *ring_sqe(RingContext *r);
[[nodiscard]] int ring_update_file(RingContext *r, uint32_t slot, int fd);
// One absolute CLOCK_MONOTONIC timeout. The deadline is carried in the SQE.
[[nodiscard]] int ring_arm_timeout_abs(RingContext *r, uint64_t deadline_ns, uint64_t user_data);
// Read a registered arena slice. buf_index is an ENG_FBUF_* value.
[[nodiscard]] int ring_arm_read_fixed(RingContext *r, uint32_t slot, bool fixed_file, int raw_fd,
                                      uint32_t buf_index, uint64_t user_data);
// One multishot read that pops a buffer from bgid on every completion.
[[nodiscard]] int ring_arm_read_mshot(RingContext *r, uint32_t slot, bool fixed_file, int raw_fd,
                                      uint16_t bgid, uint64_t user_data);
[[nodiscard]] uint8_t *ring_fixed_ptr(const RingContext *r, uint32_t buf_index);
// Copy the timerfd count out of the selected sim buffer and recycle it.
[[nodiscard]] bool ring_take_sim_count(RingContext *r, const struct io_uring_cqe *cqe, uint64_t *out);
unsigned ring_submit(RingContext *r);
[[nodiscard]] int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms);
[[nodiscard]] bool ring_peek(RingContext *r, struct io_uring_cqe *out);
[[nodiscard]] uint8_t *ring_buf(const RingContext *r, uint16_t bid);
[[nodiscard]] uint8_t *ring_pbuf_chunk(const RingContext *r, uint16_t bid, uint32_t nbytes);
void ring_pbuf_finish(RingContext *r, uint16_t bid);
void ring_pbuf_recycle(RingContext *r, uint16_t bid);
void ring_sqe_unget(RingContext *r, uint32_t n);
[[nodiscard]] int ring_submit_read_file(RingContext *r, const char *path, void *buf, size_t cap,
                                        uint64_t open_ud, uint64_t read_ud, uint64_t close_ud);
[[nodiscard]] int ring_read_file_all(RingContext *r, const char *path, void *buf, size_t cap,
                                     size_t *out_len, RingCqeFn on_cqe, void *on_user);
[[nodiscard]] int ring_init(void);
void ring_shutdown(void);
[[nodiscard]] RingContext *ring_get_context(void);

#endif /* ENGINE_RING_H */
