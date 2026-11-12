#include "frame/loop/lane.h"

// Content-local hits. A hit asks for one present. The callback is not
// left armed after the picture lands; turn_step commits the buffer with
// the callback when another frame is owed.

bool loop_take_input(WaylandContext *wl, VulkanContext *vk,
                     bool *signal_poll_armed, bool *got_signal, int *signo,
                     bool *inotify_armed, uint32_t pending_serial,
                     bool *send_in_flight, bool *resend_wire,
                     int *run_rc, const char **why) {
    const EngineApp *app = engine_app();
    int32_t sx = 0;
    int32_t sy = 0;
    while (pointer_take_spawn(&sx, &sy)) {
        float ox = (float)sx - (float)shown()->image.width * 0.5f;
        float oy = (float)shown()->image.height * 0.5f - (float)sy;
        if (app != nullptr && app->click != nullptr && app->click(ox, oy)) {
            frame_note_picture();
        }
    }
    uint32_t key = 0;
    bool stop_run = false;
    while (pointer_take_key(&key)) {
        EngineKeyContext key_ctx = {
            .wl             = wl,
            .vk             = vk,
            .signal_armed   = signal_poll_armed,
            .got_signal     = got_signal,
            .signo          = signo,
            .inotify_armed  = inotify_armed,
            .pending_serial = pending_serial,
            .send_in_flight = send_in_flight,
            .resend_wire    = resend_wire,
            .run_rc         = run_rc,
            .why            = why,
            .stop           = &stop_run,
            .reload_cqe     = forward_reload_cqe,
        };
        if (app != nullptr && app->key != nullptr && app->key(key, &key_ctx)) {
            frame_note_picture();
        }
        if (stop_run) {
            return true;
        }
    }
    pointer_pump_text();
    if (wl != nullptr) {
        wl->clipboard_updated = false;
    }
    char utf8[16];
    uint32_t utf8_n = 0;
    while (pointer_take_text(utf8, sizeof utf8, &utf8_n)) {
        if (app != nullptr && app->text != nullptr && app->text(utf8, utf8_n)) {
            frame_note_picture();
        }
    }
    float steps = 0.0f;
    if (pointer_take_scroll(&steps) && app != nullptr && app->scroll != nullptr &&
        app->scroll(steps)) {
        frame_note_picture();
    }
    float drag_x = 0.0f;
    float drag_y = 0.0f;
    uint32_t drag_buttons = 0;
    if (pointer_take_drag(&drag_x, &drag_y, &drag_buttons) &&
        app != nullptr && app->drag != nullptr &&
        app->drag(drag_x, drag_y, drag_buttons)) {
        frame_note_picture();
    }
    return false;
}
