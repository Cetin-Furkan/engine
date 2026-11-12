#include "ring/internal.h"
#include <stdio.h>

static RingContext s_primary_ring = { .fd = -1, .enter_fd = -1 };

int ring_init(void) {
    return ring_context_init(&s_primary_ring, "primary", true);
}

void ring_shutdown(void) {
    if (s_primary_ring.is_initialized) {
        ring_context_shutdown(&s_primary_ring);
        printf("goodbye from ring\n");
    }
}

RingContext *ring_get_context(void) {
    return &s_primary_ring;
}

void ring_set_dispatcher(RingContext *r, RingCqeFn fn, void *user) {
    if (r == nullptr) {
        return;
    }
    r->default_cqe_fn   = fn;
    r->default_cqe_user = user;
}
