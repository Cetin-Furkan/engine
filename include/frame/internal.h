#ifndef ENGINE_FRAME_INTERNAL_H
#define ENGINE_FRAME_INTERNAL_H

#include "engine/app.h"
#include "frame/frame.h"
#include "frame/clock.h"
#include "frame/handle.h"
#include "chrome/gpu.h"
#include "chrome/layer.h"
#include "chrome/spec.h"
#include "input/pointer.h"
#include "ring/ring.h"
#include "wayland/wayland.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include "vulkan/bda.h"
#include <drm/drm.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <stddef.h>
#include <unistd.h>

// Present process shared by boot/, clock/, loop/, sim/, and present/.
//
// The first 64 bytes of FrameState are the pace the idle loop and turn_step
// read on every wake. FrameClock is the next line. DMA-BUF images and the
// orphan table sit after that, so a flag load does not pull a VkImage into
// the same line. signalfd, inotify, and the release counter live in the
// ring arena and are read with frame_signal_number and ring_fixed_ptr.
//
// The first 64 bytes of a FrameSlot are the release gate spare_ready polls.
// The exported image starts on the next line.

enum : uint64_t {
    k_ud_signal_cancel  = UINT64_MAX,
    k_ud_release_cancel = UINT64_MAX - 1u,
    k_ud_inotify_cancel = UINT64_MAX - 2u,
};

enum : uint32_t {
    k_orphan_init_cap = 8u,
    k_orphan_max_cap  = 64u,
    k_gpu_timer_slots = 12u, // Four gfx timestamps + two async PRIME transfer timestamps per scene slot.
};

enum : int {
    k_turn_arm  = 0,
    k_turn_run  = 1,
    k_turn_done = 2,
};

enum : uint32_t {
    k_inotify_mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                     IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF,
};

// One presented image. The DRM node and the shared acquire timeline are not
// in here. Bytes 0..63 are the gate the turn polls. The image begins at 64.
typedef struct FrameSlot {
    uint64_t              acquire_point;
    uint64_t              release_point;
    uint32_t              wl_buffer_id;
    uint32_t              release_timeline_id;
    // Bumped every time the slot is blanked or reaped. A SlotRef taken before
    // the bump no longer resolves, so a stale async release can never be
    // mistaken for the slot's new image.
    uint32_t              generation;
    uint32_t              release_handle;
    int                   release_eventfd;
    bool                  release_pending;
    bool                  foreign_owned;
    bool                  ts_armed;      // Graphics timestamp queries were written in the last submit.
    bool                  ts_xfer_armed; // Async PRIME transfer timestamps were written in the last submit.
    uint8_t               ts_base;       // First graphics query index in the shared pool.
    uint8_t               ts_xfer_base;  // First transfer query index in the shared pool.
    alignas(64) VulkanExportableImage image;
    VkCommandBuffer       cmd_buf;
    VkImageView           image_view;
    VkFormat              view_format;
    EngineTargets         targets;
    // Device-local color target used when image.prime_copy is set. The export
    // image is then only a transfer destination for the display GPU.
    VkImage               prime_image;
    VkDeviceMemory        prime_memory;
    VkImageView           prime_view;
    VkCommandBuffer       transfer_cmd_buf;
    FrameRect             surface_damage;
    FrameRect             copy_damage;
    FrameRect             stale_rect;
    bool                  has_valid_history;
} FrameSlot;

typedef EngineRelay RunRelay;

typedef struct FrameState {
    // Line 0. turn_step and the idle arm read these on every wake.
    int32_t  turn_phase;
    int32_t  front;
    uint32_t pending_heat;
    uint32_t bar_px;
    uint64_t sync_point;
    uint64_t pending_release;
    uint64_t last_stamp_mono_ns;
    uint64_t mono0_ns;
    bool     spare_live;
    bool     turn_swap_pending;
    bool     scene_dirty;
    bool     sim_hot;
    bool     sim_latched;
    bool     sim_timer_on;
    bool     sim_mshot_armed;
    bool     timer_armed;
    bool     stall_reported;
    bool     loop_was_busy;
    bool     resize_pending;
    bool     chrome_dirty;
    bool     presented;
    bool     bar_dirty;
    bool     bar_live;
    bool     bar_swap_on_send;

    // Line 1. The compositor period and the simulation cursor.
    alignas(64) FrameClock clock;

    // Cold bookkeeping. Images follow, each slot on its own lines.
    int32_t  bar_front;
    uint32_t window_w;
    uint32_t window_h;
    uint32_t pending_sim_stamp; // Wall ms of the tick whose heat is still uncommitted.
    uint32_t resize_serial;
    uint32_t resize_old_buffer;
    uint32_t resize_old_w;
    uint32_t resize_old_h;
    uint64_t resize_release;
    int      drm_fd;
    uint32_t acquire_handle;
    VkSemaphore acquire;
    VkSemaphore acquire_sync;
    VkQueryPool ts_pool;
    float    ts_period_ns;
    uint32_t ts_valid_bits;
    uint32_t ts_xfer_valid_bits;
    uint16_t ts_reset_mask;
    bool     ts_host_reset;
    bool     sim_cancel_sent;
    bool     same_repaint;
    bool     release_fixed;
    bool     inotify_fixed;
    bool     inotify_stop;
    bool     inotify_warned;
    bool     world_dirty;
    bool     bench_uncapped;
    uint32_t sim_gen;
    uint32_t sim_inflight_count;
    uint32_t sim_cancel_inflight;
    uint64_t sim_active_ud;
    uint64_t timer_deadline_ns;
    uint64_t last_gpu_comp_ns;
    uint64_t last_gpu_rend_ns;
    uint64_t last_gpu_xfer_ns;
    uint64_t last_gpu_tot_ns;
    uint64_t gpu_samples_retired;
    uint64_t gpu_submits;
    uint32_t turn_frames;
    VkSemaphore prime_ready_sem;
    VkSemaphore prime_acquire_sync;
    uint64_t prime_ready_point;
    uint64_t last_acquire_signal_point;
    VkQueue  last_acquire_signal_queue;
    bool     timeline_fd_to_handle_unsupported;
    bool     implicit_dmabuf_sync;
    bool     pending_damage_set;
    bool     pending_damage_full;
    FrameRect pending_damage;
    // One eventfd for the life of the process. DRM_IOCTL_SYNCOBJ_EVENTFD is
    // issued only while shutting down, for the point still on screen. The turn
    // does not sleep on it. Slots keep release_eventfd at -1 so a reap cannot
    // close this fd.
    int      release_fd;
    int      stuck_release_fd;
    int      inotify_fd;
    int      inotify_wd;
    int      world_wd;
    uint64_t orphan_mask;
    uint32_t orphan_count;
    uint32_t orphan_cap;
    uint32_t orphan_gen[k_orphan_max_cap];
    FrameSlot *orphans;
    RunRelay *relay;
    char     resize_tranche[16];
    char     chrome_watch_base[128];
    char     world_watch_path[256];
    char     world_watch_dir[256];
    char     world_watch_base[128];

    // slot[front] is on screen. The other slot is the spare while the turn runs.
    FrameSlot slot[2];
    FrameSlot bar_slot[2];
    // A resize commit is queued like a turn commit. The old front is retired
    // when ENG_UD_WL_SEND accepts the bytes, not when a wl_callback.done arrives.
    FrameSlot resize_fresh;
    // Buffers whose compositor release has not signalled. Indexed by fixed
    // bits in orphan_mask so a SlotRef does not shift when a neighbour is swept.
    // Starts inline at 8 slots and dynamically grows up to 64 slots on demand.
    alignas(64) FrameSlot orphans_inline[k_orphan_init_cap];
} FrameState;

static_assert(offsetof(FrameState, clock) == 64, "pace is one cache line");
static_assert(offsetof(FrameState, bar_front) == 128, "clock is the next cache line");
static_assert(sizeof(struct signalfd_siginfo) == ENG_FBUF_SIGNAL_LEN, "signalfd record is one fixed slice");

static inline int frame_signal_number(int32_t res) {
    struct signalfd_siginfo *info =
        (struct signalfd_siginfo *)ring_fixed_ptr(ring_get_context(), ENG_FBUF_SIGNAL);
    if (res != (int32_t)ENG_FBUF_SIGNAL_LEN || info == nullptr) {
        return 0;
    }
    return (int)info->ssi_signo;
}
static_assert(offsetof(FrameSlot, release_pending) < 64, "release gate stays on line 0");
static_assert(offsetof(FrameSlot, image) == 64, "dma-buf image starts on the next line");
static_assert(_Alignof(FrameSlot) >= 64, "a slot starts on its own cache line");

extern FrameState frame_state;


static inline FrameSlot *shown(void) {
    return &frame_state.slot[frame_state.front];
}

static inline FrameSlot *spare(void) {
    return &frame_state.slot[frame_state.front ^ 1];
}

// Table-agnostic resolve shared by slot_from_ref and the handle test.
// The pack and generation live in frame/handle.h so a test can use them
// without this header. Resolving still needs a FrameSlot.
// slots are indices 1..2, orphans follow at 3.. in the same handle space.
static inline FrameSlot *slot_resolve(SlotRef ref, FrameSlot slots[2],
                                      FrameSlot *orphans, uint32_t orphan_count) {
    if (ref == 0) {
        return nullptr;
    }
    uint32_t generation = ref >> 8;
    uint32_t index = ref & 0xffu;
    FrameSlot *slot = nullptr;
    if (index >= 1u && index <= 2u) {
        slot = &slots[index - 1u];
    } else if (index >= 3u && index - 3u < orphan_count) {
        slot = &orphans[index - 3u];
    }
    if (slot == nullptr || slot->generation != generation) {
        return nullptr;
    }
    return slot;
}

SlotRef slot_ref(const FrameSlot *slot);
FrameSlot *slot_from_ref(SlotRef ref);

// Scene-slot timestamp queries. Resize and bar recordings do not use the pool.
void gpu_timer_init(VulkanContext *vk);
void gpu_timer_shutdown(VulkanContext *vk);
void gpu_timer_begin_frame(VulkanContext *vk, VkCommandBuffer cmd, FrameSlot *frame);
void gpu_timer_mark_compute_end(VkCommandBuffer cmd, const FrameSlot *frame);
void gpu_timer_mark_render_begin(VkCommandBuffer cmd, const FrameSlot *frame);
void gpu_timer_end_frame(VkCommandBuffer cmd, FrameSlot *frame);
void gpu_timer_begin_transfer(VulkanContext *vk, VkCommandBuffer cmd, FrameSlot *frame);
void gpu_timer_end_transfer(VkCommandBuffer cmd, FrameSlot *frame);
void gpu_timer_harvest(VulkanContext *vk, FrameSlot *frame);

bool wait_acquire_point(VulkanContext *vk, uint64_t point);
bool acquire_point_retired(VulkanContext *vk, uint64_t point);
bool poll_slot_released(const WaylandContext *wl, const FrameSlot *frame);
int ensure_frame_targets(VulkanContext *vk, FrameSlot *frame);
int record_and_submit_clear(VulkanContext *vk, FrameSlot *frame, VkSemaphore acquire,
                                   uint64_t signal_point, bool draw_live, uint32_t heat_dt);
int open_drm_render_node(const VulkanContext *vk);
int drm_syncobj_to_fd(int drm_fd, uint32_t handle);
void drm_syncobj_destroy(int drm_fd, uint32_t *handle);
int create_acquire_semaphore(VulkanContext *vk, VkSemaphore *out_timeline, VkSemaphore *out_sync);
int create_acquire_timeline(WaylandContext *wl);
int publish_acquire_point_from(VulkanContext *vk, VkSemaphore *io_sync_sem,
                               uint64_t point, int dmabuf_fd);
int arm_slot_read(RingContext *ring, uint32_t slot, bool fixed, int raw_fd,
                         uint32_t buf_index, uint64_t user_data);
int prep_cancel_fd(RingContext *ring, int fd, uint32_t slot, bool fixed, uint64_t user_data);
int ensure_release_fd(RingContext *ring);
int attach_release_eventfd(const FrameSlot *frame);
int arm_release_read(RingContext *ring);
int wait_gpu_release(RingContext *ring, WaylandContext *wl);
int create_release_timeline(WaylandContext *wl, FrameSlot *frame);
int wait_own_release(const FrameSlot *frame, uint64_t timeout_ns);
void clear_pixels(FrameSlot *frame);
bool slot_has_pixels(const FrameSlot *frame);
[[nodiscard]] int ensure_prime_color(VulkanContext *vk, FrameSlot *frame);
void destroy_prime_color(VulkanContext *vk, FrameSlot *frame);
void destroy_frame_gpu(VulkanContext *vk, FrameSlot *frame);
void reap_frame_resources(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame);
void sweep_ready_orphans(WaylandContext *wl, VulkanContext *vk, bool force_all);
int make_orphan_room(WaylandContext *wl, VulkanContext *vk);
int alloc_content_image(
    VulkanContext         *vk,
    WaylandContext        *wl,
    uint32_t               width,
    uint32_t               height,
    VulkanExportableImage *out,
    const char           **tranche_kind
);
bool configure_needs_ack(const WaylandContext *wl);
bool consume_noted_signal(WaylandContext *wl, int *signo);
bool pending_content_size(const WaylandContext *wl, uint32_t *width, uint32_t *height);
bool content_size_pending(const ChromeSpec *spec, const WaylandContext *wl);
int cancel_release_poll(RingContext *ring, WaylandContext *wl, bool *released);
int retire_pixels(WaylandContext *wl, VulkanContext *vk, FrameSlot *frame);
void blank_frame(FrameSlot *frame);
void retire_spare(WaylandContext *wl, VulkanContext *vk);
int alloc_spare(WaylandContext *wl, VulkanContext *vk);
void swap_front_spare(void);
void commit_turn_swap(WaylandContext *wl);
bool turn_wants_step(const WaylandContext *wl);
int turn_step(WaylandContext *wl, VulkanContext *vk, bool *queued);
void drop_resize_attempt(WaylandContext *wl, VulkanContext *vk);
int finish_idle_send(WaylandContext *wl, VulkanContext *vk,
                            const struct io_uring_cqe *cqe, uint32_t pending_serial);
int queue_resize_frame(WaylandContext *wl, VulkanContext *vk);
int queue_chrome_repaint(WaylandContext *wl, VulkanContext *vk);
int queue_feedback_realloc(WaylandContext *wl, VulkanContext *vk);
int frame_boot(void);
void ensure_inotify_watch(void);
void note_inotify_cqe(RingContext *ring, int32_t res, bool *armed);
void start_inotify(RingContext *ring, bool *armed);
void forward_reload_cqe(const struct io_uring_cqe *cqe, void *user);
void begin_file_relay(RunRelay *relay, bool *signal_armed, bool *got_signal,
                             int *signo, bool *inotify_armed);
bool end_file_relay(RunRelay *relay, WaylandContext *wl, VulkanContext *vk,
                           uint32_t pending_serial, bool *send_in_flight, bool *resend_wire,
                           int *run_rc, const char **why);
void release_persistent_fd(RingContext *ring);
// Watchdog for a stalled turn: the compositor stopped sending frame
// stamps mid-turn. Re-arms one frame callback and counts the stall.
int frame_watchdog(WaylandContext *wl, VulkanContext *vk, bool *queued);

// Vulkan 1.4 sync2. A discarded image has no source availability.
// A foreign-queue release publishes through the timeline signal, not a
// destination pipeline stage.
void frame_fill_discard_color(VkImageMemoryBarrier2 *out, VkImage image, bool unified);
void frame_fill_discard_depth(VkImageMemoryBarrier2 *out, VkImage image, bool unified);
void frame_fill_release_foreign(VkImageMemoryBarrier2 *out, const VulkanContext *vk, VkImage image);
void frame_cmd_barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2 *barriers, uint32_t count);
// Direct present releases the color attachment. A cross-device present copies
// the device-local target into the export image, then releases that copy.
void frame_cmd_finish_present(VkCommandBuffer cmd, const VulkanContext *vk, const FrameSlot *frame);
int frame_submit_slot_commands(VulkanContext *vk, FrameSlot *frame, VkSemaphore acquire,
                               uint64_t signal_point, uint64_t wait_point,
                               VkPipelineStageFlags2 wait_stage);

static inline bool frame_use_async_prime_transfer(const VulkanContext *vk, const FrameSlot *frame) {
    return vk != nullptr && frame != nullptr &&
           frame->image.prime_copy &&
           frame->prime_image != VK_NULL_HANDLE &&
           vk->queues.has_dedicated_transfer &&
           vk->queues.transfer_family_index != vk->queues.graphics_family_index &&
           !vk->queues.submission_aliased &&
           vk->transfer_cmd_pool != VK_NULL_HANDLE &&
           frame_state.prime_ready_sem != VK_NULL_HANDLE &&
           frame_state.prime_acquire_sync != VK_NULL_HANDLE;
}

uint32_t sim_wall_ms(void);
void note_sim_idle(void);

// Top bar: its own DMA-BUF and a desynced subsurface. Scene frames do not record it.
void frame_apply_layout(WaylandContext *wl, uint32_t window_w, uint32_t window_h,
                        uint32_t *content_w, uint32_t *content_h);
[[nodiscard]] int bar_open_surface(WaylandContext *wl);
[[nodiscard]] int bar_record_first(WaylandContext *wl, VulkanContext *vk);
void bar_note_presented(void);
// 1 when a bar attach is armed and the caller must send it. 0 when the bar
// is unchanged. -EAGAIN when the spare is still on screen or its command
// buffer is still pending on the shared acquire timeline.
[[nodiscard]] int bar_prepare(WaylandContext *wl, VulkanContext *vk, bool force);
void bar_drop_unsent(WaylandContext *wl, VulkanContext *vk);
void bar_install(void);
void bar_shutdown(WaylandContext *wl, VulkanContext *vk);

// Simulation quanta. The compositor callback does not call these.
void sim_on_timer(uint64_t expirations);
void sim_timer_set(struct RingContext *ring, bool want);
void frame_note_picture(void);
bool frame_sim_wanted(void);
bool frame_note_sim_cqe(struct RingContext *ring, const struct io_uring_cqe *cqe,
                        bool rearm, uint64_t *io_ticks);
void frame_apply_noted_sim(struct RingContext *ring, uint64_t last_ud, uint64_t expirations,
                           uint32_t retired, int res, bool alive, bool rearm);
int frame_run(void);
int frame_stop(void);

#endif /* ENGINE_FRAME_INTERNAL_H */
