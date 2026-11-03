// Word indices are InputPage in src/input/page/page.c. The first line is the
// seat. The second line is what this dispatch loaded. A matching even
// sequence is a stable sample. An odd or torn sequence stores zero.
// Word 15 is the cursor shape and resize edge. It lands in word 21.

struct Push {
    uint64_t page;
    uint     pad0;
    uint     pad1;
};
[[vk::push_constant]] Push pc;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    uint* words = (uint*)pc.page;
    uint seq1 = words[0];
    uint time_ms = words[7];
    uint flags = words[8];
    uint mono_lo = words[11];
    uint mono_hi = words[12];
    uint commit = words[15];
    uint seq2 = words[0];
    uint stable = (seq1 == seq2 && (seq1 & 1u) == 0u) ? seq1 : 0u;
    words[16] = stable;
    words[17] = stable != 0u ? time_ms : 0u;
    words[18] = stable != 0u ? flags : 0u;
    words[19] = stable != 0u ? mono_lo : 0u;
    words[20] = stable != 0u ? mono_hi : 0u;
    words[21] = stable != 0u ? commit : 0u;
}
