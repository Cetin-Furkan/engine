#include "frame/frame_int.h"
#include <stdio.h>

// Focused regression test for generation-tagged slot handles.
// Exercises the exact inlines slot.c uses (slot_ref_pack,
// slot_next_generation, slot_resolve): a handle taken before a
// blank/reuse must stop resolving, a live handle must resolve to the
// same slot, and handle 0 / generation 0 must never resolve.

FrameState frame_state;

static int s_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                               \
        }                                                               \
    } while (0)

int main(void) {
    FrameSlot slots[2] = {0};
    FrameSlot orphans[2] = {0};
    slots[0].generation = 1;
    slots[1].generation = 7;
    orphans[0].generation = 3;

    // Live handles resolve to the same storage.
    CHECK(slot_resolve(slot_ref_pack(1, 1), slots, orphans, 1) == &slots[0]);
    CHECK(slot_resolve(slot_ref_pack(7, 2), slots, orphans, 1) == &slots[1]);
    CHECK(slot_resolve(slot_ref_pack(3, 3), slots, orphans, 1) == &orphans[0]);

    // Null and never-valid handles do not resolve.
    CHECK(slot_resolve(0, slots, orphans, 1) == nullptr);
    CHECK(slot_ref_pack(0, 1) == 0);
    CHECK(slot_ref_pack(1, 0) == 0);

    // Stale handle: blanking bumps the generation, old ref dies.
    SlotRef stale = slot_ref_pack(slots[0].generation, 1);
    slots[0].generation = slot_next_generation(slots[0].generation);
    CHECK(slot_resolve(stale, slots, orphans, 1) == nullptr);
    CHECK(slot_resolve(slot_ref_pack(slots[0].generation, 1),
                       slots, orphans, 1) == &slots[0]);

    // Generation wrap never lands on 0 (0 is the poison value).
    CHECK(slot_next_generation(0xffffffffu) == 1);

    // Out-of-range orphan index does not resolve.
    CHECK(slot_resolve(slot_ref_pack(3, 4), slots, orphans, 1) == nullptr);

    if (s_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("test_handles: all checks passed\n");
    return 0;
}
