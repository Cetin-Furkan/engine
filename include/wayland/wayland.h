#ifndef ENGINE_WAYLAND_H
#define ENGINE_WAYLAND_H

#include "ring/ring.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

enum : uint32_t {
    ENG_WL_GLOBALS          = 96,
    ENG_WL_NAME             = 128,
    ENG_FD_PARK             = 32,
    ENG_ID_FREE             = 64,
    ENG_WL_MAX_MODIFIERS    = 64,
    ENG_WL_MAX_INDICES      = 512,
    ENG_WL_DESTROY_LATER    = 16,
    ENG_DRM_FORMAT_ARGB8888 = 0x34325241u,
};

// xdg_toplevel state values, one bit each. Tiled is any of the four edges.
enum : uint32_t {
    ENG_TOP_MAXIMIZED    = 1u << 1,
    ENG_TOP_FULLSCREEN   = 1u << 2,
    ENG_TOP_RESIZING     = 1u << 3,
    ENG_TOP_ACTIVATED    = 1u << 4,
    ENG_TOP_TILED_LEFT   = 1u << 5,
    ENG_TOP_TILED_RIGHT  = 1u << 6,
    ENG_TOP_TILED_TOP    = 1u << 7,
    ENG_TOP_TILED_BOTTOM = 1u << 8,
    ENG_TOP_SUSPENDED    = 1u << 9,
};

// zxdg_toplevel_decoration_v1.mode. Client-side is 1. Server-side is 2.
enum : uint32_t {
    ENG_DECOR_NONE   = 0,
    ENG_DECOR_CLIENT = 1,
    ENG_DECOR_SERVER = 2,
};

typedef struct WaylandGlobal {
    uint32_t name;
    uint32_t version;
    char     interface[ENG_WL_NAME];
} WaylandGlobal;

typedef struct WaylandFormatTableEntry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
} WaylandFormatTableEntry;

typedef struct WaylandContext {
    RingContext *ring;
    bool         fixed_socket_installed;
    bool         recv_armed;
    bool         synced;
    bool         failed;
    char         err[160];
    uint32_t     display_id;
    uint32_t     registry_id;
    uint32_t     sync_id;
    uint32_t     next_id;
    uint32_t     free_id[ENG_ID_FREE];
    uint32_t     nfree;

    // Discovered global registry names & versions
    uint32_t compositor_global_name;
    uint32_t compositor_version;
    uint32_t xdg_wm_base_global_name;
    uint32_t xdg_wm_base_version;
    uint32_t linux_dmabuf_global_name;
    uint32_t linux_dmabuf_version;
    uint32_t drm_syncobj_global_name;
    uint32_t drm_syncobj_version;
    uint32_t seat_global_name;
    uint32_t seat_version;
    uint32_t shm_global_name;
    uint32_t shm_version;
    uint32_t cursor_shape_global_name;
    uint32_t cursor_shape_version;
    uint32_t viewporter_global_name;
    uint32_t viewporter_version;
    uint32_t subcompositor_global_name;
    uint32_t subcompositor_version;
    uint32_t decoration_global_name;
    uint32_t decoration_version;

    // Bound global object IDs
    uint32_t compositor_id;
    uint32_t compositor_bound_version;
    uint32_t xdg_wm_base_id;
    uint32_t linux_dmabuf_id;
    uint32_t linux_dmabuf_bound_version;
    uint32_t drm_syncobj_id;
    uint32_t seat_id;
    uint32_t seat_bound_version;
    uint32_t shm_id;
    uint32_t cursor_shape_manager_id;
    uint32_t cursor_shape_bound_version;
    uint32_t viewporter_id;
    uint32_t viewport_id;
    uint32_t subcompositor_id;
    uint32_t decoration_manager_id;
    uint32_t pointer_id;
    uint32_t keyboard_id;
    uint32_t cursor_shape_device_id;
    uint32_t seat_capabilities;
    bool     shm_argb8888;
    bool     configure_resizing;
    // xdg_toplevel.configure states. ENG_TOP_* bits. Empty means floating.
    uint32_t toplevel_states;
    bool     noted_signal;
    int      noted_signal_res;
    // Completions a blocking io_uring wait took off the ring. The idle loop
    // applies them. They are not handled inside the wait, because that wait
    // can be in the middle of a send buffer.
    bool     noted_timer;
    bool     noted_inotify;
    int      noted_inotify_res;
    bool     noted_sim;
    int      noted_sim_res;
    bool     noted_sim_alive;
    uint64_t noted_sim_exp;
    bool     noted_send;
    int      noted_send_res;
    // True from the moment an ENG_UD_WL_SEND SQE is queued until its CQE is seen.
    // A second send over the same buffer is refused while this is set.
    bool     send_queued;
    bool     buffers_detached;
    uint32_t destroy_later[ENG_WL_DESTROY_LATER];
    uint32_t destroy_later_n;
    bool     send_tracks_ack;
    bool     lock_toplevel_size;
    int32_t  lock_width;
    int32_t  lock_height;
    // Positive values are xdg_toplevel.set_min_size. Zero means do not send one.
    int32_t  min_width;
    int32_t  min_height;
    uint32_t explicit_sync_id;
    uint32_t acquire_timeline_id;
    uint32_t release_timeline_id;

    // Active window, feedback, buffer, and frame callback object IDs
    uint32_t surface_id;
    uint32_t bar_surface_id;
    uint32_t bar_subsurface_id;
    uint32_t bar_sync_id;
    // Top band in window pixels. The scene surface's pointer y is short by this.
    uint32_t bar_px;
    // Window geometry in the scene surface's coordinate space. y is negative
    // when the bar subsurface sits above the scene buffer. Zero width means
    // "use the buffer size at 0,0" (no bar yet).
    int32_t  geom_x;
    int32_t  geom_y;
    uint32_t geom_w;
    uint32_t geom_h;
    // Included in the next scene commit, or sent alone by wayland_queue_bar_commit.
    uint32_t bar_attach_buffer;
    uint32_t bar_attach_w;
    uint32_t bar_attach_h;
    uint32_t bar_attach_acquire_tl;
    uint64_t bar_attach_acquire_pt;
    uint32_t bar_attach_release_tl;
    uint64_t bar_attach_release_pt;
    uint32_t xdg_surface_id;
    uint32_t xdg_toplevel_id;
    uint32_t feedback_id;
    uint32_t buffer_params_id;
    uint32_t buffer_id;
    uint32_t frame_cb_id;
    // zxdg_toplevel_decoration_v1. Mode stays NONE until the configure event.
    uint32_t decoration_id;
    uint32_t decoration_mode;
    bool     decoration_configured;
    bool     decoration_server_noted;
    // Live wl_region objects. Zero means the next commit creates one.
    // A later commit reuses the object while the pixel size is unchanged.
    uint32_t scene_opaque_id;
    uint32_t scene_opaque_w;
    uint32_t scene_opaque_h;
    uint32_t bar_opaque_id;
    uint32_t bar_opaque_w;
    uint32_t bar_opaque_h;

    // Toplevel & xdg_surface configure state
    uint32_t configure_width;
    uint32_t configure_height;
    uint32_t configure_serial;
    uint32_t acked_configure_serial;
    bool     configure_ack_valid;
    bool     toplevel_configured;
    bool     xdg_surface_configured;
    bool     toplevel_closed;
    bool     buffer_released;
    bool     frame_done;
    uint32_t frame_time_ms;
    bool     frame_stamp_valid;

    // DMA-BUF surface feedback state
    int                      feedback_table_fd;
    WaylandFormatTableEntry *feedback_table;
    uint32_t                 feedback_table_count;
    size_t                   feedback_table_bytes;
    int64_t                  feedback_target_major;
    int64_t                  feedback_target_minor;
    int64_t                  feedback_alt_major;
    int64_t                  feedback_alt_minor;
    bool                     feedback_has_alt;
    int64_t                  feedback_main_major;
    int64_t                  feedback_main_minor;
    bool                     feedback_has_main_dev;
    bool                     feedback_done;
    // Modifiers the compositor's main device can import when that device is
    // not the Vulkan render GPU. Merged into all_modifiers for a cross-device
    // copy. Scanout stays empty: those flags describe the other GPU's tiling.
    uint64_t                 prime_modifiers[ENG_WL_MAX_MODIFIERS];
    uint32_t                 prime_modifier_count;
    bool                     feedback_cross_device;
    bool                     feedback_round_sealed;
    uint32_t                 feedback_producer_mods;
    uint32_t                 feedback_consumer_mods;
    uint32_t                 feedback_producer_tranches;
    uint32_t                 feedback_consumer_tranches;
    uint32_t                 feedback_other_tranches;

    // Staging for the tranche currently being parsed
    int64_t  tranche_major;
    int64_t  tranche_minor;
    bool     tranche_has_dev;
    uint32_t tranche_flags;
    uint16_t tranche_indices[ENG_WL_MAX_INDICES];
    uint32_t tranche_index_count;

    // Collected DRM_FORMAT_ARGB8888 modifiers for the matched GPU DRM node
    uint64_t scanout_modifiers[ENG_WL_MAX_MODIFIERS];
    uint32_t scanout_modifier_count;
    uint64_t all_modifiers[ENG_WL_MAX_MODIFIERS];
    uint32_t all_modifier_count;

    // Receive state
    uint32_t      control_reserve;
    struct msghdr recv_msg;
    struct iovec  recv_iov;
    unsigned char recv_control[256];

    // Primary batch send state (supports 1 optional SCM_RIGHTS fd)
    struct msghdr send_msg;
    struct iovec  send_iov;
    unsigned char send_bytes[2048];
    uint32_t      send_len;
    uint32_t      send_off;
    unsigned char send_control[CMSG_SPACE(sizeof(int))];

    // Dedicated pong send state (isolated from send_bytes / ENG_UD_WL_SEND)
    struct msghdr pong_msg;
    struct iovec  pong_iov;
    unsigned char pong_bytes[12];
    uint32_t      pong_off;
    bool          pong_in_flight;

    struct sockaddr_un addr;
    unsigned           addr_len;
    char               path[108];

    unsigned char stream[16384];
    uint32_t      stream_len;

    WaylandGlobal globals[ENG_WL_GLOBALS];
    uint32_t      nglobals;
    uint32_t      globals_seen;

    int      parked_fds[ENG_FD_PARK];
    uint32_t parked_head;
    uint32_t nparked;
    uint32_t recv_cqes;
    uint16_t noted_pager_mask;
    int32_t  noted_pager_res[ENG_UD_PAGER_COUNT];
    bool     is_initialized;
} WaylandContext;

[[nodiscard]] int wayland_init(void);
void wayland_shutdown(void);
[[nodiscard]] WaylandContext *wayland_get_context(void);
[[nodiscard]] uint32_t wayland_alloc_id(WaylandContext *w);

// The next SCM_RIGHTS fd, in arrival order. The caller closes it.
[[nodiscard]] bool wayland_take_parked_fd(WaylandContext *w, int *out_fd);
void wayland_on_cqe(WaylandContext *w, const struct io_uring_cqe *cqe);
[[nodiscard]] int wayland_ensure_recv(WaylandContext *w);

/**
 * @brief Creates wl_surface, xdg_surface, and xdg_toplevel, commits an empty body,
 * and waits until both xdg_toplevel.configure and xdg_surface.configure arrive.
 */
[[nodiscard]] int wayland_create_toplevel_and_configure(
    WaylandContext *w,
    const char     *title,
    const char     *app_id,
    uint32_t       *out_width,
    uint32_t       *out_height,
    uint32_t       *out_serial
);

/**
 * @brief Requests zwp_linux_dmabuf_v1.get_surface_feedback for the active wl_surface.
 * Tranches for the render node, and the primary node when has_alt is set, fill
 * scanout_modifiers and all_modifiers. Tranches for the compositor main device
 * are kept when that device is different, so a muxless dGPU can export a buffer
 * the display GPU imports. Only DRM_FORMAT_ARGB8888 is collected.
 */
[[nodiscard]] int wayland_query_surface_dmabuf_feedback(
    WaylandContext *w,
    int64_t         target_major,
    int64_t         target_minor,
    int64_t         alt_major,
    int64_t         alt_minor,
    bool            has_alt
);

/**
 * @brief Imports a DMA-BUF fd into a wl_buffer via zwp_linux_buffer_params_v1.add + create_immed.
 * The fd is placed in SCM_RIGHTS and must stay open until this call returns.
 */
[[nodiscard]] int wayland_import_dmabuf_immed(
    WaylandContext *w,
    int             dmabuf_fd,
    uint32_t        width,
    uint32_t        height,
    uint32_t        stride,
    uint32_t        offset,
    uint32_t        drm_fourcc,
    uint64_t        modifier,
    uint32_t       *out_buffer_id
);

/**
 * @brief Imports one DRM syncobj fd as a wp_linux_drm_syncobj_timeline_v1.
 * The fd is placed in SCM_RIGHTS and must stay open until this call returns.
 */
[[nodiscard]] int wayland_import_syncobj_timeline(
    WaylandContext *w,
    int             syncobj_fd,
    uint32_t       *out_timeline_id
);

/**
 * @brief Binds wp_linux_drm_syncobj_surface_v1 to the current wl_surface.
 */
[[nodiscard]] int wayland_bind_explicit_sync(WaylandContext *w);

/**
 * @brief Acknowledges configure, sets window geometry, sets acquire and release
 * timeline points, attaches the buffer, damages, commits, and synchronizes
 * with the compositor. No standing frame callback is armed.
 */
[[nodiscard]] int wayland_present_single_frame_and_wait(
    WaylandContext *w,
    uint32_t        buffer_id,
    uint32_t        width,
    uint32_t        height,
    uint32_t        configure_serial,
    uint32_t        acquire_timeline,
    uint64_t        acquire_point,
    uint32_t        release_timeline,
    uint64_t        release_point
);

enum : int32_t {
    ENG_VIEWPORT_LEAVE = 0,
    ENG_VIEWPORT_UNSET = 2,
};

/**
 * @brief Queues ack_configure, set_window_geometry, and an empty wl_surface.commit.
 * ENG_VIEWPORT_UNSET also clears a wp_viewport destination. No buffer is attached,
 * no frame callback is armed, and no sync point is set. The bytes stay live until
 * the ENG_UD_WL_SEND completion is noted.
 */
[[nodiscard]] int wayland_queue_configure_ack(
    WaylandContext *w,
    uint32_t        serial,
    uint32_t        width,
    uint32_t        height,
    int32_t         viewport_mode
);

/**
 * @brief Records the result of a configure-ack send queued above.
 * Returns 0 when the whole buffer was accepted, 1 when the caller must queue
 * the remaining tail, and a negative errno on a hard failure.
 * On full success, acked_configure_serial becomes acked_serial.
 */
[[nodiscard]] int wayland_note_send_cqe(
    WaylandContext             *w,
    const struct io_uring_cqe  *cqe,
    uint32_t                    acked_serial
);

/**
 * @brief Attaches a null buffer and commits, with no acquire or release point.
 * The compositor drops the current buffer and signals its release point.
 */
[[nodiscard]] int wayland_detach_buffer(WaylandContext *w);

/**
 * @brief Destroys the explicit-sync objects, wl_buffer, xdg_toplevel,
 * xdg_surface, and wl_surface cleanly.
 */
void wayland_destroy_frame_objects(WaylandContext *w);

void wayland_fail(WaylandContext *w, const char *msg);

void wayland_rewind_send(WaylandContext *w, uint32_t mark);

[[nodiscard]] bool wayland_put_raw(
    WaylandContext *w,
    uint32_t        obj_id,
    uint16_t        opcode,
    const void     *payload,
    uint16_t        payload_len
);

[[nodiscard]] int wayland_send_batch(WaylandContext *w);

// Remembers an opcode-0 destroy until the send buffer is idle.
// False when the list is full.
bool wayland_defer_destroy(WaylandContext *w, uint32_t id);

// Encodes every deferred destroy and queues one ENG_UD_WL_SEND.
// 1 when that send was queued, 0 when nothing is waiting,
// -EBUSY when a send already owns the buffer.
[[nodiscard]] int wayland_queue_destroys(WaylandContext *w);

/**
 * @brief Appends a wl_display.sync and waits for it. fd_to_send is SCM_RIGHTS, or -1.
 */
[[nodiscard]] int wayland_roundtrip_batch(WaylandContext *w, int fd_to_send);

/**
 * @brief Queues the bytes already in the send buffer as ENG_UD_WL_SEND.
 * When tracks_ack is set, the completion records acked_configure_serial.
 */
[[nodiscard]] int wayland_queue_send(WaylandContext *w, bool tracks_ack);

/**
 * @brief Queues one commit. buffer_id 0 attaches nothing.
 * A non-zero buffer sets acquire and release points, attaches, and damages.
 * arm_frame requests wl_surface.frame before that commit.
 * with_configure also sends the toplevel size, ack_configure when that serial
 * is new, set_window_geometry, and clears a viewport destination. The ack is
 * recorded only when the ENG_UD_WL_SEND completion accepts every byte.
 * Returns -EBUSY when a send is already buffered, -EALREADY when a callback
 * is already armed, and 0 after the bytes are queued as ENG_UD_WL_SEND.
 */
/**
 * @brief Remembers the window geometry. The scene buffer is smaller than this
 * when a bar subsurface sits outside it. y may be negative.
 */
void wayland_set_window_geom(WaylandContext *w, int32_t x, int32_t y,
                             uint32_t width, uint32_t height);

/**
 * @brief Creates the top-bar wl_surface, a desynced wl_subsurface at (x, y),
 * and its explicit-sync object. The parent surface must already exist.
 */
[[nodiscard]] int wayland_create_bar(WaylandContext *w, int32_t x, int32_t y);

/**
 * @brief Arms one bar-surface attach. The next scene commit includes it, or
 * wayland_queue_bar_commit sends it alone. Zero buffer clears the arm.
 */
void wayland_arm_bar(WaylandContext *w, uint32_t buffer, uint32_t width, uint32_t height,
                     uint32_t acquire_timeline, uint64_t acquire_point,
                     uint32_t release_timeline, uint64_t release_point);

/**
 * @brief Queues the armed bar attach and commit. Does not touch the scene surface
 * and does not arm a frame callback. Returns -EAGAIN when nothing is armed.
 */
[[nodiscard]] int wayland_queue_bar_commit(WaylandContext *w);

[[nodiscard]] int wayland_queue_turn_commit(
    WaylandContext *w,
    uint32_t        buffer_id,
    uint32_t        width,
    uint32_t        height,
    int32_t         dmg_x,
    int32_t         dmg_y,
    uint32_t        dmg_w,
    uint32_t        dmg_h,
    uint32_t        acquire_timeline,
    uint64_t        acquire_point,
    uint32_t        release_timeline,
    uint64_t        release_point,
    bool            arm_frame,
    bool            with_configure,
    uint32_t        configure_serial
);

#endif /* ENGINE_WAYLAND_H */