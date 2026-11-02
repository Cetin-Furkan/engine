// Client-side decoration. zxdg_toplevel_decoration_v1.mode.client_side is 1.
// Requesting it tells the compositor not to wrap this window in its own frame.
#include "wayland/internal.h"
#include <stdio.h>

bool wl_bind_decoration_manager(WaylandContext *w) {
    if (w == nullptr || w->decoration_global_name == 0) {
        return true;
    }
    uint32_t id = wayland_alloc_id(w);
    if (id == 0) {
        return false;
    }
    uint32_t ver = wl_min_u32(w->decoration_version, 2u);
    if (ver == 0) {
        ver = 1u;
    }
    // Prefer v2: a decoration object may be created after a buffer commit.
    // The toplevel path still creates it before the first commit, which v1 requires.
    if (!wl_append_registry_bind(w, w->decoration_global_name, "zxdg_decoration_manager_v1",
                                 ver, id)) {
        wl_release_unsent(w, id);
        return false;
    }
    w->decoration_manager_id = id;
    return true;
}

bool wl_request_client_decoration(WaylandContext *w) {
    if (w == nullptr || w->decoration_manager_id == 0 || w->xdg_toplevel_id == 0) {
        return true;
    }
    if (w->decoration_id != 0) {
        return true;
    }
    uint32_t mark = w->send_len;
    uint32_t id = wayland_alloc_id(w);
    if (id == 0) {
        return false;
    }
    uint32_t created[2] = { id, w->xdg_toplevel_id };
    uint32_t mode = ENG_DECOR_CLIENT;
    // get_toplevel_decoration is request 1. set_mode is request 1.
    // The id is encoded immediately, after every earlier id in this batch.
    if (!wayland_put_raw(w, w->decoration_manager_id, 1u, created, sizeof created) ||
        !wayland_put_raw(w, id, 1u, &mode, sizeof mode)) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, id);
        return false;
    }
    w->decoration_id = id;
    w->decoration_mode = ENG_DECOR_NONE;
    w->decoration_configured = false;
    return true;
}

void wl_on_decoration_event(WaylandContext *w, uint16_t opcode,
                            const unsigned char *p, const unsigned char *end) {
    if (w == nullptr || opcode != 0u) {
        return;
    }
    uint32_t mode = 0;
    if (!wl_take_u32(&p, end, &mode)) {
        wl_fail(w, "truncated zxdg_toplevel_decoration_v1.configure");
        return;
    }
    w->decoration_mode = mode;
    w->decoration_configured = true;
    if (mode == ENG_DECOR_SERVER && !w->decoration_server_noted) {
        w->decoration_server_noted = true;
        fprintf(stderr,
                "[Wayland] Compositor forced server-side decorations. Client-side was requested.\n");
    }
}
