// Object ids and request encoding.
#include "wayland/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert((ENG_FD_PARK & (ENG_FD_PARK - 1u)) == 0u, "ENG_FD_PARK must be a power of two");
static_assert((ENG_WL_SEND_RING_CAP & (ENG_WL_SEND_RING_CAP - 1u)) == 0u,
              "ENG_WL_SEND_RING_CAP must be a power of two");
static_assert((ENG_WL_SEND_PKT_CAP & (ENG_WL_SEND_PKT_CAP - 1u)) == 0u,
              "ENG_WL_SEND_PKT_CAP must be a power of two");


void wl_fail(WaylandContext *w, const char *msg) {
    snprintf(w->err, sizeof w->err, "%s", msg);
    w->failed = true;
}


void wayland_fail(WaylandContext *w, const char *msg) {
    wl_fail(w, msg);
}


void wl_fail_err(WaylandContext *w, const char *what, int err) {
    if (err < 0) {
        err = -err;
    }
    snprintf(w->err, sizeof w->err, "%s: %s", what, strerror(err));
    w->failed = true;
}


void wl_put_u16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }

void wl_put_u32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }


bool wl_take_u16(const unsigned char **p, const unsigned char *end, uint16_t *o) {
    if ((size_t)(end - *p) < 2u) return false;
    memcpy(o, *p, 2);
    *p += 2;
    return true;
}


bool wl_take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o) {
    if ((size_t)(end - *p) < 4u) return false;
    memcpy(o, *p, 4);
    *p += 4;
    return true;
}


bool wl_take_i32(const unsigned char **p, const unsigned char *end, int32_t *o) {
    if ((size_t)(end - *p) < 4u) return false;
    memcpy(o, *p, 4);
    *p += 4;
    return true;
}


uint32_t wl_min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static bool ensure_free_id_cap(WaylandContext *w) {
    if (w->free_ids == nullptr || w->free_cap == 0u) {
        w->free_ids = w->free_id_inline;
        w->free_cap = ENG_ID_FREE;
    }
    if (w->nfree < w->free_cap) {
        return true;
    }
    uint32_t new_cap = w->free_cap * 2u;
    if (new_cap <= w->free_cap) {
        return false;
    }
    uint32_t *grown = nullptr;
    if (w->free_ids == w->free_id_inline) {
        grown = (uint32_t *)malloc((size_t)new_cap * sizeof(uint32_t));
        if (grown == nullptr) {
            return false;
        }
        memcpy(grown, w->free_id_inline, (size_t)w->nfree * sizeof(uint32_t));
    } else {
        grown = (uint32_t *)realloc(w->free_ids, (size_t)new_cap * sizeof(uint32_t));
        if (grown == nullptr) {
            return false;
        }
    }
    w->free_ids = grown;
    w->free_cap = new_cap;
    return true;
}

uint32_t wayland_alloc_id(WaylandContext *w) {
    if (w == nullptr) {
        return 0;
    }
    if (w->free_ids == nullptr || w->free_cap == 0u) {
        w->free_ids = w->free_id_inline;
        w->free_cap = ENG_ID_FREE;
    }
    if (w->nfree > 0) {
        return w->free_ids[--w->nfree];
    }
    if (w->next_id == 0 || w->next_id > ENG_WL_CLIENT_ID_MAX) {
        wl_fail(w, "wayland client object ids exhausted (< 0xff000000)");
        return 0;
    }
    return w->next_id++;
}


static void clear_id_slot(WaylandContext *w, uint32_t id) {
    if (id == w->registry_id)           w->registry_id           = 0;
    if (id == w->sync_id)               w->sync_id               = 0;
    if (id == w->frame_cb_id)           w->frame_cb_id           = 0;
    if (id == w->bar_surface_id)        w->bar_surface_id        = 0;
    if (id == w->bar_subsurface_id)     w->bar_subsurface_id     = 0;
    if (id == w->bar_sync_id)           w->bar_sync_id           = 0;
    if (id == w->feedback_id)           w->feedback_id           = 0;
    if (id == w->retired_feedback_id)   w->retired_feedback_id   = 0;
    if (id == w->buffer_params_id)      w->buffer_params_id      = 0;
    if (id == w->buffer_id)             w->buffer_id             = 0;
    if (id == w->explicit_sync_id)      w->explicit_sync_id      = 0;
    if (id == w->acquire_timeline_id)   w->acquire_timeline_id   = 0;
    if (id == w->release_timeline_id)   w->release_timeline_id   = 0;
    if (id == w->viewport_id)           w->viewport_id           = 0;
    if (id == w->decoration_id)         w->decoration_id         = 0;
    if (id == w->decoration_manager_id) w->decoration_manager_id = 0;
    if (id == w->scene_opaque_id) {
        w->scene_opaque_id = 0;
        w->scene_opaque_w  = 0;
        w->scene_opaque_h  = 0;
    }
    if (id == w->bar_opaque_id) {
        w->bar_opaque_id = 0;
        w->bar_opaque_w  = 0;
        w->bar_opaque_h  = 0;
    }
}

static void push_free_id(WaylandContext *w, uint32_t id) {
    for (uint32_t i = 0; i < w->nfree; ++i) {
        if (w->free_ids[i] == id) {
            return;
        }
    }
    if (!ensure_free_id_cap(w)) {
        wl_fail(w, "wayland free-id stack out of memory");
        return;
    }
    w->free_ids[w->nfree++] = id;
}

void wl_free_id(WaylandContext *w, uint32_t id) {
    if (w == nullptr || id == 0 || id == w->display_id || id > ENG_WL_CLIENT_ID_MAX) {
        return;
    }
    clear_id_slot(w, id);
    push_free_id(w, id);
}

void wl_release_unsent(WaylandContext *w, uint32_t id) {
    if (w == nullptr || id == 0 || id == w->display_id || id > ENG_WL_CLIENT_ID_MAX) {
        return;
    }
    clear_id_slot(w, id);
    // This id is the newest fresh one and the compositor has not seen it.
    // Putting it on the free stack would let a later alloc send it out of order.
    if (id + 1u == w->next_id) {
        w->next_id = id;
        return;
    }
    push_free_id(w, id);
}


bool wayland_put_raw(WaylandContext *w, uint32_t obj_id, uint16_t opcode,
                     const void *payload, uint16_t payload_len) {
    uint32_t total = 8u + (uint32_t)payload_len;
    if ((total & 3u) != 0u || w->send_len + total > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding wayland request");
        return false;
    }
    unsigned char *dst = w->send_bytes + w->send_len;
    wl_put_u32(dst + 0, obj_id);
    wl_put_u16(dst + 4, opcode);
    wl_put_u16(dst + 6, (uint16_t)total);
    if (payload_len > 0 && payload != nullptr) {
        memcpy(dst + 8, payload, payload_len);
    }
    w->send_len += total;
    return true;
}


bool wl_append_string(WaylandContext *w, uint32_t obj_id, uint16_t opcode, const char *str) {
    uint32_t slen = (uint32_t)strlen(str) + 1u;
    uint32_t pad  = (4u - (slen & 3u)) & 3u;
    uint32_t size = 8u + 4u + slen + pad;
    if (w->send_len + size > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding string request");
        return false;
    }
    unsigned char *dst = w->send_bytes + w->send_len;
    wl_put_u32(dst + 0, obj_id);
    wl_put_u16(dst + 4, opcode);
    wl_put_u16(dst + 6, (uint16_t)size);
    wl_put_u32(dst + 8, slen);
    memcpy(dst + 12, str, slen);
    if (pad > 0) {
        memset(dst + 12 + slen, 0, pad);
    }
    w->send_len += size;
    return true;
}


void wayland_rewind_send(WaylandContext *w, uint32_t mark) {
    if (w == nullptr) {
        return;
    }
    if (mark < w->send_off) {
        mark = w->send_off;
    }
    w->send_len = mark;
}


bool wl_append_registry_bind(WaylandContext *w, uint32_t global_name,
                                 const char *iface, uint32_t version, uint32_t new_id) {
    uint32_t slen = (uint32_t)strlen(iface) + 1u;
    uint32_t pad  = (4u - (slen & 3u)) & 3u;
    uint32_t size = 8u + 4u + 4u + slen + pad + 4u + 4u;
    if (w->send_len + size > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding wl_registry.bind");
        return false;
    }

    unsigned char *dst = w->send_bytes + w->send_len;
    wl_put_u32(dst + 0, w->registry_id);
    wl_put_u16(dst + 4, 0u); // wl_registry.bind opcode = 0
    wl_put_u16(dst + 6, (uint16_t)size);
    wl_put_u32(dst + 8, global_name);
    wl_put_u32(dst + 12, slen);
    memcpy(dst + 16, iface, slen);
    if (pad > 0) {
        memset(dst + 16 + slen, 0, pad);
    }
    wl_put_u32(dst + 16 + slen + pad, version);
    wl_put_u32(dst + 20 + slen + pad, new_id);

    w->send_len += size;
    return true;
}


bool wl_append_display_sync(WaylandContext *w, uint32_t sync_id) {
    uint32_t payload = sync_id;
    return wayland_put_raw(w, w->display_id, 0u, &payload, sizeof(payload));
}

