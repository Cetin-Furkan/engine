// DRM syncobj timelines on the wl_surface.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>


void wl_put_timeline_point(uint32_t out[3], uint32_t timeline, uint64_t point) {
    out[0] = timeline;
    out[1] = (uint32_t)(point >> 32u);
    out[2] = (uint32_t)point;
}


int wayland_import_syncobj_timeline(
    WaylandContext *w,
    int             syncobj_fd,
    uint32_t       *out_timeline_id
) {
    if (w == nullptr || !w->is_initialized || w->drm_syncobj_id == 0 ||
        syncobj_fd < 0 || out_timeline_id == nullptr) {
        return -EINVAL;
    }
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    uint32_t id = wayland_alloc_id(w);
    if (id == 0) {
        return -EIO;
    }
    // wp_linux_drm_syncobj_manager_v1.import_timeline (request 2). The fd is SCM_RIGHTS.
    if (!wayland_put_raw(w, w->drm_syncobj_id, 2u, &id, sizeof id)) {
        wl_release_unsent(w, id);
        fprintf(stderr, "[Wayland Error] syncobj import encode failed: %s\n", w->err);
        return -EIO;
    }
    *out_timeline_id = id;
    if (wl_finish_roundtrip(w, syncobj_fd) < 0) {
        fprintf(stderr, "[Wayland Error] syncobj import failed: %s\n", w->err);
        return -EIO;
    }
    return 0;
}


int wayland_bind_explicit_sync(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized || w->drm_syncobj_id == 0 || w->surface_id == 0) {
        return -EINVAL;
    }
    if (w->explicit_sync_id != 0) {
        return 0;
    }
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    uint32_t id = wayland_alloc_id(w);
    if (id == 0) {
        return -EIO;
    }
    uint32_t payload[2] = { id, w->surface_id };
    // wp_linux_drm_syncobj_manager_v1.get_surface (request 1)
    if (!wayland_put_raw(w, w->drm_syncobj_id, 1u, payload, sizeof payload)) {
        wl_release_unsent(w, id);
        fprintf(stderr, "[Wayland Error] explicit sync bind encode failed: %s\n", w->err);
        return -EIO;
    }
    w->explicit_sync_id = id;
    if (wl_finish_roundtrip(w, -1) < 0) {
        fprintf(stderr, "[Wayland Error] explicit sync bind failed: %s\n", w->err);
        return -EIO;
    }
    return 0;
}

