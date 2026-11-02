#ifndef ENGINE_WAYLAND_INTERNAL_H
#define ENGINE_WAYLAND_INTERNAL_H

#include "wayland/wayland.h"

void wl_fail(WaylandContext *w, const char *msg);
void wl_fail_err(WaylandContext *w, const char *what, int err);

void wl_put_u16(unsigned char *p, uint16_t v);
void wl_put_u32(unsigned char *p, uint32_t v);
[[nodiscard]] bool wl_take_u16(const unsigned char **p, const unsigned char *end, uint16_t *o);
[[nodiscard]] bool wl_take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o);
[[nodiscard]] bool wl_take_i32(const unsigned char **p, const unsigned char *end, int32_t *o);
[[nodiscard]] uint32_t wl_min_u32(uint32_t a, uint32_t b);

void wl_free_id(WaylandContext *w, uint32_t id);
// An id that never left the process. The newest fresh id rolls next_id back.
// A recycled id returns to the free stack. Callers release newest first.
void wl_release_unsent(WaylandContext *w, uint32_t id);

void wl_close_feedback_table(WaylandContext *w);
[[nodiscard]] bool wl_append_string(WaylandContext *w, uint32_t obj_id, uint16_t opcode, const char *str);
[[nodiscard]] bool wl_append_registry_bind(WaylandContext *w, uint32_t global_name, const char *iface,
                                           uint32_t version, uint32_t new_id);
[[nodiscard]] bool wl_append_display_sync(WaylandContext *w, uint32_t sync_id);

[[nodiscard]] int wl_arm_pong(WaylandContext *w);
void wl_queue_xdg_pong(WaylandContext *w, uint32_t serial);
[[nodiscard]] int wl_note_send_progress(WaylandContext *w, int32_t res);
[[nodiscard]] bool wl_send_blocked(const WaylandContext *w);
[[nodiscard]] int wl_send_with_fd(WaylandContext *w, int fd_to_send);
[[nodiscard]] int wl_send_batch(WaylandContext *w);
[[nodiscard]] int wl_flush_pending(WaylandContext *w);
[[nodiscard]] int wl_wait_tag(WaylandContext *w, uint64_t tag, struct io_uring_cqe *out);
[[nodiscard]] int wl_finish_roundtrip(WaylandContext *w, int fd_to_send);

void wl_arm_recv(WaylandContext *w);
void wl_handle_dmabuf_feedback(WaylandContext *w, uint16_t opcode, uint16_t size,
                               const unsigned char *p, const unsigned char *end);

void wl_put_timeline_point(uint32_t out[3], uint32_t timeline, uint64_t point);
[[nodiscard]] bool wl_put_min_size(WaylandContext *w, int32_t width, int32_t height);
void wl_geom_box(const WaylandContext *w, uint32_t width, uint32_t height, int32_t out[4]);

// Creates or reuses the surface opaque region inside the current batch.
// *fresh is a new id encoded here and not yet sent. The stored id changes
// only in wl_opaque_keep, after the batch is queued. On failure *fresh is 0
// and any id minted here is already back on the free stack.
[[nodiscard]] bool wl_opaque_prepare(WaylandContext *w, uint32_t surface,
                                     uint32_t *id, uint32_t *rw, uint32_t *rh,
                                     uint32_t width, uint32_t height, uint32_t *fresh);
void wl_opaque_keep(uint32_t *id, uint32_t *rw, uint32_t *rh,
                    uint32_t fresh, uint32_t width, uint32_t height);

[[nodiscard]] bool wl_commit_explicit(WaylandContext *w, uint32_t surface, uint32_t sync_id,
                                      uint32_t buffer, uint32_t width, uint32_t height,
                                      uint32_t acquire_timeline, uint64_t acquire_point,
                                      uint32_t release_timeline, uint64_t release_point,
                                      uint32_t *opaque_id, uint32_t *opaque_w, uint32_t *opaque_h,
                                      uint32_t *fresh);

[[nodiscard]] bool wl_bind_decoration_manager(WaylandContext *w);
[[nodiscard]] bool wl_request_client_decoration(WaylandContext *w);
void wl_on_decoration_event(WaylandContext *w, uint16_t opcode,
                            const unsigned char *p, const unsigned char *end);

#endif
