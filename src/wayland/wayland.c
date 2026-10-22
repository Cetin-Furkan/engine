#include "wayland/wayland.h"
#include "input/pointer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static constexpr uint64_t k_drm_format_mod_invalid = 0x00ffffffffffffffULL;
static WaylandContext s_wl_ctx = { .feedback_table_fd = -1 };

static void wl_fail(WaylandContext *w, const char *msg) {
    snprintf(w->err, sizeof w->err, "%s", msg);
    w->failed = true;
}

void wayland_fail(WaylandContext *w, const char *msg) {
    wl_fail(w, msg);
}

static void wl_fail_err(WaylandContext *w, const char *what, int err) {
    if (err < 0) {
        err = -err;
    }
    snprintf(w->err, sizeof w->err, "%s: %s", what, strerror(err));
    w->failed = true;
}

static void put_u16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }

static bool take_u16(const unsigned char **p, const unsigned char *end, uint16_t *o) {
    if ((size_t)(end - *p) < 2u) return false;
    memcpy(o, *p, 2);
    *p += 2;
    return true;
}

static bool take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o) {
    if ((size_t)(end - *p) < 4u) return false;
    memcpy(o, *p, 4);
    *p += 4;
    return true;
}

static bool take_i32(const unsigned char **p, const unsigned char *end, int32_t *o) {
    if ((size_t)(end - *p) < 4u) return false;
    memcpy(o, *p, 4);
    *p += 4;
    return true;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

uint32_t wayland_alloc_id(WaylandContext *w) {
    if (w->nfree > 0) {
        return w->free_id[--w->nfree];
    }
    if (w->next_id == 0) {
        wl_fail(w, "wayland object ids wrapped");
        return 0;
    }
    return w->next_id++;
}

static void free_id(WaylandContext *w, uint32_t id) {
    if (id == 0 || id == w->display_id) return;
    if (id == w->registry_id)         w->registry_id         = 0;
    if (id == w->sync_id)             w->sync_id             = 0;
    if (id == w->frame_cb_id)         w->frame_cb_id         = 0;
    if (id == w->feedback_id)         w->feedback_id         = 0;
    if (id == w->buffer_params_id)    w->buffer_params_id    = 0;
    if (id == w->explicit_sync_id)    w->explicit_sync_id    = 0;
    if (id == w->acquire_timeline_id) w->acquire_timeline_id = 0;
    if (id == w->release_timeline_id) w->release_timeline_id = 0;
    if (w->nfree >= ENG_ID_FREE) {
        wl_fail(w, "wayland free-id stack full");
        return;
    }
    w->free_id[w->nfree++] = id;
}

static bool take_parked_fd(WaylandContext *w, int *out_fd) {
    if (w->nparked == 0) {
        return false;
    }
    *out_fd = w->parked_fds[0];
    for (uint32_t i = 1; i < w->nparked; ++i) {
        w->parked_fds[i - 1] = w->parked_fds[i];
    }
    w->nparked--;
    return true;
}

static void close_feedback_table(WaylandContext *w) {
    if (w->feedback_table != nullptr && w->feedback_table != MAP_FAILED && w->feedback_table_bytes > 0) {
        munmap(w->feedback_table, w->feedback_table_bytes);
    }
    w->feedback_table       = nullptr;
    w->feedback_table_count = 0;
    w->feedback_table_bytes = 0;
    if (w->feedback_table_fd >= 0) {
        close(w->feedback_table_fd);
        w->feedback_table_fd = -1;
    }
}

static bool decode_dev_array(const unsigned char *bytes, uint32_t len,
                             int64_t *out_major, int64_t *out_minor) {
    if (len == 0 || len > sizeof(dev_t)) {
        return false;
    }
    dev_t dev = 0;
    memcpy(&dev, bytes, len);
    *out_major = (int64_t)major(dev);
    *out_minor = (int64_t)minor(dev);
    return true;
}

static void push_unique_modifier(uint64_t *list, uint32_t *count, uint32_t cap, uint64_t mod) {
    for (uint32_t i = 0; i < *count; ++i) {
        if (list[i] == mod) {
            return;
        }
    }
    if (*count < cap) {
        list[(*count)++] = mod;
    }
}

static bool tranche_device_is_gpu(const WaylandContext *w) {
    if (!w->tranche_has_dev) {
        return false;
    }
    if (w->tranche_major == w->feedback_target_major &&
        w->tranche_minor == w->feedback_target_minor) {
        return true;
    }
    // Primary and render nodes are different dev_t values for one GPU.
    // The protocol does not say which node a tranche advertises.
    if (w->feedback_has_alt &&
        w->tranche_major == w->feedback_alt_major &&
        w->tranche_minor == w->feedback_alt_minor) {
        return true;
    }
    return false;
}

static void commit_feedback_tranche(WaylandContext *w) {
    if (tranche_device_is_gpu(w) && w->feedback_table != nullptr) {
        const bool is_scanout = (w->tranche_flags & 1u) != 0u;
        for (uint32_t i = 0; i < w->tranche_index_count; ++i) {
            uint16_t idx = w->tranche_indices[i];
            if (idx >= w->feedback_table_count) {
                continue;
            }
            const WaylandFormatTableEntry *entry = &w->feedback_table[idx];
            if (entry->format != ENG_DRM_FORMAT_ARGB8888 ||
                entry->modifier == k_drm_format_mod_invalid) {
                continue;
            }
            if (is_scanout) {
                push_unique_modifier(w->scanout_modifiers, &w->scanout_modifier_count,
                                     ENG_WL_MAX_MODIFIERS, entry->modifier);
            }
            push_unique_modifier(w->all_modifiers, &w->all_modifier_count,
                                 ENG_WL_MAX_MODIFIERS, entry->modifier);
        }
    }
    w->tranche_has_dev     = false;
    w->tranche_major       = 0;
    w->tranche_minor       = 0;
    w->tranche_flags       = 0;
    w->tranche_index_count = 0;
}

bool wayland_put_raw(WaylandContext *w, uint32_t obj_id, uint16_t opcode,
                     const void *payload, uint16_t payload_len) {
    uint32_t total = 8u + (uint32_t)payload_len;
    if ((total & 3u) != 0u || w->send_len + total > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding wayland request");
        return false;
    }
    unsigned char *dst = w->send_bytes + w->send_len;
    put_u32(dst + 0, obj_id);
    put_u16(dst + 4, opcode);
    put_u16(dst + 6, (uint16_t)total);
    if (payload_len > 0 && payload != nullptr) {
        memcpy(dst + 8, payload, payload_len);
    }
    w->send_len += total;
    return true;
}

static bool append_string_req(WaylandContext *w, uint32_t obj_id, uint16_t opcode, const char *str) {
    uint32_t slen = (uint32_t)strlen(str) + 1u;
    uint32_t pad  = (4u - (slen & 3u)) & 3u;
    uint32_t size = 8u + 4u + slen + pad;
    if (w->send_len + size > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding string request");
        return false;
    }
    unsigned char *dst = w->send_bytes + w->send_len;
    put_u32(dst + 0, obj_id);
    put_u16(dst + 4, opcode);
    put_u16(dst + 6, (uint16_t)size);
    put_u32(dst + 8, slen);
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

static bool append_registry_bind(WaylandContext *w, uint32_t global_name,
                                 const char *iface, uint32_t version, uint32_t new_id) {
    uint32_t slen = (uint32_t)strlen(iface) + 1u;
    uint32_t pad  = (4u - (slen & 3u)) & 3u;
    uint32_t size = 8u + 4u + 4u + slen + pad + 4u + 4u;
    if (w->send_len + size > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding wl_registry.bind");
        return false;
    }

    unsigned char *dst = w->send_bytes + w->send_len;
    put_u32(dst + 0, w->registry_id);
    put_u16(dst + 4, 0u); // wl_registry.bind opcode = 0
    put_u16(dst + 6, (uint16_t)size);
    put_u32(dst + 8, global_name);
    put_u32(dst + 12, slen);
    memcpy(dst + 16, iface, slen);
    if (pad > 0) {
        memset(dst + 16 + slen, 0, pad);
    }
    put_u32(dst + 16 + slen + pad, version);
    put_u32(dst + 20 + slen + pad, new_id);

    w->send_len += size;
    return true;
}

static bool append_display_sync(WaylandContext *w, uint32_t sync_id) {
    uint32_t payload = sync_id;
    return wayland_put_raw(w, w->display_id, 0u, &payload, sizeof(payload));
}

static int arm_pong_send(WaylandContext *w) {
    if (w->pong_off > 12u) {
        return -EIO;
    }
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }
    sqe->opcode    = IORING_OP_SEND;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)(w->pong_bytes + w->pong_off);
    sqe->len       = 12u - w->pong_off;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_PONG;
    w->pong_in_flight = true;
    return 0;
}

static void queue_xdg_pong(WaylandContext *w, uint32_t serial) {
    if (w->xdg_wm_base_id == 0) {
        return;
    }
    if (w->pong_in_flight) {
        wl_fail(w, "xdg_wm_base.ping received while pong is already in flight");
        return;
    }
    put_u32(w->pong_bytes + 0, w->xdg_wm_base_id);
    put_u16(w->pong_bytes + 4, 3u); // xdg_wm_base.pong opcode = 3
    put_u16(w->pong_bytes + 6, 12u);
    put_u32(w->pong_bytes + 8, serial);
    w->pong_off = 0;
    if (arm_pong_send(w) != 0) {
        wl_fail(w, "SQ full while queuing xdg_wm_base.pong");
    }
}

static void park_fds(WaylandContext *w, const unsigned char *control, uint32_t controllen) {
    struct msghdr msg = {
        .msg_control    = (void *)control,
        .msg_controllen = controllen,
    };
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) continue;
        int n = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *fds = (int *)CMSG_DATA(cmsg);
        for (int i = 0; i < n; ++i) {
            if (w->nparked >= ENG_FD_PARK) {
                close(fds[i]);
                wl_fail(w, "SCM_RIGHTS fd park overflow");
                continue;
            }
            w->parked_fds[w->nparked++] = fds[i];
        }
    }
}

static void note_key_global(WaylandContext *w, uint32_t name, const char *iface, uint32_t version) {
    if (strcmp(iface, "wl_compositor") == 0) {
        w->compositor_global_name = name;
        w->compositor_version     = version;
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        w->xdg_wm_base_global_name = name;
        w->xdg_wm_base_version     = version;
    } else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
        w->linux_dmabuf_global_name = name;
        w->linux_dmabuf_version     = version;
    } else if (strcmp(iface, "wp_linux_drm_syncobj_manager_v1") == 0) {
        w->drm_syncobj_global_name = name;
        w->drm_syncobj_version     = version;
    } else if (strcmp(iface, "wl_seat") == 0 && w->seat_global_name == 0) {
        w->seat_global_name = name;
        w->seat_version     = version;
    } else if (strcmp(iface, "wl_shm") == 0 && w->shm_global_name == 0) {
        w->shm_global_name = name;
        w->shm_version     = version;
    } else if (strcmp(iface, "wp_cursor_shape_manager_v1") == 0 && w->cursor_shape_global_name == 0) {
        w->cursor_shape_global_name = name;
        w->cursor_shape_version     = version;
    } else if (strcmp(iface, "wp_viewporter") == 0 && w->viewporter_global_name == 0) {
        w->viewporter_global_name = name;
        w->viewporter_version     = version;
    }
}

static void parse_stream(WaylandContext *w) {
    uint32_t offset = 0;
    const unsigned char *stream_end = w->stream + w->stream_len;
    while (w->stream_len - offset >= 8u && !w->failed) {
        const unsigned char *msg_start = w->stream + offset;
        const unsigned char *p         = msg_start;
        uint32_t object = 0;
        uint16_t opcode = 0;
        uint16_t size   = 0;

        if (!take_u32(&p, stream_end, &object) ||
            !take_u16(&p, stream_end, &opcode) ||
            !take_u16(&p, stream_end, &size)) {
            break;
        }
        if (size < 8u || (size & 3u) != 0u) {
            wl_fail(w, "invalid wayland wire message size");
            break;
        }
        if (w->stream_len - offset < size) {
            break;
        }

        const unsigned char *end = msg_start + size;
        p = msg_start + 8;

        if (object == w->display_id && opcode == 0u) {
            uint32_t bad_id = 0, code = 0, slen = 0;
            if (!take_u32(&p, end, &bad_id) || !take_u32(&p, end, &code) ||
                !take_u32(&p, end, &slen) || slen == 0 || p + slen > end) {
                wl_fail(w, "truncated wl_display.error");
                break;
            }
            snprintf(w->err, sizeof w->err, "wl_display.error object %u code %u: %.*s",
                     bad_id, code, (int)(slen - 1u), (const char *)p);
            w->failed = true;
            break;
        } else if (object == w->display_id && opcode == 1u) {
            uint32_t id = 0;
            if (!take_u32(&p, end, &id)) {
                wl_fail(w, "truncated wl_display.delete_id");
                break;
            }
            free_id(w, id);
        } else if (w->registry_id != 0 && object == w->registry_id && opcode == 0u) {
            uint32_t name = 0, slen = 0, version = 0;
            if (!take_u32(&p, end, &name) || !take_u32(&p, end, &slen) ||
                slen == 0 || slen > ENG_WL_NAME || p + slen > end || p[slen - 1] != 0) {
                wl_fail(w, "invalid wl_registry.global");
                break;
            }
            const char *iface = (const char *)p;
            uint32_t pad = (4u - (slen & 3u)) & 3u;
            p += slen + pad;
            if (!take_u32(&p, end, &version) || p != end) {
                wl_fail(w, "wl_registry.global size mismatch");
                break;
            }
            w->globals_seen++;
            if (w->nglobals < ENG_WL_GLOBALS) {
                WaylandGlobal *g = &w->globals[w->nglobals++];
                g->name    = name;
                g->version = version;
                memcpy(g->interface, iface, slen);
            }
            note_key_global(w, name, iface, version);
        } else if (w->xdg_wm_base_id != 0 && object == w->xdg_wm_base_id && opcode == 0u) {
            uint32_t serial = 0;
            if (!take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated xdg_wm_base.ping");
                break;
            }
            queue_xdg_pong(w, serial);
        } else if (w->sync_id != 0 && object == w->sync_id && opcode == 0u) {
            uint32_t serial = 0;
            if (size != 12u || !take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated sync wl_callback.done");
                break;
            }
            w->synced = true;
        } else if (w->frame_cb_id != 0 && object == w->frame_cb_id && opcode == 0u) {
            uint32_t timestamp_ms = 0;
            if (size != 12u || !take_u32(&p, end, &timestamp_ms)) {
                wl_fail(w, "truncated frame wl_callback.done");
                break;
            }
            w->frame_time_ms = timestamp_ms;
            w->frame_stamp_valid = true;
            w->frame_done = true;
            // delete_id returns this id. Clear the slot so the next commit can arm one.
            w->frame_cb_id = 0;
        } else if (w->xdg_toplevel_id != 0 && object == w->xdg_toplevel_id) {
            if (opcode == 0u) {
                int32_t width = 0, height = 0;
                uint32_t states_len = 0;
                if (!take_i32(&p, end, &width) || !take_i32(&p, end, &height) ||
                    !take_u32(&p, end, &states_len)) {
                    wl_fail(w, "truncated xdg_toplevel.configure");
                    break;
                }
                uint32_t pad = (4u - (states_len & 3u)) & 3u;
                if ((size_t)(end - p) < (size_t)states_len + pad) {
                    wl_fail(w, "xdg_toplevel.configure state array out of bounds");
                    break;
                }
                bool resizing = false;
                uint32_t left = states_len;
                const unsigned char *sp = p;
                while (left >= 4u) {
                    uint32_t st = 0;
                    memcpy(&st, sp, 4);
                    if (st == 3u) {
                        resizing = true;
                    }
                    sp += 4;
                    left -= 4u;
                }
                w->configure_resizing  = resizing;
                w->configure_width     = (width > 0) ? (uint32_t)width : 0u;
                w->configure_height    = (height > 0) ? (uint32_t)height : 0u;
                w->toplevel_configured = true;
            } else if (opcode == 1u) {
                // The client owns the exit. Close is not a protocol failure.
                w->toplevel_closed = true;
            }
        } else if (w->xdg_surface_id != 0 && object == w->xdg_surface_id && opcode == 0u) {
            uint32_t serial = 0;
            if (!take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated xdg_surface.configure");
                break;
            }
            w->configure_serial       = serial;
            w->xdg_surface_configured = true;
        } else if (w->feedback_id != 0 && object == w->feedback_id) {
            if (opcode == 0u) {
                w->feedback_done = true;
            } else if (opcode == 1u) {
                // format_table is an fd plus a uint byte length. The fd is not in the body.
                uint32_t table_size = 0;
                int fd = -1;
                if (size < 12u || !take_u32(&p, end, &table_size) || !take_parked_fd(w, &fd)) {
                    wl_fail(w, "zwp_linux_dmabuf_feedback_v1.format_table missing size or fd");
                    break;
                }
                struct stat st;
                if (fstat(fd, &st) != 0) {
                    int err = errno;
                    close(fd);
                    wl_fail_err(w, "fstat dmabuf format_table", err);
                    break;
                }
                if (table_size == 0u ||
                    (table_size % (uint32_t)sizeof(WaylandFormatTableEntry)) != 0u ||
                    st.st_size < (off_t)table_size) {
                    close(fd);
                    wl_fail(w, "zwp_linux_dmabuf_feedback_v1.format_table bad length");
                    break;
                }
                void *map = mmap(nullptr, table_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (map == MAP_FAILED) {
                    int err = errno;
                    close(fd);
                    wl_fail_err(w, "mmap dmabuf format_table", err);
                    break;
                }
                close_feedback_table(w);
                w->feedback_table_fd    = fd;
                w->feedback_table       = (WaylandFormatTableEntry *)map;
                w->feedback_table_bytes = table_size;
                w->feedback_table_count = table_size / (uint32_t)sizeof(WaylandFormatTableEntry);
            } else if (opcode == 2u) {
                uint32_t arr_len = 0;
                if (!take_u32(&p, end, &arr_len) || (size_t)(end - p) < arr_len) {
                    wl_fail(w, "truncated zwp_linux_dmabuf_feedback_v1.main_device");
                    break;
                }
                if (decode_dev_array(p, arr_len, &w->feedback_main_major, &w->feedback_main_minor)) {
                    w->feedback_has_main_dev = true;
                }
            } else if (opcode == 3u) {
                commit_feedback_tranche(w);
            } else if (opcode == 4u) {
                uint32_t arr_len = 0;
                if (!take_u32(&p, end, &arr_len) || (size_t)(end - p) < arr_len) {
                    wl_fail(w, "truncated zwp_linux_dmabuf_feedback_v1.tranche_target_device");
                    break;
                }
                w->tranche_has_dev = decode_dev_array(
                    p, arr_len, &w->tranche_major, &w->tranche_minor);
            } else if (opcode == 5u) {
                uint32_t arr_len = 0;
                if (!take_u32(&p, end, &arr_len) || (size_t)(end - p) < arr_len || (arr_len & 1u) != 0u) {
                    wl_fail(w, "invalid zwp_linux_dmabuf_feedback_v1.tranche_formats");
                    break;
                }
                uint32_t count = arr_len / 2u;
                if (w->tranche_index_count > ENG_WL_MAX_INDICES ||
                    count > ENG_WL_MAX_INDICES - w->tranche_index_count) {
                    wl_fail(w, "zwp_linux_dmabuf_feedback_v1 tranche index list exceeds capacity");
                    break;
                }
                memcpy(w->tranche_indices + w->tranche_index_count, p, count * sizeof(uint16_t));
                w->tranche_index_count += count;
            } else if (opcode == 6u) {
                uint32_t flags = 0;
                if (!take_u32(&p, end, &flags)) {
                    wl_fail(w, "truncated zwp_linux_dmabuf_feedback_v1.tranche_flags");
                    break;
                }
                w->tranche_flags = flags;
            }
        } else if (w->buffer_params_id != 0 && object == w->buffer_params_id && opcode == 1u) {
            wl_fail(w, "zwp_linux_buffer_params_v1.failed received from compositor");
            break;
        } else if (w->buffer_id != 0 && object == w->buffer_id && opcode == 0u) {
            w->buffer_released = true;
        } else {
            (void)pointer_dispatch(w, object, opcode, msg_start + 8, (uint16_t)(size - 8u));
        }

        offset += size;
    }

    if (offset > 0) {
        uint32_t remaining = w->stream_len - offset;
        if (remaining > 0) {
            memmove(w->stream, w->stream + offset, remaining);
        }
        w->stream_len = remaining;
    }
}

static void arm_recv(WaylandContext *w) {
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while arming multishot recvmsg");
        return;
    }
    sqe->opcode    = IORING_OP_RECVMSG;
    sqe->flags     = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->ioprio    = IORING_RECV_MULTISHOT;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)&w->recv_msg;
    sqe->msg_flags = MSG_CMSG_CLOEXEC;
    sqe->buf_group = ENG_PBUF_BGID;
    sqe->user_data = ENG_UD_WL_RECV;
    w->recv_armed  = true;
}

int wayland_ensure_recv(WaylandContext *w) {
    if (w->failed) return -EIO;
    if (w->recv_msg.msg_iov == nullptr) return -EINVAL;
    if (w->recv_armed) return 0;
    arm_recv(w);
    return w->failed ? -EIO : 0;
}

void wayland_on_cqe(WaylandContext *w, const struct io_uring_cqe *cqe) {
    if (cqe->user_data == ENG_UD_SIGNAL) {
        w->noted_signal = true;
        return;
    }
    if (cqe->user_data == ENG_UD_WL_PONG) {
        w->pong_in_flight = false;
        uint32_t left = (w->pong_off < 12u) ? (12u - w->pong_off) : 0u;
        if (cqe->res == -EAGAIN || cqe->res == -EINTR || cqe->res == 0) {
            if (arm_pong_send(w) != 0) {
                wl_fail(w, "SQ full while retrying xdg_wm_base.pong");
            }
            return;
        }
        if (cqe->res < 0 || (uint32_t)cqe->res > left) {
            wl_fail_err(w, "xdg_wm_base.pong send", cqe->res);
            return;
        }
        w->pong_off += (uint32_t)cqe->res;
        if (w->pong_off < 12u) {
            if (arm_pong_send(w) != 0) {
                wl_fail(w, "SQ full while retrying xdg_wm_base.pong");
            }
            return;
        }
        w->pong_off = 0;
        return;
    }
    if (cqe->user_data != ENG_UD_WL_RECV) {
        return;
    }
    w->recv_cqes++;
    w->recv_armed = (cqe->flags & IORING_CQE_F_MORE) != 0;

    if (cqe->res < 0) {
        w->recv_armed = false;
        if (cqe->res == -ENOBUFS) {
            (void)wayland_ensure_recv(w);
            return;
        }
        wl_fail_err(w, "recvmsg", cqe->res);
        return;
    }
    if ((cqe->flags & IORING_CQE_F_BUFFER) == 0) {
        wl_fail(w, "recvmsg CQE missing buffer ID");
        return;
    }

    uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    uint8_t *buf = ring_buf(w->ring, bid);
    if (buf == nullptr) {
        wl_fail(w, "recvmsg buffer ID out of range");
        return;
    }

    const struct io_uring_recvmsg_out *out = (const struct io_uring_recvmsg_out *)buf;
    uint32_t hdr = (uint32_t)sizeof(*out) + w->control_reserve;
    if ((uint32_t)cqe->res < hdr || (out->flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
        ring_pbuf_recycle(w->ring, bid);
        wl_fail(w, "recvmsg truncated");
        return;
    }

    uint32_t copied = (uint32_t)cqe->res - hdr;
    if (out->controllen > 0) {
        park_fds(w, buf + sizeof(*out), out->controllen);
    }
    if (!w->failed && w->stream_len + copied <= sizeof(w->stream)) {
        memcpy(w->stream + w->stream_len, buf + hdr, copied);
        w->stream_len += copied;
        ring_pbuf_recycle(w->ring, bid);
        parse_stream(w);
    } else {
        ring_pbuf_recycle(w->ring, bid);
        wl_fail(w, "wayland stream buffer overflow");
    }

    if (!w->failed) {
        (void)wayland_ensure_recv(w);
    }
}

static int submit_and_wait_tag(WaylandContext *w, uint64_t tag, struct io_uring_cqe *out) {
    uint64_t deadline = ring_mono_ns() + 3000000000ull;
    for (;;) {
        struct io_uring_cqe cqe;
        while (ring_peek(w->ring, &cqe)) {
            if (cqe.user_data == tag) {
                *out = cqe;
                return 0;
            }
            wayland_on_cqe(w, &cqe);
            if (w->failed) return -EIO;
        }
        if (ring_mono_ns() > deadline) {
            wl_fail(w, "wayland io_uring operation timed out");
            return -ETIMEDOUT;
        }
        if (w->recv_msg.msg_iov != nullptr && !w->recv_armed) {
            (void)wayland_ensure_recv(w);
        }
        unsigned n = ring_submit(w->ring);
        int rc = ring_enter(w->ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME) {
            wl_fail_err(w, "io_uring_enter", rc);
            return rc;
        }
    }
}

static int note_send_progress(WaylandContext *w, int32_t res) {
    if (w->send_off > w->send_len) {
        wl_fail(w, "wayland send cursor past the buffer");
        w->send_len = 0;
        w->send_off = 0;
        w->send_tracks_ack = false;
        return -EIO;
    }
    uint32_t remaining = w->send_len - w->send_off;
    if (remaining == 0) {
        w->send_len = 0;
        w->send_off = 0;
        return 0;
    }
    if (res < 0) {
        if (res == -EAGAIN || res == -EINTR) {
            return 1;
        }
        wl_fail_err(w, "wayland send", res);
        w->send_len = 0;
        w->send_off = 0;
        w->send_tracks_ack = false;
        return -EIO;
    }
    if ((uint32_t)res > remaining) {
        wl_fail(w, "wayland send wrote past the buffer");
        w->send_len = 0;
        w->send_off = 0;
        w->send_tracks_ack = false;
        return -EIO;
    }
    if (res == 0) {
        return 1;
    }
    w->send_off += (uint32_t)res;
    if (w->send_off < w->send_len) {
        return 1;
    }
    w->send_len = 0;
    w->send_off = 0;
    return 0;
}

static int send_wire_batch_with_fd(WaylandContext *w, int fd_to_send) {
    if (w->send_off >= w->send_len) {
        w->send_len = 0;
        w->send_off = 0;
        return 0;
    }
    if (w->failed) {
        return -EIO;
    }
    int dup_fd = -1;
    if (fd_to_send >= 0) {
        dup_fd = dup(fd_to_send);
        if (dup_fd < 0) {
            wl_fail_err(w, "dup SCM_RIGHTS fd", errno);
            return -errno;
        }
    }

    for (;;) {
        uint32_t remaining = w->send_len - w->send_off;
        struct io_uring_sqe *sqe = ring_sqe(w->ring);
        if (sqe == nullptr) {
            if (dup_fd >= 0) {
                close(dup_fd);
            }
            wl_fail(w, "SQ full while sending wire batch");
            return -ENOSPC;
        }

        if (dup_fd >= 0) {
            w->send_iov.iov_base   = w->send_bytes + w->send_off;
            w->send_iov.iov_len    = remaining;
            w->send_msg.msg_iov    = &w->send_iov;
            w->send_msg.msg_iovlen = 1u;

            memset(w->send_control, 0, sizeof(w->send_control));
            w->send_msg.msg_control    = w->send_control;
            w->send_msg.msg_controllen = sizeof(w->send_control);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&w->send_msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;
            cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &dup_fd, sizeof(int));

            sqe->opcode    = IORING_OP_SENDMSG;
            sqe->flags     = IOSQE_FIXED_FILE;
            sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
            sqe->fd        = ENG_WL_SOCK_SLOT;
            sqe->addr      = (uint64_t)(uintptr_t)&w->send_msg;
            sqe->msg_flags = MSG_NOSIGNAL;
            sqe->user_data = ENG_UD_WL_SEND;
        } else {
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;

            sqe->opcode    = IORING_OP_SEND;
            sqe->flags     = IOSQE_FIXED_FILE;
            sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
            sqe->fd        = ENG_WL_SOCK_SLOT;
            sqe->addr      = (uint64_t)(uintptr_t)(w->send_bytes + w->send_off);
            sqe->len       = remaining;
            sqe->msg_flags = MSG_NOSIGNAL;
            sqe->user_data = ENG_UD_WL_SEND;
        }

        struct io_uring_cqe cqe = {0};
        int rc = submit_and_wait_tag(w, ENG_UD_WL_SEND, &cqe);
        if (rc < 0) {
            if (dup_fd >= 0) {
                close(dup_fd);
            }
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            return rc;
        }
        if (cqe.res > 0 && dup_fd >= 0) {
            close(dup_fd);
            dup_fd = -1;
        }
        int progress = note_send_progress(w, cqe.res);
        if (progress < 0) {
            if (dup_fd >= 0) {
                close(dup_fd);
            }
            w->send_msg.msg_control    = nullptr;
            w->send_msg.msg_controllen = 0u;
            return progress;
        }
        if (progress == 0) {
            break;
        }
    }

    if (dup_fd >= 0) {
        close(dup_fd);
    }
    w->send_msg.msg_control    = nullptr;
    w->send_msg.msg_controllen = 0u;
    return 0;
}

static int send_wire_batch(WaylandContext *w) {
    return send_wire_batch_with_fd(w, -1);
}

static int flush_if_pending(WaylandContext *w) {
    if (w->send_len == 0) {
        return 0;
    }
    return send_wire_batch(w);
}

int wayland_send_batch(WaylandContext *w) {
    return send_wire_batch(w);
}

static int finish_batch_and_roundtrip(WaylandContext *w, int fd_to_send);

int wayland_roundtrip_batch(WaylandContext *w, int fd_to_send) {
    return finish_batch_and_roundtrip(w, fd_to_send);
}

static bool put_toplevel_min_size(WaylandContext *w, int32_t width, int32_t height) {
    if (w->xdg_toplevel_id == 0 || width <= 0 || height <= 0) {
        return false;
    }
    int32_t sz[2] = { width, height };
    // xdg_toplevel.set_min_size is request 8. A zero argument means no bound.
    return wayland_put_raw(w, w->xdg_toplevel_id, 8u, sz, sizeof sz);
}

int wayland_create_toplevel_and_configure(
    WaylandContext *w,
    const char     *title,
    const char     *app_id,
    uint32_t       *out_width,
    uint32_t       *out_height,
    uint32_t       *out_serial
) {
    if (w == nullptr || !w->is_initialized || out_width == nullptr ||
        out_height == nullptr || out_serial == nullptr) {
        return -EINVAL;
    }
    if (flush_if_pending(w) < 0) {
        return -EIO;
    }
    w->surface_id             = wayland_alloc_id(w);
    w->xdg_surface_id         = wayland_alloc_id(w);
    w->xdg_toplevel_id        = wayland_alloc_id(w);
    w->toplevel_configured    = false;
    w->xdg_surface_configured = false;
    w->configure_width        = 0;
    w->configure_height       = 0;
    w->configure_serial       = 0;

    // 1. wl_compositor.create_surface (request 0)
    (void)wayland_put_raw(w, w->compositor_id, 0u, &w->surface_id, sizeof(w->surface_id));

    // 2. xdg_wm_base.get_xdg_surface (request 2)
    uint32_t get_xdg_payload[2] = { w->xdg_surface_id, w->surface_id };
    (void)wayland_put_raw(w, w->xdg_wm_base_id, 2u, get_xdg_payload, sizeof(get_xdg_payload));

    // 3. xdg_surface.get_toplevel (request 1)
    (void)wayland_put_raw(w, w->xdg_surface_id, 1u, &w->xdg_toplevel_id, sizeof(w->xdg_toplevel_id));

    // 4. xdg_toplevel.set_title (request 2) and set_app_id (request 3)
    (void)append_string_req(w, w->xdg_toplevel_id, 2u, title != nullptr ? title : "Antigravity");
    (void)append_string_req(w, w->xdg_toplevel_id, 3u, app_id != nullptr ? app_id : "antigravity");

    if (w->viewporter_id != 0) {
        w->viewport_id = wayland_alloc_id(w);
        uint32_t viewport_payload[2] = { w->viewport_id, w->surface_id };
        // wp_viewporter.get_viewport is request 1.
        if (w->viewport_id == 0 ||
            !wayland_put_raw(w, w->viewporter_id, 1u, viewport_payload, sizeof viewport_payload)) {
            w->viewport_id = 0;
        }
    }

    if (w->min_width > 0 && w->min_height > 0 &&
        !put_toplevel_min_size(w, w->min_width, w->min_height)) {
        fprintf(stderr, "[Wayland Error] Toplevel minimum encode failed: %s\n", w->err);
        return -EIO;
    }

    // 5. wl_surface.commit (request 6) with empty body
    (void)wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);

    if (send_wire_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Failed to send toplevel creation batch: %s\n", w->err);
        return -EIO;
    }

    struct io_uring_cqe cqe = {0};
    while ((!w->toplevel_configured || !w->xdg_surface_configured) && !w->failed) {
        if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->toplevel_configured || !w->xdg_surface_configured) {
        fprintf(stderr, "[Wayland Error] Toplevel configure wait failed: %s\n", w->err);
        return -EIO;
    }

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }

    *out_width  = (w->configure_width > 0 && w->configure_height > 0) ? w->configure_width  : 1280u;
    *out_height = (w->configure_width > 0 && w->configure_height > 0) ? w->configure_height : 720u;
    *out_serial = w->configure_serial;
    return 0;
}

int wayland_query_surface_dmabuf_feedback(
    WaylandContext *w,
    int64_t         target_major,
    int64_t         target_minor,
    int64_t         alt_major,
    int64_t         alt_minor,
    bool            has_alt
) {
    if (w == nullptr || !w->is_initialized || w->surface_id == 0) {
        return -EINVAL;
    }
    if (w->linux_dmabuf_bound_version < 4u) {
        fprintf(stderr,
                "[Wayland Error] zwp_linux_dmabuf_v1 is bound at v%u, but surface feedback requires v4+\n",
                w->linux_dmabuf_bound_version);
        return -ENOTSUP;
    }
    if (flush_if_pending(w) < 0) {
        return -EIO;
    }
    close_feedback_table(w);
    w->feedback_id            = wayland_alloc_id(w);
    w->feedback_target_major  = target_major;
    w->feedback_target_minor  = target_minor;
    w->feedback_alt_major     = alt_major;
    w->feedback_alt_minor     = alt_minor;
    w->feedback_has_alt       = has_alt;
    w->feedback_has_main_dev  = false;
    w->feedback_done          = false;
    w->tranche_has_dev        = false;
    w->tranche_flags          = 0;
    w->tranche_index_count    = 0;
    w->scanout_modifier_count = 0;
    w->all_modifier_count     = 0;

    // zwp_linux_dmabuf_v1.get_surface_feedback (request 3)
    uint32_t fb_payload[2] = { w->feedback_id, w->surface_id };
    (void)wayland_put_raw(w, w->linux_dmabuf_id, 3u, fb_payload, sizeof(fb_payload));

    if (send_wire_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] get_surface_feedback send failed: %s\n", w->err);
        return -EIO;
    }

    struct io_uring_cqe cqe = {0};
    while (!w->feedback_done && !w->failed) {
        if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }

    // Destroy feedback object (request 0) and release mapped format table
    if (!w->failed && w->feedback_id != 0) {
        (void)wayland_put_raw(w, w->feedback_id, 0u, nullptr, 0u);
        (void)send_wire_batch(w);
    }
    close_feedback_table(w);

    if (w->failed || !w->feedback_done) {
        fprintf(stderr, "[Wayland Error] Surface DMA-BUF feedback failed: %s\n", w->err);
        return -EIO;
    }

    if (w->all_modifier_count == 0) {
        fprintf(stderr,
                "[Wayland Error] No DRM_FORMAT_ARGB8888 modifiers offered for DRM node %lld:%lld",
                (long long)target_major, (long long)target_minor);
        if (has_alt) {
            fprintf(stderr, " or %lld:%lld", (long long)alt_major, (long long)alt_minor);
        }
        fprintf(stderr, "\n");
        return -ENOENT;
    }

    return 0;
}

int wayland_import_dmabuf_immed(
    WaylandContext *w,
    int             dmabuf_fd,
    uint32_t        width,
    uint32_t        height,
    uint32_t        stride,
    uint32_t        offset,
    uint32_t        drm_fourcc,
    uint64_t        modifier,
    uint32_t       *out_buffer_id
) {
    if (w == nullptr || !w->is_initialized || dmabuf_fd < 0 || out_buffer_id == nullptr) {
        return -EINVAL;
    }
    if (flush_if_pending(w) < 0) {
        return -EIO;
    }
    w->buffer_params_id = wayland_alloc_id(w);
    w->buffer_id        = wayland_alloc_id(w);
    w->buffer_released  = false;

    // 1. zwp_linux_dmabuf_v1.create_params (request 1)
    (void)wayland_put_raw(w, w->linux_dmabuf_id, 1u, &w->buffer_params_id, sizeof(w->buffer_params_id));

    // 2. zwp_linux_buffer_params_v1.add (request 1, carries 1 SCM_RIGHTS fd)
    uint32_t add_payload[5] = {
        0u,                                 // plane_idx = 0
        offset,                             // offset = 0
        stride,                             // stride = row_pitch
        (uint32_t)(modifier >> 32u),        // modifier_hi
        (uint32_t)(modifier & 0xffffffffu), // modifier_lo
    };
    (void)wayland_put_raw(w, w->buffer_params_id, 1u, add_payload, sizeof(add_payload));

    // 3. zwp_linux_buffer_params_v1.create_immed (request 3)
    uint32_t immed_payload[5] = {
        w->buffer_id,
        width,
        height,
        drm_fourcc,
        0u, // flags = 0
    };
    (void)wayland_put_raw(w, w->buffer_params_id, 3u, immed_payload, sizeof(immed_payload));

    // 4. zwp_linux_buffer_params_v1.destroy (request 0)
    (void)wayland_put_raw(w, w->buffer_params_id, 0u, nullptr, 0u);

    if (send_wire_batch_with_fd(w, dmabuf_fd) < 0) {
        fprintf(stderr, "[Wayland Error] DMA-BUF create_immed batch failed: %s\n", w->err);
        return -EIO;
    }

    *out_buffer_id = w->buffer_id;
    return 0;
}

static int finish_batch_and_roundtrip(WaylandContext *w, int fd_to_send) {
    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    if (w->sync_id == 0 || !append_display_sync(w, w->sync_id)) {
        return -EIO;
    }
    if (send_wire_batch_with_fd(w, fd_to_send) < 0) {
        return -EIO;
    }
    struct io_uring_cqe cqe = {0};
    while (!w->synced && !w->failed) {
        if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }
    if (w->failed || !w->synced) {
        return -EIO;
    }
    return 0;
}

static void put_timeline_point(uint32_t out[3], uint32_t timeline, uint64_t point) {
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
    if (flush_if_pending(w) < 0) {
        return -EIO;
    }
    uint32_t id = wayland_alloc_id(w);
    if (id == 0) {
        return -EIO;
    }
    // wp_linux_drm_syncobj_manager_v1.import_timeline (request 2). The fd is SCM_RIGHTS.
    if (!wayland_put_raw(w, w->drm_syncobj_id, 2u, &id, sizeof id)) {
        fprintf(stderr, "[Wayland Error] syncobj import encode failed: %s\n", w->err);
        return -EIO;
    }
    *out_timeline_id = id;
    if (finish_batch_and_roundtrip(w, syncobj_fd) < 0) {
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
    if (flush_if_pending(w) < 0) {
        return -EIO;
    }
    uint32_t id = wayland_alloc_id(w);
    if (id == 0) {
        return -EIO;
    }
    uint32_t payload[2] = { id, w->surface_id };
    // wp_linux_drm_syncobj_manager_v1.get_surface (request 1)
    if (!wayland_put_raw(w, w->drm_syncobj_id, 1u, payload, sizeof payload)) {
        fprintf(stderr, "[Wayland Error] explicit sync bind encode failed: %s\n", w->err);
        return -EIO;
    }
    w->explicit_sync_id = id;
    if (finish_batch_and_roundtrip(w, -1) < 0) {
        fprintf(stderr, "[Wayland Error] explicit sync bind failed: %s\n", w->err);
        return -EIO;
    }
    return 0;
}

int wayland_present_single_frame_and_wait(
    WaylandContext *w,
    uint32_t        buffer_id,
    uint32_t        width,
    uint32_t        height,
    uint32_t        configure_serial,
    uint32_t        acquire_timeline,
    uint64_t        acquire_point,
    uint32_t        release_timeline,
    uint64_t        release_point
) {
    if (w == nullptr || !w->is_initialized || buffer_id == 0 || w->surface_id == 0 ||
        w->explicit_sync_id == 0 || acquire_timeline == 0 || release_timeline == 0 ||
        acquire_timeline == release_timeline) {
        return -EINVAL;
    }
    // Flush a batch already queued ahead of this present when the buffer is close to full.
    if (w->send_len + 256u > sizeof(w->send_bytes)) {
        if (send_wire_batch(w) < 0) {
            return -EIO;
        }
    }

    w->frame_done = false;

    if (w->lock_toplevel_size) {
        int32_t sz[2] = { w->lock_width, w->lock_height };
        // set_max_size is request 7, set_min_size is request 8. Applied on commit.
        if (!wayland_put_raw(w, w->xdg_toplevel_id, 7u, sz, sizeof sz) ||
            !wayland_put_raw(w, w->xdg_toplevel_id, 8u, sz, sizeof sz)) {
            fprintf(stderr, "[Wayland Error] Toplevel size lock encode failed: %s\n", w->err);
            return -EIO;
        }
    } else if (w->min_width > 0 && w->min_height > 0 &&
               !put_toplevel_min_size(w, w->min_width, w->min_height)) {
        fprintf(stderr, "[Wayland Error] Toplevel minimum encode failed: %s\n", w->err);
        return -EIO;
    }

    // Ack only a serial we have not already accepted. Repeating one is invalid_serial.
    // acked_configure_serial starts at 0, so the flag distinguishes "never acked".
    bool did_ack = false;
    if (!w->configure_ack_valid || configure_serial != w->acked_configure_serial) {
        if (!wayland_put_raw(w, w->xdg_surface_id, 4u, &configure_serial, sizeof configure_serial)) {
            fprintf(stderr, "[Wayland Error] ack_configure encode failed: %s\n", w->err);
            return -EIO;
        }
        did_ack = true;
    }

    // 2. xdg_surface.set_window_geometry (request 3)
    int32_t geom_payload[4] = { 0, 0, (int32_t)width, (int32_t)height };
    (void)wayland_put_raw(w, w->xdg_surface_id, 3u, geom_payload, sizeof(geom_payload));

    // 3. set_acquire_point (request 1) and set_release_point (request 2) on this commit
    uint32_t acquire_payload[3];
    uint32_t release_payload[3];
    put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
    put_timeline_point(release_payload, release_timeline, release_point);
    (void)wayland_put_raw(w, w->explicit_sync_id, 1u, acquire_payload, sizeof acquire_payload);
    (void)wayland_put_raw(w, w->explicit_sync_id, 2u, release_payload, sizeof release_payload);

    // 4. wl_surface.attach (request 1) at (0, 0)
    uint32_t attach_payload[3] = { buffer_id, 0u, 0u };
    (void)wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof(attach_payload));

    // damage_buffer is request 9 since wl_surface v4. Older surfaces use damage (request 2).
    uint16_t damage_opcode = (w->compositor_bound_version >= 4u) ? 9u : 2u;
    int32_t damage_payload[4] = { 0, 0, (int32_t)width, (int32_t)height };
    (void)wayland_put_raw(w, w->surface_id, damage_opcode, damage_payload, sizeof(damage_payload));

    if (w->viewport_id != 0) {
        int32_t drop_dest[2] = { -1, -1 };
        // wp_viewport.set_destination is request 2. -1,-1 clears the scale.
        (void)wayland_put_raw(w, w->viewport_id, 2u, drop_dest, sizeof drop_dest);
    }

    // 5. wl_surface.commit (request 6)
    (void)wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);

    // 6. wl_display.sync guarantees the compositor processed commit even if occluded.
    // No standing wl_surface.frame callback is armed.
    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    (void)append_display_sync(w, w->sync_id);

    if (send_wire_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Present commit batch failed: %s\n", w->err);
        return -EIO;
    }
    if (did_ack) {
        w->acked_configure_serial = configure_serial;
        w->configure_ack_valid = true;
    }

    struct io_uring_cqe cqe = {0};
    while (!w->synced && !w->failed) {
        if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->synced) {
        fprintf(stderr, "[Wayland Error] Present sync wait failed: %s\n", w->err);
        return -EIO;
    }
    w->frame_done = true;

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(w->ring);
        (void)ring_enter(w->ring, n, 0, -1);
    }

    return 0;
}

int wayland_queue_configure_ack(
    WaylandContext *w,
    uint32_t        serial,
    uint32_t        width,
    uint32_t        height,
    int32_t         viewport_mode
) {
    if (w == nullptr || !w->is_initialized || w->failed ||
        w->surface_id == 0 || w->xdg_surface_id == 0 || width == 0 || height == 0) {
        return -EINVAL;
    }
    if (w->send_len != 0) {
        return -EBUSY;
    }
    uint32_t mark = w->send_len;
    int32_t geom[4] = { 0, 0, (int32_t)width, (int32_t)height };
    if (!wayland_put_raw(w, w->xdg_surface_id, 4u, &serial, sizeof serial) ||
        !wayland_put_raw(w, w->xdg_surface_id, 3u, geom, sizeof geom)) {
        wayland_rewind_send(w, mark);
        return -EIO;
    }
    if (w->viewport_id != 0 && viewport_mode == ENG_VIEWPORT_UNSET) {
        int32_t dest[2] = { -1, -1 };
        if (!wayland_put_raw(w, w->viewport_id, 2u, dest, sizeof dest)) {
            wayland_rewind_send(w, mark);
            return -EIO;
        }
    }
    if (!wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u)) {
        wayland_rewind_send(w, mark);
        return -EIO;
    }
    return wayland_queue_send(w, true);
}

int wayland_queue_turn_commit(
    WaylandContext *w,
    uint32_t        buffer_id,
    uint32_t        width,
    uint32_t        height,
    uint32_t        acquire_timeline,
    uint64_t        acquire_point,
    uint32_t        release_timeline,
    uint64_t        release_point,
    bool            arm_frame,
    bool            with_configure,
    uint32_t        configure_serial
) {
    if (w == nullptr || !w->is_initialized || w->failed || w->surface_id == 0) {
        return -EINVAL;
    }
    if (w->send_len != 0) {
        return -EBUSY;
    }
    if (buffer_id == 0 && !arm_frame) {
        return -EINVAL;
    }
    if (with_configure && buffer_id == 0) {
        return -EINVAL;
    }
    if (buffer_id != 0 &&
        (width == 0 || height == 0 || w->explicit_sync_id == 0 ||
         acquire_timeline == 0 || release_timeline == 0 ||
         acquire_timeline == release_timeline ||
         acquire_point == 0 || release_point == 0)) {
        return -EINVAL;
    }
    if (arm_frame && w->frame_cb_id != 0) {
        return -EALREADY;
    }

    uint32_t mark = w->send_len;
    uint32_t cb = 0;
    if (arm_frame) {
        cb = wayland_alloc_id(w);
        if (cb == 0) {
            return -EIO;
        }
    }

    bool did_ack = false;
    bool ok = true;
    if (with_configure) {
        if (w->xdg_surface_id == 0 || w->xdg_toplevel_id == 0) {
            ok = false;
        } else if (w->lock_toplevel_size) {
            int32_t sz[2] = { w->lock_width, w->lock_height };
            // set_max_size is request 7, set_min_size is request 8. Applied on commit.
            ok = wayland_put_raw(w, w->xdg_toplevel_id, 7u, sz, sizeof sz) &&
                 wayland_put_raw(w, w->xdg_toplevel_id, 8u, sz, sizeof sz);
        } else if (w->min_width > 0 && w->min_height > 0) {
            ok = put_toplevel_min_size(w, w->min_width, w->min_height);
        }
        // Repeating an acked serial is invalid_serial. A new one has to ride this commit.
        if (ok && (!w->configure_ack_valid || configure_serial != w->acked_configure_serial)) {
            ok = wayland_put_raw(w, w->xdg_surface_id, 4u, &configure_serial, sizeof configure_serial);
            did_ack = ok;
        }
        if (ok) {
            int32_t geom[4] = { 0, 0, (int32_t)width, (int32_t)height };
            ok = wayland_put_raw(w, w->xdg_surface_id, 3u, geom, sizeof geom);
        }
        if (ok && w->viewport_id != 0) {
            int32_t drop_dest[2] = { -1, -1 };
            ok = wayland_put_raw(w, w->viewport_id, 2u, drop_dest, sizeof drop_dest);
        }
    }
    if (ok && buffer_id != 0) {
        uint32_t acquire_payload[3];
        uint32_t release_payload[3];
        put_timeline_point(acquire_payload, acquire_timeline, acquire_point);
        put_timeline_point(release_payload, release_timeline, release_point);
        uint32_t attach_payload[3] = { buffer_id, 0u, 0u };
        uint16_t damage_opcode = (w->compositor_bound_version >= 4u) ? 9u : 2u;
        int32_t damage_payload[4] = { 0, 0, (int32_t)width, (int32_t)height };
        ok = wayland_put_raw(w, w->explicit_sync_id, 1u, acquire_payload, sizeof acquire_payload) &&
             wayland_put_raw(w, w->explicit_sync_id, 2u, release_payload, sizeof release_payload) &&
             wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof attach_payload) &&
             wayland_put_raw(w, w->surface_id, damage_opcode, damage_payload, sizeof damage_payload);
    }
    if (ok && cb != 0) {
        // wl_surface.frame is request 3. It applies to the commit below.
        ok = wayland_put_raw(w, w->surface_id, 3u, &cb, sizeof cb);
    }
    if (ok) {
        ok = wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u);
    }
    if (!ok) {
        wayland_rewind_send(w, mark);
        if (cb != 0) {
            free_id(w, cb);
        }
        fprintf(stderr, "[Wayland Error] Queued commit encode failed: %s\n", w->err);
        return -EIO;
    }
    int queued = wayland_queue_send(w, did_ack);
    if (queued != 0) {
        wayland_rewind_send(w, mark);
        if (cb != 0) {
            free_id(w, cb);
        }
        return queued;
    }
    if (cb != 0) {
        w->frame_cb_id = cb;
    }
    return 0;
}

int wayland_queue_send(WaylandContext *w, bool tracks_ack) {
    if (w == nullptr || !w->is_initialized || w->failed || w->send_len == 0) {
        return -EINVAL;
    }
    if (w->send_off >= w->send_len) {
        return -EINVAL;
    }
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while queuing wayland send");
        return -ENOSPC;
    }
    w->send_tracks_ack = tracks_ack;
    w->send_msg.msg_control    = nullptr;
    w->send_msg.msg_controllen = 0u;

    sqe->opcode    = IORING_OP_SEND;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->ioprio    = IORING_RECVSEND_POLL_FIRST;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)(w->send_bytes + w->send_off);
    sqe->len       = w->send_len - w->send_off;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_SEND;
    return 0;
}

int wayland_note_send_cqe(
    WaylandContext            *w,
    const struct io_uring_cqe *cqe,
    uint32_t                   acked_serial
) {
    if (w == nullptr || cqe == nullptr) {
        return -EINVAL;
    }
    int progress = note_send_progress(w, cqe->res);
    if (progress != 0) {
        return progress;
    }
    bool track = w->send_tracks_ack;
    w->send_tracks_ack = false;
    if (track) {
        w->acked_configure_serial = acked_serial;
        w->configure_ack_valid = true;
    }
    return 0;
}

int wayland_detach_buffer(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized || w->surface_id == 0 || w->failed) {
        return -EINVAL;
    }
    if (flush_if_pending(w) < 0) {
        return -EIO;
    }
    // A null wl_buffer unmaps the surface. Acquire and release points are
    // required only on a commit that attaches a non-null buffer.
    uint32_t attach_payload[3] = { 0u, 0u, 0u };
    if (!wayland_put_raw(w, w->surface_id, 1u, attach_payload, sizeof attach_payload) ||
        !wayland_put_raw(w, w->surface_id, 6u, nullptr, 0u)) {
        fprintf(stderr, "[Wayland Error] buffer detach encode failed: %s\n", w->err);
        return -EIO;
    }
    if (finish_batch_and_roundtrip(w, -1) < 0) {
        fprintf(stderr, "[Wayland Error] buffer detach failed: %s\n", w->err);
        return -EIO;
    }
    return 0;
}

void wayland_destroy_frame_objects(WaylandContext *w) {
    if (w == nullptr || !w->is_initialized) {
        return;
    }
    if (!w->failed && w->send_len > 0) {
        (void)send_wire_batch(w);
    }
    w->send_len = 0;
    w->send_off = 0;
    if (w->viewport_id != 0) {
        (void)wayland_put_raw(w, w->viewport_id, 0u, nullptr, 0u);
        w->viewport_id = 0;
    }
    if (w->explicit_sync_id != 0) {
        (void)wayland_put_raw(w, w->explicit_sync_id, 0u, nullptr, 0u);
        w->explicit_sync_id = 0;
    }
    if (w->acquire_timeline_id != 0) {
        (void)wayland_put_raw(w, w->acquire_timeline_id, 0u, nullptr, 0u);
        w->acquire_timeline_id = 0;
    }
    if (w->release_timeline_id != 0) {
        (void)wayland_put_raw(w, w->release_timeline_id, 0u, nullptr, 0u);
        w->release_timeline_id = 0;
    }
    if (w->xdg_toplevel_id != 0) {
        (void)wayland_put_raw(w, w->xdg_toplevel_id, 0u, nullptr, 0u);
        w->xdg_toplevel_id = 0;
    }
    if (w->xdg_surface_id != 0) {
        (void)wayland_put_raw(w, w->xdg_surface_id, 0u, nullptr, 0u);
        w->xdg_surface_id = 0;
    }
    if (w->surface_id != 0) {
        (void)wayland_put_raw(w, w->surface_id, 0u, nullptr, 0u);
        w->surface_id = 0;
    }
    if (w->buffer_id != 0) {
        (void)wayland_put_raw(w, w->buffer_id, 0u, nullptr, 0u);
        w->buffer_id = 0;
    }

    if (w->send_len > 0 && !w->failed) {
        w->synced  = false;
        w->sync_id = wayland_alloc_id(w);
        (void)append_display_sync(w, w->sync_id);
        if (send_wire_batch(w) == 0) {
            struct io_uring_cqe cqe = {0};
            while (!w->synced && !w->failed) {
                if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
                    break;
                }
                wayland_on_cqe(w, &cqe);
            }
        }
    }
    close_feedback_table(w);
}

int wayland_init(void) {
    RingContext *r = ring_get_context();
    if (r == nullptr || !r->is_initialized) {
        fprintf(stderr, "[Wayland Error] Primary io_uring must be initialized before wayland_init\n");
        return 1;
    }
    WaylandContext *w = &s_wl_ctx;
    *w = (WaylandContext){
        .ring              = r,
        .display_id        = 1,
        .next_id           = 2,
        .feedback_table_fd = -1,
    };
    w->registry_id = wayland_alloc_id(w);
    w->sync_id     = wayland_alloc_id(w);

    const char *dir  = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("WAYLAND_DISPLAY");
    if (dir == nullptr || dir[0] == '\0' || disp == nullptr || disp[0] == '\0') {
        fprintf(stderr, "[Wayland Error] XDG_RUNTIME_DIR or WAYLAND_DISPLAY is not set\n");
        return 1;
    }

    int path_n = snprintf(w->path, sizeof w->path, "%s/%s", dir, disp);
    if (path_n < 0 || (size_t)path_n >= sizeof w->path) {
        fprintf(stderr, "[Wayland Error] Wayland socket path exceeds sockaddr_un limit\n");
        return 1;
    }
    if (access(w->path, F_OK) != 0) {
        fprintf(stderr, "[Wayland Error] Socket '%s' not accessible: %s\n", w->path, strerror(errno));
        return 1;
    }

    w->addr.sun_family = AF_UNIX;
    memcpy(w->addr.sun_path, w->path, (size_t)path_n + 1u);
    w->addr_len = (unsigned)(offsetof(struct sockaddr_un, sun_path) + (size_t)path_n + 1u);

    // 1. Direct socket creation into io_uring fixed file slot 0 (1-based file_index = 1)
    struct io_uring_sqe *sqe = ring_sqe(r);
    struct io_uring_cqe cqe  = {0};
    sqe->opcode     = IORING_OP_SOCKET;
    sqe->fd         = AF_UNIX;
    sqe->off        = SOCK_STREAM | SOCK_NONBLOCK;
    sqe->file_index = ENG_WL_SOCK_SLOT + 1u;
    sqe->user_data  = ENG_UD_WL_SOCKET;
    if (submit_and_wait_tag(w, ENG_UD_WL_SOCKET, &cqe) < 0 || cqe.res != 0) {
        fprintf(stderr, "[Wayland Error] IORING_OP_SOCKET fixed slot 0 failed\n");
        return 1;
    }
    w->fixed_socket_installed = true;

    // 2. Connect fixed file 0 to the Wayland compositor
    sqe = ring_sqe(r);
    sqe->opcode    = IORING_OP_CONNECT;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)&w->addr;
    sqe->addr2     = w->addr_len;
    sqe->user_data = ENG_UD_WL_CONNECT;
    if (submit_and_wait_tag(w, ENG_UD_WL_CONNECT, &cqe) < 0 || cqe.res < 0) {
        fprintf(stderr, "[Wayland Error] IORING_OP_CONNECT failed: %s (%d)\n",
                strerror(-cqe.res), cqe.res);
        return 1;
    }

    // 3. Send wl_display.get_registry (opcode 1) + wl_display.sync (opcode 0)
    w->send_len = 0;
    (void)wayland_put_raw(w, w->display_id, 1u, &w->registry_id, sizeof(w->registry_id));
    (void)append_display_sync(w, w->sync_id);

    w->control_reserve         = 256u;
    w->recv_iov.iov_base       = r->bufs;
    w->recv_iov.iov_len        = ENG_PBUF_LEN;
    w->recv_msg.msg_iov        = &w->recv_iov;
    w->recv_msg.msg_iovlen     = 1u;
    w->recv_msg.msg_control    = w->recv_control;
    w->recv_msg.msg_controllen = w->control_reserve;

    if (send_wire_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Registry handshake send failed: %s\n", w->err);
        return 1;
    }

    // 4. Arm multishot RECVMSG and drain until first wl_callback.done
    arm_recv(w);
    while (!w->synced && !w->failed) {
        if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->synced) {
        fprintf(stderr, "[Wayland Error] Wire registry sync failed: %s\n", w->err);
        return 1;
    }

    if (w->compositor_global_name == 0 || w->xdg_wm_base_global_name == 0 ||
        w->linux_dmabuf_global_name == 0) {
        fprintf(stderr,
                "[Wayland Error] Missing required compositor globals (compositor=%u, xdg_wm_base=%u, linux_dmabuf=%u)\n",
                w->compositor_global_name, w->xdg_wm_base_global_name, w->linux_dmabuf_global_name);
        return 1;
    }
    if (w->linux_dmabuf_version < 4u) {
        fprintf(stderr,
                "[Wayland Error] zwp_linux_dmabuf_v1 version is %u, but engine requires v4+\n",
                w->linux_dmabuf_version);
        return 1;
    }

    // 5. Bind the core interfaces and verify with a second wl_display.sync roundtrip
    w->compositor_id            = wayland_alloc_id(w);
    w->compositor_bound_version = min_u32(w->compositor_version, 6u);
    (void)append_registry_bind(w, w->compositor_global_name, "wl_compositor",
                               w->compositor_bound_version, w->compositor_id);

    w->xdg_wm_base_id = wayland_alloc_id(w);
    (void)append_registry_bind(w, w->xdg_wm_base_global_name, "xdg_wm_base",
                               min_u32(w->xdg_wm_base_version, 6u), w->xdg_wm_base_id);

    w->linux_dmabuf_id            = wayland_alloc_id(w);
    w->linux_dmabuf_bound_version = min_u32(w->linux_dmabuf_version, 4u);
    (void)append_registry_bind(w, w->linux_dmabuf_global_name, "zwp_linux_dmabuf_v1",
                               w->linux_dmabuf_bound_version, w->linux_dmabuf_id);

    if (w->drm_syncobj_global_name != 0) {
        w->drm_syncobj_id = wayland_alloc_id(w);
        (void)append_registry_bind(w, w->drm_syncobj_global_name, "wp_linux_drm_syncobj_manager_v1",
                                   1u, w->drm_syncobj_id);
    }

    if (w->seat_global_name != 0) {
        w->seat_id = wayland_alloc_id(w);
        w->seat_bound_version = min_u32(w->seat_version, 9u);
        (void)append_registry_bind(w, w->seat_global_name, "wl_seat",
                                   w->seat_bound_version, w->seat_id);
    }
    // The bar is cleared into the dma-buf. wl_shm is only the cursor fallback.
    if (w->cursor_shape_global_name == 0 && w->shm_global_name != 0) {
        w->shm_id = wayland_alloc_id(w);
        (void)append_registry_bind(w, w->shm_global_name, "wl_shm",
                                   min_u32(w->shm_version, 1u), w->shm_id);
    }
    if (w->cursor_shape_global_name != 0) {
        w->cursor_shape_manager_id = wayland_alloc_id(w);
        w->cursor_shape_bound_version = min_u32(w->cursor_shape_version, 2u);
        (void)append_registry_bind(w, w->cursor_shape_global_name, "wp_cursor_shape_manager_v1",
                                   w->cursor_shape_bound_version, w->cursor_shape_manager_id);
    }
    if (w->viewporter_global_name != 0) {
        w->viewporter_id = wayland_alloc_id(w);
        (void)append_registry_bind(w, w->viewporter_global_name, "wp_viewporter",
                                   min_u32(w->viewporter_version, 1u), w->viewporter_id);
    }

    w->synced  = false;
    w->sync_id = wayland_alloc_id(w);
    (void)append_display_sync(w, w->sync_id);

    if (send_wire_batch(w) < 0) {
        fprintf(stderr, "[Wayland Error] Global bind batch failed: %s\n", w->err);
        return 1;
    }

    while (!w->synced && !w->failed) {
        if (submit_and_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }
    if (w->failed || !w->synced) {
        fprintf(stderr, "[Wayland Error] Global bind verification failed: %s\n", w->err);
        return 1;
    }

    if (w->ring->sq_pending > 0) {
        unsigned n = ring_submit(r);
        (void)ring_enter(r, n, 0, -1);
    }

    w->send_len = 0;
    if (pointer_open_devices(w) != 0) {
        return 1;
    }

    w->is_initialized = true;
    printf("[Wayland] Wire connected (fixed slot 0: %s) | Globals: %u\n",
           w->path, w->globals_seen);
    printf("[Wayland] Bound Object IDs: wl_compositor=%u, xdg_wm_base=%u, linux_dmabuf=%u, drm_syncobj=%u\n",
           w->compositor_id,
           w->xdg_wm_base_id,
           w->linux_dmabuf_id,
           w->drm_syncobj_id);
    printf("[Wayland] Input Object IDs: wl_seat=%u, wl_shm=%u, cursor_shape=%u, viewporter=%u\n",
           w->seat_id, w->shm_id, w->cursor_shape_manager_id, w->viewporter_id);
    return 0;
}

void wayland_shutdown(void) {
    WaylandContext *w = &s_wl_ctx;
    if (!w->is_initialized) {
        return;
    }
    pointer_shutdown(w);
    close_feedback_table(w);
    for (uint32_t i = 0; i < w->nparked; ++i) {
        close(w->parked_fds[i]);
    }
    w->nparked        = 0;
    w->is_initialized = false;
    printf("goodbye from wayland\n");
}

WaylandContext *wayland_get_context(void) {
    return &s_wl_ctx;
}