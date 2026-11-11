// Socket reads and the Wayland event demux.
#include "wayland/internal.h"
#include "input/pointer.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void park_fds(WaylandContext *w, const unsigned char *control, uint32_t controllen);
static void note_key_global(WaylandContext *w, uint32_t name, const char *iface, uint32_t version);
static void handle_display_msg(WaylandContext *w, uint16_t opcode,
                               const unsigned char *p, const unsigned char *end);
static void handle_registry_msg(WaylandContext *w, uint16_t opcode,
                                const unsigned char *p, const unsigned char *end);
static void handle_xdg_toplevel_msg(WaylandContext *w, uint16_t opcode,
                                    const unsigned char *p, const unsigned char *end);
static uint32_t parse_span(WaylandContext *w, const unsigned char *data, uint32_t len);


bool wayland_take_parked_fd(WaylandContext *w, int *out_fd) {
    if (w == nullptr || out_fd == nullptr || w->nparked == 0) {
        return false;
    }
    *out_fd = w->parked_fds[w->parked_head];
    w->parked_head = (w->parked_head + 1u) & (ENG_FD_PARK - 1u);
    w->nparked--;
    return true;
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
            uint32_t slot = (w->parked_head + w->nparked) & (ENG_FD_PARK - 1u);
            w->parked_fds[slot] = fds[i];
            w->nparked++;
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
    } else if (strcmp(iface, "wl_subcompositor") == 0 && w->subcompositor_global_name == 0) {
        w->subcompositor_global_name = name;
        w->subcompositor_version     = version;
    } else if (strcmp(iface, "zxdg_decoration_manager_v1") == 0 && w->decoration_global_name == 0) {
        w->decoration_global_name = name;
        w->decoration_version     = version;
    }
}


static void handle_display_msg(WaylandContext *w, uint16_t opcode,
                               const unsigned char *p, const unsigned char *end) {
    if (opcode == 0u) {
        uint32_t bad_id = 0, code = 0, slen = 0;
        if (!wl_take_u32(&p, end, &bad_id) || !wl_take_u32(&p, end, &code) ||
            !wl_take_u32(&p, end, &slen) || slen == 0 || p + slen > end) {
            wl_fail(w, "truncated wl_display.error");
            return;
        }
        snprintf(w->err, sizeof w->err, "wl_display.error object %u code %u: %.*s",
                 bad_id, code, (int)(slen - 1u), (const char *)p);
        w->failed = true;
    } else if (opcode == 1u) {
        uint32_t id = 0;
        if (!wl_take_u32(&p, end, &id)) {
            wl_fail(w, "truncated wl_display.delete_id");
            return;
        }
        wl_free_id(w, id);
    }
}


static void handle_registry_msg(WaylandContext *w, uint16_t opcode,
                                const unsigned char *p, const unsigned char *end) {
    if (opcode != 0u) {
        return;
    }
    uint32_t name = 0, slen = 0, version = 0;
    if (!wl_take_u32(&p, end, &name) || !wl_take_u32(&p, end, &slen) ||
        slen == 0 || slen > ENG_WL_NAME || p + slen > end || p[slen - 1] != 0) {
        wl_fail(w, "invalid wl_registry.global");
        return;
    }
    const char *iface = (const char *)p;
    uint32_t pad = (4u - (slen & 3u)) & 3u;
    p += slen + pad;
    if (!wl_take_u32(&p, end, &version) || p != end) {
        wl_fail(w, "wl_registry.global size mismatch");
        return;
    }
    w->globals_seen++;
    if (w->nglobals < ENG_WL_GLOBALS) {
        WaylandGlobal *g = &w->globals[w->nglobals++];
        g->name    = name;
        g->version = version;
        memcpy(g->interface, iface, slen);
    }
    note_key_global(w, name, iface, version);
}


static void handle_xdg_toplevel_msg(WaylandContext *w, uint16_t opcode,
                                    const unsigned char *p, const unsigned char *end) {
    if (opcode == 0u) {
        int32_t width = 0, height = 0;
        uint32_t states_len = 0;
        if (!wl_take_i32(&p, end, &width) || !wl_take_i32(&p, end, &height) ||
            !wl_take_u32(&p, end, &states_len)) {
            wl_fail(w, "truncated xdg_toplevel.configure");
            return;
        }
        uint32_t pad = (4u - (states_len & 3u)) & 3u;
        if ((size_t)(end - p) < (size_t)states_len + pad) {
            wl_fail(w, "xdg_toplevel.configure state array out of bounds");
            return;
        }
        uint32_t states = 0;
        uint32_t left = states_len;
        const unsigned char *sp = p;
        while (left >= 4u) {
            uint32_t st = 0;
            memcpy(&st, sp, 4);
            if (st < 31u) {
                states |= 1u << st;
            }
            sp += 4;
            left -= 4u;
        }
        w->toplevel_states     = states;
        w->configure_resizing  = (states & ENG_TOP_RESIZING) != 0;
        w->configure_width     = (width > 0) ? (uint32_t)width : 0u;
        w->configure_height    = (height > 0) ? (uint32_t)height : 0u;
        w->toplevel_configured = true;
    } else if (opcode == 1u) {
        // The client owns the exit. Close is not a protocol failure.
        w->toplevel_closed = true;
    }
}


static uint32_t parse_span(WaylandContext *w, const unsigned char *data, uint32_t len) {
    uint32_t offset = 0;
    const unsigned char *stream_end = data + len;
    while (len - offset >= 8u && !w->failed) {
        const unsigned char *msg_start = data + offset;
        const unsigned char *p         = msg_start;
        uint32_t object = 0;
        uint16_t opcode = 0;
        uint16_t size   = 0;

        if (!wl_take_u32(&p, stream_end, &object) ||
            !wl_take_u16(&p, stream_end, &opcode) ||
            !wl_take_u16(&p, stream_end, &size)) {
            break;
        }
        if (size < 8u || (size & 3u) != 0u) {
            wl_fail(w, "invalid wayland wire message size");
            break;
        }
        if (len - offset < size) {
            break;
        }

        const unsigned char *end = msg_start + size;
        p = msg_start + 8;

        if (object == w->display_id) {
            handle_display_msg(w, opcode, p, end);
        } else if (w->registry_id != 0 && object == w->registry_id) {
            handle_registry_msg(w, opcode, p, end);
        } else if (w->xdg_wm_base_id != 0 && object == w->xdg_wm_base_id && opcode == 0u) {
            uint32_t serial = 0;
            if (!wl_take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated xdg_wm_base.ping");
                break;
            }
            wl_queue_xdg_pong(w, serial);
        } else if (w->sync_id != 0 && object == w->sync_id && opcode == 0u) {
            uint32_t serial = 0;
            if (size != 12u || !wl_take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated sync wl_callback.done");
                break;
            }
            w->synced = true;
        } else if (w->frame_cb_id != 0 && object == w->frame_cb_id && opcode == 0u) {
            uint32_t timestamp_ms = 0;
            if (size != 12u || !wl_take_u32(&p, end, &timestamp_ms)) {
                wl_fail(w, "truncated frame wl_callback.done");
                break;
            }
            w->frame_time_ms = timestamp_ms;
            w->frame_stamp_valid = true;
            w->frame_done = true;
            // delete_id returns this id. Clear the slot so the next commit can arm one.
            w->frame_cb_id = 0;
        } else if (w->xdg_toplevel_id != 0 && object == w->xdg_toplevel_id) {
            handle_xdg_toplevel_msg(w, opcode, p, end);
        } else if (w->decoration_id != 0 && object == w->decoration_id) {
            wl_on_decoration_event(w, opcode, p, end);
        } else if (w->xdg_surface_id != 0 && object == w->xdg_surface_id && opcode == 0u) {
            uint32_t serial = 0;
            if (!wl_take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated xdg_surface.configure");
                break;
            }
            w->configure_serial       = serial;
            w->xdg_surface_configured = true;
        } else if (w->feedback_id != 0 && object == w->feedback_id) {
            wl_handle_dmabuf_feedback(w, opcode, size, p, end);
        } else if (w->buffer_params_id != 0 && object == w->buffer_params_id && opcode == 1u) {
            wl_fail(w, "zwp_linux_buffer_params_v1.failed received from compositor");
            break;
        } else if (w->buffer_id != 0 && object == w->buffer_id && opcode == 0u) {
            w->buffer_released = true;
        } else {
            (void)pointer_dispatch(w, object, opcode, msg_start + 8, (uint16_t)(size - 8u));
        }

        if (w->failed) {
            break;
        }
        offset += size;
    }
    return offset;
}


void wayland_on_cqe(WaylandContext *w, const struct io_uring_cqe *cqe) {
    if (cqe->user_data == ENG_UD_SIGNAL) {
        w->noted_signal = true;
        w->noted_signal_res = cqe->res;
        return;
    }
    // A blocking wait shares this ring. Recording the completion is all
    // that is safe here. The idle loop applies it after the wait returns.
    if (cqe->user_data == ENG_UD_FRAME_TIMER) {
        w->noted_timer = true;
        return;
    }
    if (cqe->user_data == ENG_UD_INOTIFY) {
        w->noted_inotify = true;
        w->noted_inotify_res = cqe->res;
        return;
    }
    if (ring_ud_is_pager(cqe->user_data)) {
        uint32_t idx = (uint32_t)(cqe->user_data - ENG_UD_PAGER_BASE);
        w->noted_pager_mask |= (uint16_t)(1u << idx);
        w->noted_pager_res[idx] = cqe->res;
        return;
    }
    if (cqe->user_data == ENG_UD_SIM_TIMER) {
        // Recycle the provided buffer here. A blocking wait can see several
        // ticks before the idle loop applies the note, and each tick owns a
        // distinct buffer.
        uint64_t n = 0;
        bool have = ring_take_sim_count(w->ring, cqe, &n);
        if (!w->noted_sim) {
            w->noted_sim = true;
            w->noted_sim_res = cqe->res;
            w->noted_sim_alive = (cqe->flags & IORING_CQE_F_MORE) != 0;
            w->noted_sim_exp = have ? n : 0;
        } else {
            if (have) {
                w->noted_sim_exp += n;
            }
            if (cqe->res < 0) {
                w->noted_sim_res = cqe->res;
            }
            if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
                w->noted_sim_alive = false;
            }
        }
        return;
    }
    if (cqe->user_data == ENG_UD_WL_SEND) {
        w->send_queued = false;
        if (!w->noted_send) {
            w->noted_send = true;
            w->noted_send_res = cqe->res;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_WL_PONG) {
        w->pong_in_flight = false;
        uint32_t left = (w->pong_off < 12u) ? (12u - w->pong_off) : 0u;
        if (cqe->res == -EAGAIN || cqe->res == -EINTR || cqe->res == 0) {
            if (wl_arm_pong(w) != 0) {
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
            if (wl_arm_pong(w) != 0) {
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
    uint8_t *buf = ring_pbuf_chunk(w->ring, bid, (uint32_t)cqe->res);
    if (buf == nullptr) {
        ring_pbuf_finish(w->ring, bid);
        wl_fail(w, "recvmsg buffer ID out of range");
        return;
    }

    const struct io_uring_recvmsg_out *out = (const struct io_uring_recvmsg_out *)buf;
    uint32_t hdr = (uint32_t)sizeof(*out) + w->control_reserve;
    if ((uint32_t)cqe->res < hdr || (out->flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
        ring_pbuf_finish(w->ring, bid);
        wl_fail(w, "recvmsg truncated");
        return;
    }

    uint32_t copied = (uint32_t)cqe->res - hdr;
    if (out->controllen > 0) {
        park_fds(w, buf + sizeof(*out), out->controllen);
    }
    if (w->failed) {
        ring_pbuf_finish(w->ring, bid);
        return;
    }
    const unsigned char *payload = buf + hdr;
    uint32_t left = copied;
    if (w->stream_len > 0) {
        // Stitch only the single message that straddled the previous recvmsg,
        // then parse the rest of the incoming buffer in-place without memmove.
        if (w->stream_len < 8u && left > 0u) {
            uint32_t need_hdr = 8u - w->stream_len;
            uint32_t take = (left < need_hdr) ? left : need_hdr;
            memcpy(w->stream + w->stream_len, payload, take);
            w->stream_len += take;
            payload += take;
            left -= take;
        }
        if (w->stream_len >= 8u) {
            uint16_t msg_size = 0;
            memcpy(&msg_size, w->stream + 6, 2);
            if (msg_size < 8u || (msg_size & 3u) != 0u) {
                wl_fail(w, "invalid wayland wire message size");
            } else if (msg_size > sizeof w->stream) {
                wl_fail(w, "wayland stream buffer overflow");
            } else {
                uint32_t need_body = (uint32_t)msg_size - w->stream_len;
                uint32_t take = (left < need_body) ? left : need_body;
                if (take > 0u) {
                    memcpy(w->stream + w->stream_len, payload, take);
                    w->stream_len += take;
                    payload += take;
                    left -= take;
                }
                if (w->stream_len == (uint32_t)msg_size) {
                    (void)parse_span(w, w->stream, msg_size);
                    w->stream_len = 0;
                }
            }
        }
    }
    if (!w->failed && w->stream_len == 0 && left > 0u) {
        uint32_t consumed = parse_span(w, payload, left);
        uint32_t tail = left - consumed;
        if (!w->failed && tail > 0u) {
            if (tail > sizeof w->stream) {
                wl_fail(w, "wayland stream buffer overflow");
            } else {
                memcpy(w->stream, payload + consumed, tail);
                w->stream_len = tail;
            }
        }
    }
    ring_pbuf_finish(w->ring, bid);

    if (!w->failed) {
        (void)wayland_ensure_recv(w);
    }
}

