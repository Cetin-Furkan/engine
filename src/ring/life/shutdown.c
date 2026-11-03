#include "ring/internal.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void ring_release_registered_mem(RingContext *r) {
    if (r->fixed_on) {
        long rc = ring_register(r, IORING_UNREGISTER_BUFFERS, 0, 0);
        if (rc < 0) {
            fprintf(stderr, "[Ring Error] IORING_UNREGISTER_BUFFERS: %s\n", strerror((int)-rc));
        }
        r->fixed_on = false;
    }
    if (r->sim_pbuf_on) {
        struct io_uring_buf_reg unreg = { .bgid = ENG_SIM_BGID };
        long rc = ring_register(r, IORING_UNREGISTER_PBUF_RING, (long)&unreg, 1);
        if (rc < 0) {
            fprintf(stderr, "[Ring Error] unregister sim buffers: %s\n", strerror((int)-rc));
        }
        r->sim_pbuf_on = false;
    }
    if (r->pbuf_on) {
        struct io_uring_buf_reg unreg = { .bgid = ENG_PBUF_BGID };
        long rc = ring_register(r, IORING_UNREGISTER_PBUF_RING, (long)&unreg, 1);
        if (rc < 0) {
            fprintf(stderr, "[Ring Error] unregister recv buffers: %s\n", strerror((int)-rc));
        }
        r->pbuf_on = false;
    }
}

void ring_context_shutdown(RingContext *r) {
    if (r == nullptr) {
        return;
    }
    if (r->ring_registered && r->enter_fd >= 0) {
        ring_release_registered_mem(r);
        // REGISTERED_FD_ONLY returns a slot in registered_rings[], often 0.
        // That index is not a process fd. close(0) would close stdin and
        // leave the ring's NO_MMAP pages pinned.
        struct io_uring_rsrc_update up = {
            .offset = (uint32_t)r->enter_fd,
        };
        long rc = ring_register(r, IORING_UNREGISTER_RING_FDS, (long)&up, 1);
        if (rc < 0) {
            fprintf(stderr, "[Ring Error] IORING_UNREGISTER_RING_FDS: %s\n", strerror((int)-rc));
        }
        r->ring_registered = false;
        r->enter_fd = -1;
    }
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
    if (r->arena != nullptr && r->arena_len > 0) {
        munmap(r->arena, r->arena_len);
    }
    *r = (RingContext){ .fd = -1, .enter_fd = -1 };
}
