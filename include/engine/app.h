#ifndef ENGINE_APP_H
#define ENGINE_APP_H

#include "frame/clock.h"
#include "ring/ring.h"
#include "vulkan/types.h"
#include <stdbool.h>
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

typedef struct EngineApp {
    // Short line printed once the window is up (controls, keys). May be null.
    const char *help;

    // GPU pipelines and program state. init is called after Vulkan and
    // chrome exist; load places the opening scene. shutdown runs before
    // chrome/Vulkan teardown, in reverse order of init.
    int (*init)(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user);
    void (*shutdown)(VulkanContext *vk);
    int (*load)(void);
    void (*reset)(void);

    // Per-slot depth/coverage targets (see EngineTargets).
    int (*create_targets)(VulkanContext *vk, EngineTargets *targets,
                                        uint32_t width, uint32_t height);
    void (*destroy_targets)(VulkanContext *vk, EngineTargets *targets);

    // Recording, in command-buffer order: compute, then claim_frame (slice
    // ownership for this signal point), then prepare_draw, then one
    // rendering pass. The world and the overlay share that pass when depth
    // exists. depth_format and samples are the pass pair, or zero for the
    // flat pass.
    void (*record_compute)(VkCommandBuffer cmd, uint32_t heat_dt, bool live);
    void (*prepare_draw)(VkCommandBuffer cmd, VkImage depth_image);
    void (*claim_frame)(VkSemaphore acquire, uint64_t signal_point);
    void (*draw)(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
    void (*draw_overlay)(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                         VkFormat format, VkFormat depth_format,
                         VkSampleCountFlagBits samples, uint32_t frame_ms,
                         bool have_stamp, uint64_t modifier);
    // Recording outcome, mirroring the submit that carries the frame.
    void (*record_abort)(void);
    void (*record_ok)(uint64_t signal_point);
    uint64_t (*record_published)(void);
    bool (*record_step_open)(void);
    bool (*record_poke_pending)(void);
    void (*release_frame)(uint64_t signal_point);

    // Compositor-clock turn: simulation/advance decide whether a new
    // frame is painted; commit folds the landed frame into shown state.
    bool (*simulation)(uint32_t time_ms, const FrameStep *step, uint32_t *heat_dt);
    void (*simulation_hold)(void);
    void (*simulation_commit)(uint32_t time_ms, uint32_t heat_dt);
    bool (*advance)(uint32_t time_ms, const FrameStep *step);
    bool (*busy)(void);
    bool (*consume_spin)(void);
    void (*commit)(void);
    void (*note_cqe)(void);
    void (*turn_report)(uint32_t *frames, uint32_t *missed);

    // Input, already translated by the engine (window-center pixels,
    // +X right, +Y up; Linux key codes). True when the picture changed.
    bool (*click)(float origin_x, float origin_y);
    // Wheel or touchpad notches. Positive steps zoom in. Null ignores them.
    // The engine does not name zoom; the hosted program decides.
    bool (*scroll)(float steps);
    // One UTF-8 string from a key tap or a repeat. Null ignores it.
    // The engine does not name text; the hosted program decides.
    bool (*text)(const char *utf8, uint32_t bytes);
    bool (*key)(uint32_t key, EngineKeyContext *ctx);
    bool (*camera_active)(void);

    // How a press is classified. The engine does not name the action.
    // Shift stays inside the pointer. IGNORE drops the press, HOLD keeps
    // it for polling, TAP queues one press. Null keeps the pointer's
    // built-in camera set and tap set.
    uint8_t (*route_key)(uint32_t evdev_key);
} EngineApp;

enum : uint8_t {
    ENGINE_KEY_IGNORE = 0,
    ENGINE_KEY_HOLD   = 1,
    ENGINE_KEY_TAP    = 2,
};

// Implemented by the hosted program (exactly one per binary).
const EngineApp *engine_app(void);

#endif /* ENGINE_APP_H */
