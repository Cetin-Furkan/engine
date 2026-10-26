// 32 bytes per slot. The first vec4 is emit.rgb and the glow flag.
// The second vec4 is reserved. Every vertex of one object hits the same
// 32 bytes, so the line stays in the cache for the rest of that mesh.

layout(buffer_reference, scalar, buffer_reference_align = 16) buffer MaterialTable {
    vec4 e[];
};

vec3 material_emit(uint64_t addr, uint slot) {
    if (addr == 0ul) {
        return vec3(0.0);
    }
    vec4 rec = MaterialTable(addr).e[slot * 2u];
    return rec.rgb * rec.a;
}
