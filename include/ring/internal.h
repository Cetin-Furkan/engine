#ifndef ENGINE_RING_INTERNAL_H
#define ENGINE_RING_INTERNAL_H

#include "ring/ring.h"

// Shared by the ring translation units. Callers outside src/ring/ use ring.h.

long ring_sys6(long nr, long a, long b, long c, long d, long e, long f);
long ring_register(RingContext *r, unsigned opcode, long arg, unsigned nr);
size_t ring_align_up(size_t n, size_t a);

[[nodiscard]] int ring_register_files(RingContext *r);

[[nodiscard]] int ring_pbuf_register(RingContext *r);
[[nodiscard]] int ring_sim_pbuf_register(RingContext *r);
[[nodiscard]] int ring_fixed_register(RingContext *r);
void ring_release_registered_mem(RingContext *r);

#endif /* ENGINE_RING_INTERNAL_H */
