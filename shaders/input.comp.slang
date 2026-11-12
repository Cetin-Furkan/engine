// Word indices match InputPageRing in src/input/page/page.c.
// Word 0 holds head_tag = (seq << 2) | slot_idx. Words [16..79] hold four
// 16-word (64-byte) immutable input slots. Words [80..95] hold two 8-word
// per-frame GPU readback slots selected by pc.readback_slot.

struct Push {
    uint64_t page;
    uint     readback_slot;
    uint     pad1;
};
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    uint* words = (uint*)pc.page;
    uint tag = words[0];
    uint expected_seq = tag >> 2u;
    uint slot = tag & 3u;
    uint base = 16u + slot * 16u;
    uint slot_seq = words[base + 0u];
    uint time_ms  = words[base + 7u];
    uint flags    = words[base + 8u];
    uint mono_lo  = words[base + 11u];
    uint mono_hi  = words[base + 12u];
    uint commit   = words[base + 15u];
    uint stable   = (expected_seq != 0u && slot_seq == expected_seq) ? slot_seq : 0u;
    uint rb = 80u + (pc.readback_slot & 1u) * 8u;
    words[rb + 0u] = stable;
    words[rb + 1u] = stable != 0u ? time_ms : 0u;
    words[rb + 2u] = stable != 0u ? flags : 0u;
    words[rb + 3u] = stable != 0u ? mono_lo : 0u;
    words[rb + 4u] = stable != 0u ? mono_hi : 0u;
    words[rb + 5u] = stable != 0u ? commit : 0u;
}
