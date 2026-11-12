#ifndef ENGINE_RING_H
#define ENGINE_RING_H

#include <linux/io_uring.h>
#include <stddef.h>
#include <stdint.h>

// system_init and ring_context_init both require this set. A kernel that
// fails the probe never maps the arena.
static constexpr uint32_t k_ring_setup_mask =
    IORING_SETUP_SINGLE_ISSUER |
    IORING_SETUP_DEFER_TASKRUN |
    IORING_SETUP_TASKRUN_FLAG  |
    IORING_SETUP_NO_SQARRAY    |
    IORING_SETUP_CQSIZE        |
    IORING_SETUP_CLAMP         |
    IORING_SETUP_SUBMIT_ALL    |
    IORING_SETUP_NO_MMAP       |
    IORING_SETUP_REGISTERED_FD_ONLY |
    IORING_SETUP_R_DISABLED;

static constexpr uint32_t k_ring_required_features =
    IORING_FEAT_EXT_ARG |
    IORING_FEAT_CQE_SKIP |
    IORING_FEAT_LINKED_FILE |
    IORING_FEAT_REG_REG_RING |
    IORING_FEAT_MIN_TIMEOUT |
    IORING_FEAT_NO_IOWAIT;

#ifndef IOU_PBUF_RING_INC
#define IOU_PBUF_RING_INC 2u
#endif
#ifndef IORING_CQE_F_BUF_MORE
#define IORING_CQE_F_BUF_MORE (1U << 4)
#endif
#ifndef IORING_TIMEOUT_MULTISHOT
#define IORING_TIMEOUT_MULTISHOT (1U << 6)
#endif

enum : uint32_t {
    ENG_SQ_ENTRIES        = 64,
    ENG_CQ_ENTRIES        = 256,
    ENG_PBUF_ENTRIES      = 32,
    ENG_PBUF_LEN          = 8192,
    ENG_PBUF_BGID         = 1,
    ENG_PBUF_MIN_LEFT     = 512,
    ENG_FIXED_FILES       = 16,
    ENG_WL_SOCK_SLOT      = 0,
    ENG_RELEASE_FD_SLOT   = 1,
    ENG_SIGNAL_FD_SLOT    = 2,
    ENG_SCRATCH_FD_SLOT   = 3,
    ENG_INOTIFY_FD_SLOT   = 4,
    ENG_CLIPBOARD_FD_SLOT    = 5,
    ENG_PACK_FD_SLOT         = 6,
    ENG_CLIPBOARD_TX_FD_SLOT = 7,
    ENG_FILE_ALLOC_OFF       = 8,
    ENG_FILE_ALLOC_LEN       = 8,
};

// Slices of one arena slab, registered together for IORING_OP_READ_FIXED.
// On TrueUMA GPUs, pager staging slots 3..18 are released and slot 3 (ENG_FBUF_UMA_BDA)
// is reused to pin g_backend.bda_buf directly via IORING_REGISTER_BUFFERS_UPDATE.
enum : uint32_t {
    ENG_FBUF_INOTIFY     = 0,
    ENG_FBUF_SIGNAL      = 1,
    ENG_FBUF_RELEASE     = 2,
    ENG_FBUF_CORE_COUNT  = 3,
    ENG_FBUF_UMA_BDA     = 3,
    ENG_FBUF_PAGER_BASE  = 3,
    ENG_FBUF_PAGER_COUNT = 16,
    ENG_FBUF_COUNT       = ENG_FBUF_CORE_COUNT + ENG_FBUF_PAGER_COUNT,
    ENG_FBUF_INOTIFY_LEN = 4096,
    ENG_FBUF_SIGNAL_LEN  = 128,
    ENG_FBUF_RELEASE_LEN = 8,
    ENG_FBUF_PAGER_LEN   = 102400, // 100 KiB = 25 x 4 KiB (.epack v1 tile block)
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
    ENG_UD_FRAME_TIMER  = 16,
    ENG_UD_SIM_TIMER    = 17,
    ENG_UD_PAGER_CANCEL = 18,
    ENG_UD_SIM_CANCEL   = 19,
    ENG_UD_TIMER_UPDATE = 20,
    ENG_UD_CLIP_READ    = 21,
    ENG_UD_CLIP_WRITE   = 22,
    ENG_UD_CLIP_CLOSE   = 23,
    ENG_UD_CLIP_CANCEL  = 24,
    ENG_UD_PAGER_BASE   = 0x504147450000ULL,
    ENG_UD_PAGER_COUNT  = 16ULL,
};

static inline bool ring_ud_is_pager(uint64_t ud) {
    return ud >= ENG_UD_PAGER_BASE && ud < (ENG_UD_PAGER_BASE + ENG_UD_PAGER_COUNT);
}

// Generation-tagged 64-bit user_data helpers: [63:32]=gen, [31:16]=slot, [15:0]=tag.
// Unpacked tags with gen==0 and slot==0 remain bitwise-identical to plain ENG_UD_* constants.
static inline uint64_t ring_ud_pack(uint16_t tag, uint16_t slot, uint32_t gen) {
    return ((uint64_t)gen << 32) | ((uint64_t)slot << 16) | (uint64_t)tag;
}

static inline uint16_t ring_ud_tag(uint64_t ud) {
    return (uint16_t)(ud & 0xffffu);
}

static inline uint16_t ring_ud_slot(uint64_t ud) {
    return (uint16_t)((ud >> 16) & 0xffffu);
}

static inline uint32_t ring_ud_gen(uint64_t ud) {
    return (uint32_t)(ud >> 32);
}

typedef void (*RingCqeFn)(const struct io_uring_cqe *cqe, void *user);

typedef struct RingContext {
    int                       fd;
    char                      name[16];
    uint32_t                  wait_flags;
    void                     *arena;
    size_t                    arena_len;
    size_t                    sqes_off;
    size_t                    pbuf_off;
    size_t                    bufs_off;
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
    uint32_t                  pbuf_bid_off[ENG_PBUF_ENTRIES];
    uint16_t                  pbuf_tail;
    uint16_t                  pbuf_mask;
    bool                      pbuf_on;
    bool                      pbuf_inc_on;

    uint8_t                  *fixed_base;
    uint32_t                  fixed_at[ENG_FBUF_COUNT];
    uint32_t                  fixed_len[ENG_FBUF_COUNT];
    uint32_t                  fixed_count;
    uint32_t                  fixed_table_cap;
    bool                      fixed_on;
    bool                      pager_fixed_on;
    bool                      uma_buf_registered;
    void                     *uma_buf_base;
    size_t                    uma_buf_len;

    bool                      ring_registered;
    bool                      file_alloc_range_on;
    int                       enter_fd;
    bool                      wait_reg_on;
    uint32_t                  cq_overflow_total;
    uint32_t                  sq_dropped_total;
    bool                      overflow_warned;
    bool                      is_initialized;

    struct __kernel_timespec  timeout_update_ts;
    RingCqeFn                 default_cqe_fn;
    void                     *default_cqe_user;
} RingContext;

void ring_set_dispatcher(RingContext *r, RingCqeFn fn, void *user);
[[nodiscard]] size_t ring_fixed_region_bytes(bool include_pager);
void ring_release_pager_fixed_buffers(RingContext *r);
[[nodiscard]] int ring_register_uma_buffer(RingContext *r, void *base, size_t len);
void ring_unregister_uma_buffer(RingContext *r);

[[nodiscard]] uint64_t ring_mono_ns(void);
[[nodiscard]] int ring_context_init(RingContext *r, const char *name, bool with_pbuf);
void ring_context_shutdown(RingContext *r);
[[nodiscard]] struct io_uring_sqe *ring_sqe(RingContext *r);
[[nodiscard]] int ring_update_file(RingContext *r, uint32_t slot, int fd);
// One absolute CLOCK_MONOTONIC timeout. The deadline is carried in the SQE.
[[nodiscard]] int ring_arm_timeout_abs(RingContext *r, uint64_t deadline_ns, uint64_t user_data);
// Native kernel multishot relative timeout (period_ns via IORING_TIMEOUT_IMMEDIATE_ARG).
[[nodiscard]] int ring_arm_timeout_multishot(RingContext *r, uint64_t period_ns, uint64_t user_data);
// In-place deadline update for an armed absolute timeout.
[[nodiscard]] int ring_update_timeout_abs(RingContext *r, uint64_t deadline_ns,
                                          uint64_t target_ud, uint64_t update_ud);
// Remove an armed single-shot or multishot timeout by user_data.
[[nodiscard]] int ring_remove_timeout(RingContext *r, uint64_t target_ud,
                                      uint64_t remove_ud, bool skip_success);
// Read a registered arena slice. buf_index is an ENG_FBUF_* value.
[[nodiscard]] int ring_arm_read_fixed(RingContext *r, uint32_t slot, bool fixed_file, int raw_fd,
                                      uint32_t buf_index, uint64_t user_data);
[[nodiscard]] uint8_t *ring_fixed_ptr(const RingContext *r, uint32_t buf_index);
unsigned ring_submit(RingContext *r);
[[nodiscard]] int ring_enter(RingContext *r, unsigned submit, unsigned min_complete, int timeout_ms);
[[nodiscard]] bool ring_peek(RingContext *r, struct io_uring_cqe *out);
[[nodiscard]] uint8_t *ring_pbuf_chunk(const RingContext *r, uint16_t bid, uint32_t nbytes);
void ring_pbuf_finish(RingContext *r, uint16_t bid, uint32_t cqe_flags, int32_t cqe_res);
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
