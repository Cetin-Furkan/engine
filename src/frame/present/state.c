#include "frame/frame_int.h"

// The one present record. Every present file names it through the s_* macros.

FrameState frame_state = {
    .drm_fd = -1,
    .slot = {
        [0] = { .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 },
        [1] = { .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 },
    },
    .resize_fresh = { .image = { .dmabuf_fd = -1 }, .release_eventfd = -1 },
    .stuck_release_fd = -1,
    .release_fd = -1,
    .inotify_fd = -1,
    .inotify_wd = -1,
};
