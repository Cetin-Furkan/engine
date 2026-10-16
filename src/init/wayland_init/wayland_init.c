/* === FILE: ./src/init/wayland_init/wayland_init.c === */

#include "init/wayland_init/wayland_init.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static WaylandContext s_wl_ctx = {0};

static void wl_fail(WaylandContext *w, const char *msg) {
    snprintf(w->err, sizeof w->err, "%s", msg);
    w->failed = true;
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
    if (id == w->registry_id) w->registry_id = 0;
    if (id == w->sync_id)     w->sync_id     = 0;
    if (w->nfree >= ENG_ID_FREE) {
        wl_fail(w, "wayland free-id stack full");
        return;
    }
    w->free_id[w->nfree++] = id;
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
    if (w->send_len + 12u > sizeof(w->send_bytes)) {
        wl_fail(w, "send buffer overflow while encoding wl_display.sync");
        return false;
    }
    unsigned char *dst = w->send_bytes + w->send_len;
    put_u32(dst + 0, w->display_id);
    put_u16(dst + 4, 0u); // wl_display.sync opcode = 0
    put_u16(dst + 6, 12u);
    put_u32(dst + 8, sync_id);
    w->send_len += 12u;
    return true;
}

static void queue_xdg_pong(WaylandContext *w, uint32_t serial) {
    if (w->xdg_wm_base_id == 0) {
        return;
    }
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        return;
    }
    put_u32(w->send_bytes + 0, w->xdg_wm_base_id);
    put_u16(w->send_bytes + 4, 3u); // xdg_wm_base.pong opcode = 3
    put_u16(w->send_bytes + 6, 12u);
    put_u32(w->send_bytes + 8, serial);

    w->send_iov.iov_base   = w->send_bytes;
    w->send_iov.iov_len    = 12u;
    w->send_msg.msg_iov    = &w->send_iov;
    w->send_msg.msg_iovlen = 1u;

    sqe->opcode    = IORING_OP_SENDMSG;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)&w->send_msg;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_SEND;
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
    } else if (strcmp(iface, "wl_subcompositor") == 0) {
        w->subcompositor_global_name = name;
        w->subcompositor_version     = version;
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        w->xdg_wm_base_global_name = name;
        w->xdg_wm_base_version     = version;
    } else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
        w->linux_dmabuf_global_name = name;
        w->linux_dmabuf_version     = version;
    } else if (strcmp(iface, "wp_linux_drm_syncobj_manager_v1") == 0) {
        w->drm_syncobj_global_name = name;
        w->drm_syncobj_version     = version;
    }
}

static void parse_stream(WaylandContext *w) {
    while (w->stream_len >= 8u && !w->failed) {
        const unsigned char *p = w->stream;
        uint32_t object = 0;
        uint16_t opcode = 0;
        uint16_t size   = 0;

        if (!take_u32(&p, w->stream + w->stream_len, &object) ||
            !take_u16(&p, w->stream + w->stream_len, &opcode) ||
            !take_u16(&p, w->stream + w->stream_len, &size)) {
            return;
        }
        if (size < 8u || (size & 3u) != 0u) {
            wl_fail(w, "invalid wayland wire message size");
            return;
        }
        if (w->stream_len < size) {
            return;
        }

        const unsigned char *end = w->stream + size;
        p = w->stream + 8;

        if (object == w->display_id && opcode == 0u) {
            uint32_t bad_id = 0, code = 0, slen = 0;
            if (!take_u32(&p, end, &bad_id) || !take_u32(&p, end, &code) ||
                !take_u32(&p, end, &slen) || slen == 0 || p + slen > end) {
                wl_fail(w, "truncated wl_display.error");
                return;
            }
            snprintf(w->err, sizeof w->err, "wl_display.error object %u code %u: %.*s",
                     bad_id, code, (int)(slen - 1u), (const char *)p);
            w->failed = true;
            return;
        } else if (object == w->display_id && opcode == 1u) {
            uint32_t id = 0;
            if (!take_u32(&p, end, &id)) {
                wl_fail(w, "truncated wl_display.delete_id");
                return;
            }
            free_id(w, id);
        } else if (w->registry_id != 0 && object == w->registry_id && opcode == 0u) {
            uint32_t name = 0, slen = 0, version = 0;
            if (!take_u32(&p, end, &name) || !take_u32(&p, end, &slen) ||
                slen == 0 || slen > ENG_WL_NAME || p + slen > end || p[slen - 1] != 0) {
                wl_fail(w, "invalid wl_registry.global");
                return;
            }
            const char *iface = (const char *)p;
            uint32_t pad = (4u - (slen & 3u)) & 3u;
            p += slen + pad;
            if (!take_u32(&p, end, &version) || p != end) {
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
        } else if (w->xdg_wm_base_id != 0 && object == w->xdg_wm_base_id && opcode == 0u) {
            uint32_t serial = 0;
            if (take_u32(&p, end, &serial)) {
                queue_xdg_pong(w, serial);
            }
        } else if (w->sync_id != 0 && object == w->sync_id && opcode == 0u) {
            uint32_t serial = 0;
            if (size != 12u || !take_u32(&p, end, &serial)) {
                wl_fail(w, "truncated wl_callback.done");
                return;
            }
            w->synced = true;
        }

        memmove(w->stream, w->stream + size, w->stream_len - size);
        w->stream_len -= size;
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
    if (w->failed)     return -EIO;
    if (w->recv_armed) return 0;
    arm_recv(w);
    return w->failed ? -EIO : 0;
}

void wayland_on_cqe(WaylandContext *w, const struct io_uring_cqe *cqe) {
    if (cqe->user_data != ENG_UD_WL_RECV) {
        return;
    }
    w->recv_cqes++;
    w->recv_armed = (cqe->flags & IORING_CQE_F_MORE) != 0;

    if (cqe->res < 0) {
        w->recv_armed = false;
        if (cqe->res != -ENOBUFS) {
            wl_fail_err(w, "recvmsg", cqe->res);
        }
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
    uint64_t deadline = ring_mono_ns() + 2000000000ull;
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
        unsigned n = ring_submit(w->ring);
        int rc = ring_enter(w->ring, n, 1, 200);
        if (rc < 0 && rc != -ETIME) {
            wl_fail_err(w, "io_uring_enter", rc);
            return rc;
        }
    }
}

static int send_wire_batch(WaylandContext *w) {
    if (w->send_len == 0) {
        return 0;
    }
    struct io_uring_sqe *sqe = ring_sqe(w->ring);
    if (sqe == nullptr) {
        wl_fail(w, "SQ full while sending wire batch");
        return -ENOSPC;
    }
    w->send_iov.iov_base   = w->send_bytes;
    w->send_iov.iov_len    = w->send_len;
    w->send_msg.msg_iov    = &w->send_iov;
    w->send_msg.msg_iovlen = 1u;

    sqe->opcode    = IORING_OP_SENDMSG;
    sqe->flags     = IOSQE_FIXED_FILE;
    sqe->fd        = ENG_WL_SOCK_SLOT;
    sqe->addr      = (uint64_t)(uintptr_t)&w->send_msg;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = ENG_UD_WL_SEND;

    struct io_uring_cqe cqe = {0};
    int rc = submit_and_wait_tag(w, ENG_UD_WL_SEND, &cqe);
    if (rc < 0) {
        return rc;
    }
    if (cqe.res != (int32_t)w->send_len) {
        wl_fail(w, "IORING_OP_SENDMSG short write on Wayland socket");
        return -EIO;
    }
    w->send_len = 0;
    return 0;
}

int wayland_init(void) {
    RingContext *r = ring_get_context();
    if (r == nullptr || !r->is_initialized) {
        fprintf(stderr, "[Wayland Error] Primary io_uring must be initialized before wayland_init\n");
        return 1;
    }

    WaylandContext *w = &s_wl_ctx;
    *w = (WaylandContext){
        .ring       = r,
        .display_id = 1,
        .next_id    = 2,
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
        fprintf(stderr, "[Wayland Error] IORING_OP_CONNECT failed\n");
        return 1;
    }

    // 3. Send wl_display.get_registry (opcode 1) + wl_display.sync (opcode 0)
    put_u32(w->send_bytes + 0,  w->display_id);
    put_u16(w->send_bytes + 4,  1u);
    put_u16(w->send_bytes + 6,  12u);
    put_u32(w->send_bytes + 8,  w->registry_id);
    w->send_len = 12u;
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

    // 5. Bind the 5 core interfaces and verify with a second wl_display.sync roundtrip
    w->compositor_id = wayland_alloc_id(w);
    (void)append_registry_bind(w, w->compositor_global_name, "wl_compositor",
                               min_u32(w->compositor_version, 6u), w->compositor_id);

    if (w->subcompositor_global_name != 0) {
        w->subcompositor_id = wayland_alloc_id(w);
        (void)append_registry_bind(w, w->subcompositor_global_name, "wl_subcompositor",
                                   1u, w->subcompositor_id);
    }

    w->xdg_wm_base_id = wayland_alloc_id(w);
    (void)append_registry_bind(w, w->xdg_wm_base_global_name, "xdg_wm_base",
                               min_u32(w->xdg_wm_base_version, 6u), w->xdg_wm_base_id);

    w->linux_dmabuf_id = wayland_alloc_id(w);
    (void)append_registry_bind(w, w->linux_dmabuf_global_name, "zwp_linux_dmabuf_v1",
                               min_u32(w->linux_dmabuf_version, 4u), w->linux_dmabuf_id);

    if (w->drm_syncobj_global_name != 0) {
        w->drm_syncobj_id = wayland_alloc_id(w);
        (void)append_registry_bind(w, w->drm_syncobj_global_name, "wp_linux_drm_syncobj_manager_v1",
                                   1u, w->drm_syncobj_id);
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

    w->is_initialized = true;
    printf("[Wayland] Wire connected (fixed slot 0: %s) | Globals: %u\n",
           w->path, w->globals_seen);
    printf("[Wayland] Bound Object IDs: wl_compositor=%u, wl_subcompositor=%u, xdg_wm_base=%u, linux_dmabuf=%u, drm_syncobj=%u\n",
           w->compositor_id,
           w->subcompositor_id,
           w->xdg_wm_base_id,
           w->linux_dmabuf_id,
           w->drm_syncobj_id);
    return 0;
}

void wayland_shutdown(void) {
    WaylandContext *w = &s_wl_ctx;
    if (!w->is_initialized) {
        return;
    }
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