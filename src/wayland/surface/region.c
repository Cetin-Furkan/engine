// One wl_region per surface. Later commits reuse it while the pixel size holds.
#include "wayland/internal.h"

bool wl_opaque_prepare(WaylandContext *w, uint32_t surface,
                       uint32_t *id, uint32_t *rw, uint32_t *rh,
                       uint32_t width, uint32_t height, uint32_t *fresh) {
    if (fresh != nullptr) {
        *fresh = 0;
    }
    if (w == nullptr || id == nullptr || rw == nullptr || rh == nullptr ||
        surface == 0 || width == 0 || height == 0 || w->compositor_id == 0) {
        return false;
    }
    // The committed region stays until a commit replaces it.
    if (*id != 0 && *rw == width && *rh == height) {
        return true;
    }
    if (*id != 0 && !wayland_put_raw(w, *id, 0u, nullptr, 0u)) {
        return false;
    }
    // Allocated at the encode point. A fresh id must be the next one the
    // compositor has not seen yet, and it has to be written before any higher id.
    uint32_t region = wayland_alloc_id(w);
    if (region == 0) {
        return false;
    }
    int32_t box[4] = { 0, 0, (int32_t)width, (int32_t)height };
    // wl_compositor.create_region is request 1. wl_region.add is request 1.
    // wl_surface.set_opaque_region is request 4. The object stays alive.
    if (!wayland_put_raw(w, w->compositor_id, 1u, &region, sizeof region) ||
        !wayland_put_raw(w, region, 1u, box, sizeof box) ||
        !wayland_put_raw(w, surface, 4u, &region, sizeof region)) {
        wl_release_unsent(w, region);
        return false;
    }
    if (fresh != nullptr) {
        *fresh = region;
    }
    return true;
}

void wl_opaque_keep(uint32_t *id, uint32_t *rw, uint32_t *rh,
                    uint32_t fresh, uint32_t width, uint32_t height) {
    if (id == nullptr || fresh == 0) {
        return;
    }
    *id = fresh;
    if (rw != nullptr) {
        *rw = width;
    }
    if (rh != nullptr) {
        *rh = height;
    }
}
