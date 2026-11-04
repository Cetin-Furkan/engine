// DMA-BUF feedback and wl_buffer import.
#include "wayland/internal.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static constexpr uint64_t k_drm_format_mod_invalid = 0x00ffffffffffffffULL;

static bool decode_dev_array(const unsigned char *bytes, uint32_t len,
                             int64_t *out_major, int64_t *out_minor);
static void push_unique_modifier(uint64_t *list, uint32_t *count, uint32_t cap, uint64_t mod);
static bool tranche_matches(const WaylandContext *w, int64_t major, int64_t minor);
static bool tranche_is_producer(const WaylandContext *w);
static bool tranche_is_consumer(const WaylandContext *w);
static void reset_feedback_round(WaylandContext *w);
static void finish_feedback_round(WaylandContext *w);
static void commit_feedback_tranche(WaylandContext *w);


void wl_close_feedback_table(WaylandContext *w) {
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


static bool tranche_matches(const WaylandContext *w, int64_t major, int64_t minor) {
    return w->tranche_has_dev && w->tranche_major == major && w->tranche_minor == minor;
}

// The render node, or the primary node of that same GPU.
static bool tranche_is_producer(const WaylandContext *w) {
    if (tranche_matches(w, w->feedback_target_major, w->feedback_target_minor)) {
        return true;
    }
    if (w->feedback_has_alt &&
        tranche_matches(w, w->feedback_alt_major, w->feedback_alt_minor)) {
        return true;
    }
    return false;
}

// main_device is the GPU the compositor samples with when it cannot scan out
// directly. On a muxless laptop that is the iGPU, not the Vulkan device.
static bool tranche_is_consumer(const WaylandContext *w) {
    if (!w->feedback_has_main_dev || tranche_is_producer(w)) {
        return false;
    }
    return tranche_matches(w, w->feedback_main_major, w->feedback_main_minor);
}

static void reset_feedback_round(WaylandContext *w) {
    w->feedback_done              = false;
    w->feedback_round_sealed      = false;
    w->feedback_cross_device      = false;
    w->scanout_modifier_count     = 0;
    w->all_modifier_count         = 0;
    w->prime_modifier_count       = 0;
    w->feedback_producer_mods     = 0;
    w->feedback_consumer_mods     = 0;
    w->feedback_producer_tranches = 0;
    w->feedback_consumer_tranches = 0;
    w->feedback_other_tranches    = 0;
    w->tranche_has_dev            = false;
    w->tranche_flags              = 0;
    w->tranche_index_count        = 0;
}

static void finish_feedback_round(WaylandContext *w) {
    if (w->feedback_round_sealed) {
        return;
    }
    w->feedback_round_sealed = true;
    w->feedback_producer_mods = w->all_modifier_count;
    w->feedback_consumer_mods = w->prime_modifier_count;
    if (w->all_modifier_count == 0 && w->prime_modifier_count > 0) {
        uint32_t n = w->prime_modifier_count;
        if (n > ENG_WL_MAX_MODIFIERS) {
            n = ENG_WL_MAX_MODIFIERS;
        }
        memcpy(w->all_modifiers, w->prime_modifiers, n * sizeof(uint64_t));
        w->all_modifier_count = n;
        w->feedback_cross_device = true;
    } else {
        w->feedback_cross_device = false;
    }
}

static void commit_feedback_tranche(WaylandContext *w) {
    const bool producer = tranche_is_producer(w);
    const bool consumer = tranche_is_consumer(w);
    if (producer) {
        w->feedback_producer_tranches += 1;
    } else if (consumer) {
        w->feedback_consumer_tranches += 1;
    } else if (w->tranche_has_dev) {
        w->feedback_other_tranches += 1;
    }
    if ((producer || consumer) && w->feedback_table != nullptr) {
        const bool is_scanout = producer && (w->tranche_flags & 1u) != 0u;
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
            if (producer) {
                if (is_scanout) {
                    push_unique_modifier(w->scanout_modifiers, &w->scanout_modifier_count,
                                         ENG_WL_MAX_MODIFIERS, entry->modifier);
                }
                push_unique_modifier(w->all_modifiers, &w->all_modifier_count,
                                     ENG_WL_MAX_MODIFIERS, entry->modifier);
            } else {
                push_unique_modifier(w->prime_modifiers, &w->prime_modifier_count,
                                     ENG_WL_MAX_MODIFIERS, entry->modifier);
            }
        }
    }
    w->tranche_has_dev     = false;
    w->tranche_major       = 0;
    w->tranche_minor       = 0;
    w->tranche_flags       = 0;
    w->tranche_index_count = 0;
}


void wl_handle_dmabuf_feedback(WaylandContext *w, uint16_t opcode, uint16_t size,
                               const unsigned char *p, const unsigned char *end) {
    if (opcode == 0u) {
        finish_feedback_round(w);
        w->feedback_done = true;
        return;
    }
    // If a previous feedback round already completed, a new event starts a
    // fresh compositor tranche update (e.g. direct-scanout or monitor change).
    if (w->feedback_done) {
        reset_feedback_round(w);
    }
    if (opcode == 1u) {
        // format_table is an fd plus a uint byte length. The fd is not in the body.
        uint32_t table_size = 0;
        int fd = -1;
        if (size < 12u || !wl_take_u32(&p, end, &table_size) || !wayland_take_parked_fd(w, &fd)) {
            wl_fail(w, "zwp_linux_dmabuf_feedback_v1.format_table missing size or fd");
            return;
        }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            int err = errno;
            close(fd);
            wl_fail_err(w, "fstat dmabuf format_table", err);
            return;
        }
        if (table_size == 0u ||
            (table_size % (uint32_t)sizeof(WaylandFormatTableEntry)) != 0u ||
            st.st_size < (off_t)table_size) {
            close(fd);
            wl_fail(w, "zwp_linux_dmabuf_feedback_v1.format_table bad length");
            return;
        }
        void *map = mmap(nullptr, table_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) {
            int err = errno;
            close(fd);
            wl_fail_err(w, "mmap dmabuf format_table", err);
            return;
        }
        wl_close_feedback_table(w);
        w->feedback_table_fd    = fd;
        w->feedback_table       = (WaylandFormatTableEntry *)map;
        w->feedback_table_bytes = table_size;
        w->feedback_table_count = table_size / (uint32_t)sizeof(WaylandFormatTableEntry);
    } else if (opcode == 2u) {
        uint32_t arr_len = 0;
        if (!wl_take_u32(&p, end, &arr_len) || (size_t)(end - p) < arr_len) {
            wl_fail(w, "truncated zwp_linux_dmabuf_feedback_v1.main_device");
            return;
        }
        if (decode_dev_array(p, arr_len, &w->feedback_main_major, &w->feedback_main_minor)) {
            w->feedback_has_main_dev = true;
        }
    } else if (opcode == 3u) {
        commit_feedback_tranche(w);
    } else if (opcode == 4u) {
        uint32_t arr_len = 0;
        if (!wl_take_u32(&p, end, &arr_len) || (size_t)(end - p) < arr_len) {
            wl_fail(w, "truncated zwp_linux_dmabuf_feedback_v1.tranche_target_device");
            return;
        }
        w->tranche_has_dev = decode_dev_array(
            p, arr_len, &w->tranche_major, &w->tranche_minor);
    } else if (opcode == 5u) {
        uint32_t arr_len = 0;
        if (!wl_take_u32(&p, end, &arr_len) || (size_t)(end - p) < arr_len || (arr_len & 1u) != 0u) {
            wl_fail(w, "invalid zwp_linux_dmabuf_feedback_v1.tranche_formats");
            return;
        }
        uint32_t count = arr_len / 2u;
        if (w->tranche_index_count > ENG_WL_MAX_INDICES ||
            count > ENG_WL_MAX_INDICES - w->tranche_index_count) {
            wl_fail(w, "zwp_linux_dmabuf_feedback_v1 tranche index list exceeds capacity");
            return;
        }
        memcpy(w->tranche_indices + w->tranche_index_count, p, count * sizeof(uint16_t));
        w->tranche_index_count += count;
    } else if (opcode == 6u) {
        uint32_t flags = 0;
        if (!wl_take_u32(&p, end, &flags)) {
            wl_fail(w, "truncated zwp_linux_dmabuf_feedback_v1.tranche_flags");
            return;
        }
        w->tranche_flags = flags;
    }
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
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    uint32_t mark = w->send_len;
    if (w->feedback_id != 0) {
        if (!wayland_put_raw(w, w->feedback_id, 0u, nullptr, 0u)) {
            wayland_rewind_send(w, mark);
            return -EIO;
        }
        w->feedback_id = 0;
    }
    wl_close_feedback_table(w);

    uint32_t fb_id = wayland_alloc_id(w);
    if (fb_id == 0) {
        wayland_rewind_send(w, mark);
        return -EIO;
    }
    w->feedback_id            = fb_id;
    w->feedback_target_major  = target_major;
    w->feedback_target_minor  = target_minor;
    w->feedback_alt_major     = alt_major;
    w->feedback_alt_minor     = alt_minor;
    w->feedback_has_alt       = has_alt;
    w->feedback_has_main_dev  = false;
    reset_feedback_round(w);

    // zwp_linux_dmabuf_v1.get_surface_feedback (request 3)
    uint32_t fb_payload[2] = { w->feedback_id, w->surface_id };
    if (!wayland_put_raw(w, w->linux_dmabuf_id, 3u, fb_payload, sizeof(fb_payload))) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, fb_id);
        return -EIO;
    }

    if (wl_send_batch(w) < 0) {
        if (!w->failed && w->send_off == 0) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, fb_id);
        }
        fprintf(stderr, "[Wayland Error] get_surface_feedback send failed: %s\n", w->err);
        return -EIO;
    }

    struct io_uring_cqe cqe = {0};
    while (!w->feedback_done && !w->failed) {
        if (wl_wait_tag(w, ENG_UD_WL_RECV, &cqe) < 0) {
            break;
        }
        wayland_on_cqe(w, &cqe);
    }

    // Keep w->feedback_id and w->feedback_table alive for the life of the surface
    // so dynamic compositor scanout/tranche updates are received automatically.
    if (w->failed || !w->feedback_done) {
        if (!w->failed && w->feedback_id != 0) {
            (void)wayland_put_raw(w, w->feedback_id, 0u, nullptr, 0u);
            w->feedback_id = 0;
            (void)wl_send_batch(w);
        }
        wl_close_feedback_table(w);
        fprintf(stderr, "[Wayland Error] Surface DMA-BUF feedback failed: %s\n", w->err);
        return -EIO;
    }

    uint32_t producer_mods = w->all_modifier_count;
    uint32_t consumer_mods = w->prime_modifier_count;
    finish_feedback_round(w);
    producer_mods = w->feedback_producer_mods;
    consumer_mods = w->feedback_consumer_mods;

    if (w->feedback_has_main_dev) {
        printf("[DMA-BUF] Compositor main device %lld:%lld, render %lld:%lld",
               (long long)w->feedback_main_major, (long long)w->feedback_main_minor,
               (long long)target_major, (long long)target_minor);
    } else {
        printf("[DMA-BUF] Compositor sent no main device, render %lld:%lld",
               (long long)target_major, (long long)target_minor);
    }
    if (has_alt) {
        printf(", primary %lld:%lld", (long long)alt_major, (long long)alt_minor);
    }
    printf(". Producer tranches %u (%u ARGB8888), main-device tranches %u (%u ARGB8888), other tranches %u.\n",
           w->feedback_producer_tranches, producer_mods,
           w->feedback_consumer_tranches, consumer_mods,
           w->feedback_other_tranches);
    if (w->feedback_cross_device) {
        printf("[DMA-BUF] Render node has no direct tranche. Frames are drawn in device-local memory and copied into a buffer main device %lld:%lld can import.\n",
               (long long)w->feedback_main_major, (long long)w->feedback_main_minor);
    }
    fflush(stdout);

    if (w->all_modifier_count == 0) {
        fprintf(stderr,
                "[Wayland Error] No DRM_FORMAT_ARGB8888 modifiers for render node %lld:%lld",
                (long long)target_major, (long long)target_minor);
        if (has_alt) {
            fprintf(stderr, " or %lld:%lld", (long long)alt_major, (long long)alt_minor);
        }
        if (w->feedback_has_main_dev) {
            fprintf(stderr, " or compositor main device %lld:%lld",
                    (long long)w->feedback_main_major, (long long)w->feedback_main_minor);
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
    if (wl_flush_pending(w) < 0) {
        return -EIO;
    }
    uint32_t mark      = w->send_len;
    uint32_t prev_buf  = w->buffer_id;
    uint32_t params_id = wayland_alloc_id(w);
    uint32_t buffer_id = wayland_alloc_id(w);
    if (params_id == 0 || buffer_id == 0) {
        wl_release_unsent(w, buffer_id);
        wl_release_unsent(w, params_id);
        return -EIO;
    }
    w->buffer_params_id = params_id;
    w->buffer_id        = buffer_id;
    w->buffer_released  = false;

    uint32_t add_payload[5] = {
        0u,                                 // plane_idx = 0
        offset,                             // offset
        stride,                             // stride = row_pitch
        (uint32_t)(modifier >> 32u),        // modifier_hi
        (uint32_t)(modifier & 0xffffffffu), // modifier_lo
    };
    uint32_t immed_payload[5] = {
        buffer_id,
        width,
        height,
        drm_fourcc,
        0u, // flags = 0
    };
    if (!wayland_put_raw(w, w->linux_dmabuf_id, 1u, &params_id, sizeof(params_id)) ||
        !wayland_put_raw(w, params_id, 1u, add_payload, sizeof(add_payload)) ||
        !wayland_put_raw(w, params_id, 3u, immed_payload, sizeof(immed_payload)) ||
        !wayland_put_raw(w, params_id, 0u, nullptr, 0u)) {
        wayland_rewind_send(w, mark);
        wl_release_unsent(w, buffer_id);
        wl_release_unsent(w, params_id);
        w->buffer_id = prev_buf;
        fprintf(stderr, "[Wayland Error] DMA-BUF create_immed encode failed: %s\n", w->err);
        return -EIO;
    }

    if (wl_send_with_fd(w, dmabuf_fd) < 0) {
        if (!w->failed && w->send_off == 0) {
            wayland_rewind_send(w, mark);
            wl_release_unsent(w, buffer_id);
            wl_release_unsent(w, params_id);
        }
        w->buffer_id = prev_buf;
        fprintf(stderr, "[Wayland Error] DMA-BUF create_immed batch failed: %s\n", w->err);
        return -EIO;
    }

    *out_buffer_id = buffer_id;
    return 0;
}