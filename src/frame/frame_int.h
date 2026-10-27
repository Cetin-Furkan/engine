#ifndef ENGINE_FRAME_INT_H
#define ENGINE_FRAME_INT_H

#include "engine/app.h"
#include "frame/clock.h"
#include "chrome/gpu.h"
#include "chrome/layer.h"
#include "chrome/spec.h"
#include "input/pointer.h"
#include "ring/ring.h"
#include "wayland/wayland.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include <drm/drm.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

// Present process shared by boot/, loop/, and present/. The s_* names are the
// fields of frame_state. The moved bodies still use those names.

enum : uint64_t {
    k_ud_signal_cancel  = UINT64_MAX,
    k_ud_release_cancel = UINT64_MAX - 1u,
    k_ud_inotify_cancel = UINT64_MAX - 2u,
};

enum : uint32_t {
    k_orphan_cap = 8u,
};

enum : int {
    k_turn_arm  = 0,
    k_turn_run  = 1,
    k_turn_done = 2,
};

enum : uint32_t {
    k_inotify_mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB |
                     IN_DELETE_SELF | IN_MOVE_SELF,
};

// One presented image. The DRM node and the acquire timeline are not in here.
typedef struct FrameSlot {
    VulkanExportableImage image;
    VkImageView           image_view;
    VkFormat              view_format;
    EngineTargets         targets;
    // Bumped every time the slot is blanked/reaped. A SlotRef taken before
    // the bump no longer resolves, so a stale async release can never be
    // mistaken for the slot's new image (generation-tagged handle).
    uint32_t              generation;
    VkCommandBuffer       cmd_buf;
    VkSemaphore           release_semaphore;
    uint32_t              release_handle;
    uint32_t              release_timeline_id;
    int                   release_eventfd;
    uint64_t              release_point;
    uint64_t              acquire_point;
    uint32_t              wl_buffer_id;
    bool                  release_pending;
    bool                  foreign_owned;
} FrameSlot;

typedef EngineRelay RunRelay;

typedef struct FrameState {
    int         drm_fd;
    VkSemaphore acquire;
    bool        presented;

    // slot[front] is on screen. The other slot is the spare while the turn runs.
    FrameSlot slot[2];
    int       front;
    bool      spare_live;

    int      turn_phase;
    bool     turn_swap_pending;
    uint64_t pending_release;

    // A resize commit is queued like a turn commit. The old front is retired
    // when ENG_UD_WL_SEND accepts the bytes, not when a wl_callback.done arrives.
    bool      resize_pending;
    FrameSlot resize_fresh;
    uint64_t  resize_release;
    uint32_t  resize_serial;
    uint32_t  resize_old_buffer;
    uint32_t  resize_old_w;
    uint32_t  resize_old_h;
    char      resize_tranche[16];

    // Last acquire point signaled on the shared acquire timeline.
    // Each slot keeps one release syncobj. The point starts at 1. A resize
    // reallocates the images and bumps that same point. It does not import again.
    uint64_t sync_point;

    // Deferred retirement ring for buffers whose compositor release point has
    // not yet signaled at the moment of resize replacement.
    FrameSlot orphans[k_orphan_cap];
    uint32_t  orphan_count;
    int       stuck_release_fd;

    // One eventfd for the life of the process. DRM_IOCTL_SYNCOBJ_EVENTFD is
    // issued only while shutting down, for the point still on screen. The turn
    // does not sleep on it. Slots keep release_eventfd at -1 so a reap cannot
    // close this fd.
    int                      release_fd;
    bool                     release_fixed;
    uint64_t                 release_counter;
    struct signalfd_siginfo siginfo;
    bool                     same_repaint;
    bool                     chrome_dirty;
    int                      inotify_fd;
    int                      inotify_wd;
    bool                     inotify_fixed;
    bool                     inotify_stop;
    bool                     inotify_warned;
    RunRelay                *relay;
    alignas(64) FrameClock   clock;
    bool                     timer_armed;
    bool                     loop_was_busy;
    uint64_t                 last_stamp_mono_ns;
    bool                     stall_reported;
} FrameState;

extern FrameState frame_state;

#define s_drm_fd (frame_state.drm_fd)
#define s_acquire (frame_state.acquire)
#define s_presented (frame_state.presented)
#define s_slot (frame_state.slot)
#define s_front (frame_state.front)
#define s_spare_live (frame_state.spare_live)
#define s_turn_phase (frame_state.turn_phase)
#define s_turn_swap_pending (frame_state.turn_swap_pending)
#define s_pending_release (frame_state.pending_release)
#define s_resize_pending (frame_state.resize_pending)
#define s_resize_fresh (frame_state.resize_fresh)
#define s_resize_release (frame_state.resize_release)
#define s_resize_serial (frame_state.resize_serial)
#define s_resize_old_buffer (frame_state.resize_old_buffer)
#define s_resize_old_w (frame_state.resize_old_w)
#define s_resize_old_h (frame_state.resize_old_h)
#define s_resize_tranche (frame_state.resize_tranche)
#define s_sync_point (frame_state.sync_point)
#define s_orphans (frame_state.orphans)
#define s_orphan_count (frame_state.orphan_count)
#define s_stuck_release_fd (frame_state.stuck_release_fd)
#define s_release_fd (frame_state.release_fd)
#define s_release_fixed (frame_state.release_fixed)
#define s_release_counter (frame_state.release_counter)
#define s_siginfo (frame_state.siginfo)
#define s_same_repaint (frame_state.same_repaint)
#define s_chrome_dirty (frame_state.chrome_dirty)
#define s_inotify_fd (frame_state.inotify_fd)
#define s_inotify_wd (frame_state.inotify_wd)
#define s_inotify_fixed (frame_state.inotify_fixed)
#define s_inotify_stop (frame_state.inotify_stop)
#define s_inotify_warned (frame_state.inotify_warned)
#define s_relay (frame_state.relay)
#define s_clock (frame_state.clock)
#define s_timer_armed (frame_state.timer_armed)
#define s_loop_was_busy (frame_state.loop_was_busy)
#define s_last_stamp_mono_ns (frame_state.last_stamp_mono_ns)
#define s_stall_reported (frame_state.stall_reported)

static inline FrameSlot *shown(void) {
    return &s_slot[s_front];
}

static inline FrameSlot *spare(void) {
    return &s_slot[s_front ^ 1];
}

// Generation-tagged slot handle: (generation << 8) | (index + 1).
// Zero is never valid. slot_from_ref returns nullptr for a stale handle,
// which is how an async completion names a slot across a reap/resize
// without risking use-after-replace of the DMA-BUF image.
typedef uint32_t SlotRef;

static inline SlotRef slot_ref_pack(uint32_t generation, uint32_t index_plus_one) {
    if (generation == 0 || index_plus_one == 0 || index_plus_one > 0xffu) {
        return 0;
    }
    return (generation << 8) | index_plus_one;
}

static inline uint32_t slot_next_generation(uint32_t generation) {
    generation += 1u;
    return generation != 0 ? generation : 1u;
}

// Table-agnostic resolve shared by slot_from_ref and the handle test:
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

bool wait_acquire_point(VulkanContext *vk, uint64_t point);
bool acquire_point_retired(VulkanContext *vk, uint64_t point);
bool poll_release_semaphore(VulkanContext *vk, VkSemaphore semaphore, uint64_t point);
int ensure_frame_targets(VulkanContext *vk, FrameSlot *frame);
int record_and_submit_clear(VulkanContext *vk, FrameSlot *frame, VkSemaphore acquire,
                                   uint64_t signal_point, bool draw_live, uint32_t heat_dt);
int export_semaphore_fd(VulkanContext *vk, VkSemaphore semaphore);
int import_release_semaphore_fd(VulkanContext *vk, int syncobj_fd, VkSemaphore *out);
int drm_syncobj_from_fd(int drm_fd, int syncobj_fd, uint32_t *out_handle);
int drm_syncobj_to_fd(int drm_fd, uint32_t handle);
void drm_syncobj_destroy(int drm_fd, uint32_t *handle);
int create_acquire_semaphore(VulkanContext *vk, VkSemaphore *out);
int arm_slot_read(RingContext *ring, uint32_t slot, bool fixed, int raw_fd,
                         void *buf, uint32_t len, uint64_t user_data);
int prep_cancel_fd(RingContext *ring, int fd, uint32_t slot, bool fixed, uint64_t user_data);
int ensure_release_fd(RingContext *ring);
int attach_release_eventfd(const FrameSlot *frame);
int arm_release_read(RingContext *ring);
int wait_gpu_release(RingContext *ring, WaylandContext *wl);
int create_release_timeline(VulkanContext *vk, WaylandContext *wl, FrameSlot *frame);
VkResult wait_own_release(VulkanContext *vk, FrameSlot *frame, uint64_t timeout_ns);
void clear_pixels(FrameSlot *frame);
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
int frame_run(void);
// Watchdog for a stalled turn: the compositor stopped sending frame
// stamps mid-turn. Re-arms one frame callback and counts the stall.
int frame_watchdog(WaylandContext *wl, VulkanContext *vk, bool *queued);
int frame_run(void);
int frame_stop(void);

#endif /* ENGINE_FRAME_INT_H */
