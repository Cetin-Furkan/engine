#ifndef ENGINE_APP_H
#define ENGINE_APP_H

#include "frame/clock.h"
#include "ring/ring.h"
#include "vulkan/types.h"
#include <stdint.h>

// The one seam between the engine and the program it hosts.
//
// The engine owns the platform: system probe, io_uring ring, hand-written
// Wayland wire protocol, Vulkan 1.4 device, and the WSI-free DMA-BUF
// present process (the engine's own swapchain). It does not know what is
// drawn. The hosted program (today: examples/demo) implements EngineApp
// and is the only code that names cubes, plots, meshes, or keys.
//
// Rules kept here: pure C23, no liburing, no libwayland, no WSI,
// nothing older than Vulkan 1.4. Hooks run on the engine's single
// io_uring thread; they must not block.

// Per-slot render targets the hosted program wants next to the presented
// DMA-BUF image. Handles only: ownership stays with the program, which
// creates and destroys them through create_targets/destroy_targets.
// A null depth_image means "no 3D pass this slot" (the engine then
// presents chrome over a clear, exactly as before for 2D-only slots).
typedef struct EngineTargets {
    VkImage               depth_image;
    VkImageView           depth_view;
    VkImage               coverage_image;
    VkImageView           coverage_view;
    // The pass the color image is cleared in. Zero means a 1-sample pass
    // with no depth attachment. Overlay pipelines must match this pair.
    VkFormat              depth_format;
    VkSampleCountFlagBits samples;
} EngineTargets;

// File-reload relay state. Reloading a file on the io_uring thread can
// complete a pending Wayland send, a signal read, or the chrome watch;
// the relay parks those completions so the idle loop finishes them once.
// The hosted program brackets its reloads with engine_relay_begin/end.
typedef struct EngineRelay {
    bool *signal_armed;
    bool *got_signal;
    int  *signo;
    bool *inotify_armed;
    bool  stolen_send;
    struct io_uring_cqe stolen_cqe;
} EngineRelay;

struct WaylandContext;
struct RingContext;

void engine_relay_begin(EngineRelay *relay, bool *signal_armed, bool *got_signal,
                        int *signo, bool *inotify_armed);
// True when the idle loop must leave (stolen send failed, signal, or a
// failed Wayland socket). Mirrors the contract the loop already used.
bool engine_relay_end(EngineRelay *relay, struct WaylandContext *wl, VulkanContext *vk,
                      uint32_t pending_serial, bool *send_in_flight, bool *resend_wire,
                      int *run_rc, const char **why);
// CQE forwarder for async file loads started under a relay.
void engine_reload_cqe(const struct io_uring_cqe *cqe, void *user);

// Context handed to the key hook. The relay fields point at idle-loop
// locals; set *stop (through run_rc/why as needed) to leave the loop.
typedef struct EngineKeyContext {
    struct WaylandContext *wl;
    VulkanContext         *vk;
    bool                  *signal_armed;
    bool                  *got_signal;
    int                   *signo;
    bool                  *inotify_armed;
    uint32_t               pending_serial;
    bool                  *send_in_flight;
    bool                  *resend_wire;
    int                   *run_rc;
    const char           **why;
    bool                  *stop;
    RingCqeFn              reload_cqe;
} EngineKeyContext;

// Hook contract:
//   - 7 [REQUIRED] hooks define a complete interactive hosted program:
//     init, shutdown, draw, advance, busy, click, key.
//   - All remaining 24 hooks/fields are [OPTIONAL] with well-defined platform
//     defaults documented below (1-sample sRGB color-only pass, automatic
//     4-slice transient BDA ring rewind/abort, platform turn counters, etc.).
//   - The platform also guards every hook pointer for null safety so an
//     omitted hook never crashes and never leaves the 8 ms sim timer armed.
typedef struct EngineApp {
    // [OPTIONAL] Short line printed once the window is up (default: "").
    const char *help;

    // [OPTIONAL] Command-line hook run before subsystems boot. Returns 0 to
    // continue, >0 to exit 0 after printing help/listing, or <0 on invalid flags.
    int (*configure)(int argc, char **argv);

    // [REQUIRED] GPU pipelines and program state. Called after Vulkan and chrome
    // exist. Return 0 on success, <0 to abort boot.
    int (*init)(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user);
    // [REQUIRED] Destroy hosted GPU/CPU state before chrome/Vulkan teardown.
    void (*shutdown)(VulkanContext *vk);
    // [OPTIONAL] Place the opening scene after init (default: succeeds with 0).
    int (*load)(void);
    // [OPTIONAL] Reset hosted state before subsystem boot (default: no-op).
    void (*reset)(void);

    // [OPTIONAL] Per-slot depth/coverage targets (default: zeroed EngineTargets,
    // running a 1-sample VK_FORMAT_B8G8R8A8_SRGB color-only rendering pass).
    int (*create_targets)(VulkanContext *vk, EngineTargets *targets,
                          uint32_t width, uint32_t height);
    // [OPTIONAL] Destroy per-slot depth/coverage targets (default: zeroes targets).
    void (*destroy_targets)(VulkanContext *vk, EngineTargets *targets);
    // [OPTIONAL] Per-frame clear color query in RGBA [0..1] (default: dark neutral).
    void (*clear_color)(float out_rgba[4]);

    // [OPTIONAL] Pre-render compute recording (default: no-op).
    void (*record_compute)(VkCommandBuffer cmd, uint32_t heat_dt, bool live);
    // [OPTIONAL] Offscreen/pre-pass draw preparation when depth exists (default: no-op).
    void (*prepare_draw)(VkCommandBuffer cmd, VkImage depth_image);
    // [OPTIONAL] Claim timeline-guarded per-frame slices for signal_point
    // (default: calls bda_transient_begin_frame(acquire, signal_point)).
    void (*claim_frame)(VkSemaphore acquire, uint64_t signal_point);
    // [REQUIRED] Main scene draw inside vkCmdBeginRendering (viewport and scissor
    // are already set; matches EngineTargets when create_targets is provided, or
    // 1-sample VK_FORMAT_B8G8R8A8_SRGB with VK_FORMAT_UNDEFINED depth otherwise).
    void (*draw)(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
    // [OPTIONAL] HUD/overlay draw at the end of the rendering pass (default: no-op).
    void (*draw_overlay)(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                         VkFormat format, VkFormat depth_format,
                         VkSampleCountFlagBits samples, uint32_t frame_ms,
                         bool have_stamp, uint64_t modifier);
    // [OPTIONAL] Recording outcome hooks mirroring the submit carrying the frame.
    // release_frame defaults to bda_transient_abort_frame(signal_point); others
    // default to no-op / 0 / false.
    void (*record_abort)(void);
    void (*record_ok)(uint64_t signal_point);
    uint64_t (*record_published)(void);
    bool (*record_step_open)(void);
    bool (*record_poke_pending)(void);
    void (*release_frame)(uint64_t signal_point);

    // The 8 ms timer calls simulation and advance. A frame callback only
    // presents. commit folds the landed frame into shown state.
    // time_ms on simulation is monotonic milliseconds since the first
    // timer sample, for pulse windows. It is not a compositor stamp.
    // step->sim_dt_ms is the only interval the solver may integrate.
    // time_ms on advance is the simulation cursor, not the wall stamp.
    // [OPTIONAL] Continuous field/pulse step (default: *heat_dt = 0, returns false).
    bool (*simulation)(uint32_t time_ms, const FrameStep *step, uint32_t *heat_dt);
    // [OPTIONAL] Called when a spinning turn settles (default: no-op).
    void (*simulation_hold)(void);
    // [OPTIONAL] Commit accumulated simulation interval on present (default: no-op).
    void (*simulation_commit)(uint32_t time_ms, uint32_t heat_dt);
    // [REQUIRED] Advance hosted state on the issued simulation cursor (time_ms,
    // step->sim_dt_ms). Return true when the picture changed.
    bool (*advance)(uint32_t time_ms, const FrameStep *step);
    // [REQUIRED] True while the hosted program needs the 8 ms simulation timer
    // armed; false once quiescent so the platform disarms the timer into Loop idle.
    bool (*busy)(void);
    // [OPTIONAL] True when a completed spin warrants simulation_hold (default: false).
    bool (*consume_spin)(void);
    // [OPTIONAL] Fold the landed spare frame into shown state (default: no-op).
    void (*commit)(void);
    // [OPTIONAL] Telemetry hook invoked on each io_uring CQE (default: no-op).
    void (*note_cqe)(void);
    // [OPTIONAL] Asynchronous io_uring completion hook for hosted-app tags.
    // Returns true when a newly resident resource warrants a picture (default: false).
    bool (*on_cqe)(const struct io_uring_cqe *cqe);
    // [OPTIONAL] Override frame/missed count reported on Loop idle (default:
    // platform turn_frames and FrameClock::missed).
    void (*turn_report)(uint32_t *frames, uint32_t *missed);

    // Input, already translated by the engine (window-center pixels,
    // +X right, +Y up; Linux key codes). True when the picture changed.
    // [REQUIRED] Left-button press at content-centered (origin_x, origin_y).
    bool (*click)(float origin_x, float origin_y);
    // [OPTIONAL] Motion while right/middle button is held (default: false).
    bool (*drag)(float dx, float dy, uint32_t buttons);
    // [OPTIONAL] Wheel or touchpad notches; positive zooms in (default: false).
    bool (*scroll)(float steps);
    // [OPTIONAL] UTF-8 string from a key tap or repeat (default: false).
    bool (*text)(const char *utf8, uint32_t bytes);
    // [REQUIRED] Tapped Linux evdev key; set *ctx->stop = true to exit cleanly.
    bool (*key)(uint32_t key, EngineKeyContext *ctx);
    // [OPTIONAL] True while held camera keys should keep the 8 ms timer armed
    // (default: false).
    bool (*camera_active)(void);

    // [OPTIONAL] Classify a key press into IGNORE, HOLD, or TAP (default:
    // platform built-in tap/hold sets, where ESC/Q/Space/digits/letters tap).
    uint8_t (*route_key)(uint32_t evdev_key);

    // [OPTIONAL] Watched source file path and hot-reload callback for inotify
    // (default: nullptr / false).
    const char *(*watched_path)(void);
    bool (*reload_watched)(void);
} EngineApp;

enum : uint8_t {
    ENGINE_KEY_IGNORE = 0,
    ENGINE_KEY_HOLD   = 1,
    ENGINE_KEY_TAP    = 2,
};

// wl_pointer buttons the drag hook can see. A left press stays a click.
enum : uint32_t {
    ENG_BUTTON_LEFT   = 1u << 0,
    ENG_BUTTON_RIGHT  = 1u << 1,
    ENG_BUTTON_MIDDLE = 1u << 2,
};

// Implemented by the hosted program (exactly one per binary).
const EngineApp *engine_app(void);

#endif /* ENGINE_APP_H */
