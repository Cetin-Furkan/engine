#ifndef ENGINE_FRAME_HANDLE_H
#define ENGINE_FRAME_HANDLE_H

#include <stdint.h>

// Generation-tagged slot handle: (generation << 8) | (index + 1).
// Zero is never valid. A stale handle stops resolving after the slot's
// generation moves, so an async completion cannot name a replacement image.
typedef uint32_t SlotRef;

static inline SlotRef slot_ref_pack(uint32_t generation, uint32_t index_plus_one) {
    if (generation == 0 || index_plus_one == 0 || index_plus_one > 0xffu) {
        return 0;
    }
    return (generation << 8) | index_plus_one;
}

static inline uint32_t slot_next_generation(uint32_t generation) {
    generation += 1u;
    return generation != 0 ? generation : 1u;
}

#endif /* ENGINE_FRAME_HANDLE_H */
