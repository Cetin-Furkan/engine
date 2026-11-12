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


static constexpr uint32_t k_toplevel_constrained =
    ENG_TOP_MAXIMIZED | ENG_TOP_FULLSCREEN |
    ENG_TOP_TILED_LEFT | ENG_TOP_TILED_RIGHT |
    ENG_TOP_TILED_TOP | ENG_TOP_TILED_BOTTOM;

static void close_cmsg_fds(const unsigned char *control, uint32_t controllen) {
    if (control == nullptr || controllen < sizeof(struct cmsghdr)) {
        return;
    }
    struct msghdr msg = {
        .msg_control    = (void *)control,
        .msg_controllen = controllen,
    };
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) continue;
        size_t avail = (size_t)((control + controllen) - (const unsigned char *)CMSG_DATA(cmsg));
        size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
        if (data_len > avail) {
            data_len = avail;
        }
        int n = (int)(data_len / sizeof(int));
        int *fds = (int *)CMSG_DATA(cmsg);
        for (int i = 0; i < n; ++i) {
            if (fds[i] >= 0) {
                close(fds[i]);
            }
        }
    }
}

static void park_fds(WaylandContext *w, const unsigned char *control, uint32_t controllen) {
    if (control == nullptr || controllen < sizeof(struct cmsghdr)) {
        return;
    }
    struct msghdr msg = {
        .msg_control    = (void *)control,
        .msg_controllen = controllen,
    };
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) continue;
        size_t avail = (size_t)((control + controllen) - (const unsigned char *)CMSG_DATA(cmsg));
        size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
        if (data_len > avail) {
            data_len = avail;
        }
        int n = (int)(data_len / sizeof(int));
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
    } else if (strcmp(iface, "wp_fractional_scale_manager_v1") == 0 && w->fractional_scale_global_name == 0) {
        w->fractional_scale_global_name = name;
        w->fractional_scale_version     = version;
    } else if (strcmp(iface, "wp_fifo_manager_v1") == 0 && w->fifo_global_name == 0) {
        w->fifo_global_name = name;
        w->fifo_version     = version;
    } else if (strcmp(iface, "wp_commit_timing_manager_v1") == 0 && w->commit_timing_global_name == 0) {
        w->commit_timing_global_name = name;
        w->commit_timing_version     = version;
    } else if (strcmp(iface, "wp_presentation") == 0 && w->presentation_global_name == 0) {
        w->presentation_global_name = name;
        w->presentation_version     = version;
    } else if (strcmp(iface, "zwp_relative_pointer_manager_v1") == 0 && w->relative_pointer_global_name == 0) {
        w->relative_pointer_global_name = name;
        w->relative_pointer_version     = version;
    } else if (strcmp(iface, "zwp_pointer_constraints_v1") == 0 && w->pointer_constraints_global_name == 0) {
        w->pointer_constraints_global_name = name;
        w->pointer_constraints_version     = version;
    } else if (strcmp(iface, "wl_data_device_manager") == 0 && w->data_device_manager_global_name == 0) {
        w->data_device_manager_global_name = name;
        w->data_device_manager_version     = version;
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
        w->staged_states       = states;
        w->staged_width        = (width > 0) ? (uint32_t)width : 0u;
        w->staged_height       = (height > 0) ? (uint32_t)height : 0u;
        w->has_staged_toplevel = true;
    } else if (opcode == 1u) {
        // The client owns the exit. Close is not a protocol failure.
        w->toplevel_closed = true;
    } else if (opcode == 2u) {
        // xdg_toplevel.configure_bounds (v4+)
        int32_t bw = 0, bh = 0;
        if (!wl_take_i32(&p, end, &bw) || !wl_take_i32(&p, end, &bh)) {
            wl_fail(w, "truncated xdg_toplevel.configure_bounds");
            return;
        }
        w->configure_bounds_w = (bw > 0) ? (uint32_t)bw : 0u;
        w->configure_bounds_h = (bh > 0) ? (uint32_t)bh : 0u;
    } else if (opcode == 3u) {
        // xdg_toplevel.wm_capabilities (v5+)
        uint32_t cap_len = 0;
        if (!wl_take_u32(&p, end, &cap_len)) {
            wl_fail(w, "truncated xdg_toplevel.wm_capabilities");
            return;
        }
        uint32_t pad = (4u - (cap_len & 3u)) & 3u;
        if ((size_t)(end - p) < (size_t)cap_len + pad) {
            wl_fail(w, "xdg_toplevel.wm_capabilities array out of bounds");
            return;
        }
        uint32_t caps = 0u;
        uint32_t left = cap_len;
        const unsigned char *cp = p;
        while (left >= 4u) {
            uint32_t cap = 0;
            memcpy(&cap, cp, 4);
            if (cap < 31u) {
                caps |= 1u << cap;
            }
            cp += 4;
            left -= 4u;
        }
        w->wm_capabilities = caps;
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
            if (w->has_staged_toplevel) {
                bool was_constrained = (w->toplevel_states & k_toplevel_constrained) != 0u;
                bool now_constrained = (w->staged_states & k_toplevel_constrained) != 0u;
                if (!was_constrained && w->configure_width > 0u && w->configure_height > 0u) {
                    w->floating_width  = w->configure_width;
                    w->floating_height = w->configure_height;
                }
                if (w->staged_width > 0u && w->staged_height > 0u) {
                    w->configure_width  = w->staged_width;
                    w->configure_height = w->staged_height;
                    if (!now_constrained) {
                        w->floating_width  = w->staged_width;
                        w->floating_height = w->staged_height;
                    }
                } else if (was_constrained && !now_constrained &&
                           w->floating_width > 0u && w->floating_height > 0u) {
                    w->configure_width  = w->floating_width;
                    w->configure_height = w->floating_height;
                } else {
                    w->configure_width  = 0u;
                    w->configure_height = 0u;
                }
                w->toplevel_states     = w->staged_states;
                w->configure_resizing  = (w->staged_states & ENG_TOP_RESIZING) != 0u;
                w->has_staged_toplevel = false;
            }
            w->toplevel_configured    = true;
            w->configure_serial       = serial;
            w->xdg_surface_configured = true;
        } else if (w->feedback_id != 0 && object == w->feedback_id) {
            wl_handle_dmabuf_feedback(w, opcode, size, p, end);
        } else if (w->retired_feedback_id != 0 && object == w->retired_feedback_id) {
            if (opcode == 1u) {
                int stale_fd = -1;
                if (wayland_take_parked_fd(w, &stale_fd) && stale_fd >= 0) {
                    close(stale_fd);
                }
            }
        } else if (w->buffer_params_id != 0 && object == w->buffer_params_id && opcode == 1u) {
            wl_fail(w, "zwp_linux_buffer_params_v1.failed received from compositor");
            break;
        } else if (opcode == 0u && size == 8u &&
                   (wayland_mark_buffer_released(w, object) ||
                    (w->buffer_id != 0u && object == w->buffer_id))) {
            if (object == w->buffer_id) {
                w->buffer_released = true;
            }
        } else if (w->fractional_scale_id != 0 && object == w->fractional_scale_id && opcode == 0u) {
            uint32_t next_scale = 0u;
            if (!wl_take_u32(&p, end, &next_scale)) {
                wl_fail(w, "truncated wp_fractional_scale_v1.preferred_scale");
                break;
            }
            if (next_scale >= 60u && next_scale <= 960u && next_scale != w->scale_120) {
                bool after_cfg = w->xdg_surface_configured;
                w->scale_120 = next_scale;
                if (after_cfg && w->viewporter_id != 0u) {
                    w->scale_changed = true;
                }
            }
        } else if (w->surface_id != 0 && object == w->surface_id) {
            if (opcode == 2u && w->fractional_scale_id == 0u) {
                int32_t factor = 1;
                if (!wl_take_i32(&p, end, &factor)) {
                    wl_fail(w, "truncated wl_surface.preferred_buffer_scale");
                    break;
                }
                if (factor >= 1 && factor <= 8) {
                    uint32_t next_scale = (uint32_t)factor * 120u;
                    if (next_scale != w->scale_120) {
                        bool after_cfg = w->xdg_surface_configured;
                        w->scale_120 = next_scale;
                        if (after_cfg && w->viewporter_id != 0u) {
                            w->scale_changed = true;
                        }
                    }
                }
            }
        } else if (w->presentation_id != 0 && object == w->presentation_id && opcode == 0u) {
            uint32_t clk_id = 0u;
            if (!wl_take_u32(&p, end, &clk_id)) {
                wl_fail(w, "truncated wp_presentation.clock_id");
                break;
            }
            w->presentation_clk_id  = clk_id;
            w->has_presentation_clk = true;
        } else if (w->presentation_fb_id != 0 && object == w->presentation_fb_id) {
            if (opcode == 1u) {
                uint32_t tv_sec_hi = 0u, tv_sec_lo = 0u, tv_nsec = 0u;
                uint32_t refresh_ns = 0u, seq_hi = 0u, seq_lo = 0u, flags = 0u;
                if (!wl_take_u32(&p, end, &tv_sec_hi) || !wl_take_u32(&p, end, &tv_sec_lo) ||
                    !wl_take_u32(&p, end, &tv_nsec) || !wl_take_u32(&p, end, &refresh_ns) ||
                    !wl_take_u32(&p, end, &seq_hi) || !wl_take_u32(&p, end, &seq_lo) ||
                    !wl_take_u32(&p, end, &flags)) {
                    wl_fail(w, "truncated wp_presentation_feedback.presented");
                    break;
                }
                (void)seq_hi;
                (void)seq_lo;
                uint64_t sec = ((uint64_t)tv_sec_hi << 32u) | (uint64_t)tv_sec_lo;
                w->last_present_ns    = sec * 1000000000ull + (uint64_t)tv_nsec;
                w->last_refresh_ns    = refresh_ns;
                w->last_present_flags = flags;
                w->presented_frames++;
                if ((flags & ENG_PRESENTATION_ZERO_COPY) != 0u) {
                    w->zero_copy_scanout_frames++;
                }
                // presented is a destructor event; server sends wl_display.delete_id.
                w->presentation_fb_id = 0u;
            } else if (opcode == 2u) {
                w->discarded_frames++;
                // discarded is a destructor event; server sends wl_display.delete_id.
                w->presentation_fb_id = 0u;
            }
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
    if (cqe->user_data == ENG_UD_SIM_CANCEL) {
        w->noted_sim_cancels++;
        return;
    }
    if (cqe->user_data == ENG_UD_TIMER_UPDATE) {
        return;
    }
    if (cqe->user_data == ENG_UD_CLIP_READ ||
        cqe->user_data == ENG_UD_CLIP_WRITE ||
        cqe->user_data == ENG_UD_CLIP_CLOSE ||
        cqe->user_data == ENG_UD_CLIP_CANCEL) {
        pointer_on_clip_cqe(w, cqe);
        return;
    }
    if (ring_ud_tag(cqe->user_data) == (uint16_t)ENG_UD_SIM_TIMER) {
        if ((cqe->flags & IORING_CQE_F_MORE) == 0) {
            w->noted_sim_retired++;
        }
        w->noted_sim         = true;
        w->noted_sim_last_ud = cqe->user_data;
        w->noted_sim_res     = cqe->res;
        w->noted_sim_alive   = (cqe->flags & IORING_CQE_F_MORE) != 0;
        if (cqe->res == -ETIME) {
            w->noted_sim_exp++;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_WL_SEND) {
        w->tx_inflight_tag = 0u;
        w->send_queued     = false;
        if (!w->noted_send) {
            w->noted_send     = true;
            w->noted_send_res = cqe->res;
        }
        return;
    }
    if (cqe->user_data == ENG_UD_WL_PONG) {
        w->tx_inflight_tag = 0u;
        w->pong_in_flight  = false;
        if (wl_tx_step_head(w, cqe->res) >= 0) {
            (void)wl_tx_kick(w);
        }
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
        ring_pbuf_finish(w->ring, bid, cqe->flags, cqe->res);
        wl_fail(w, "recvmsg buffer ID out of range");
        return;
    }

    const struct io_uring_recvmsg_out *out = (const struct io_uring_recvmsg_out *)buf;
    uint32_t hdr = (uint32_t)sizeof(*out) + w->control_reserve;
    if ((uint32_t)cqe->res < hdr || (out->flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
        if ((uint32_t)cqe->res > sizeof(*out) && out->controllen > 0) {
            uint32_t avail_ctl = (uint32_t)cqe->res - (uint32_t)sizeof(*out);
            if (avail_ctl > w->control_reserve) {
                avail_ctl = w->control_reserve;
            }
            if (avail_ctl > out->controllen) {
                avail_ctl = out->controllen;
            }
            close_cmsg_fds(buf + sizeof(*out), avail_ctl);
        }
        ring_pbuf_finish(w->ring, bid, cqe->flags, cqe->res);
        w->recv_armed = false;
        wl_fail(w, "recvmsg truncated");
        return;
    }

    uint32_t copied = (uint32_t)cqe->res - hdr;
    if (copied == 0u && out->controllen == 0u) {
        ring_pbuf_finish(w->ring, bid, cqe->flags, cqe->res);
        w->recv_armed = false;
        wl_fail(w, "wayland compositor closed socket (EOF)");
        return;
    }
    if (out->controllen > 0) {
        uint32_t ctl_len = out->controllen <= w->control_reserve ? out->controllen : w->control_reserve;
        park_fds(w, buf + sizeof(*out), ctl_len);
    }
    if (w->failed) {
        ring_pbuf_finish(w->ring, bid, cqe->flags, cqe->res);
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
    ring_pbuf_finish(w->ring, bid, cqe->flags, cqe->res);

    if (!w->failed) {
        (void)wayland_ensure_recv(w);
    }
}

