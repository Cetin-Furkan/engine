// 32 bytes per slot. The first float4 is emit.rgb and the glow flag.
// The second float4 is reserved. Every vertex of one object hits the same
// 32 bytes, so the line stays in the cache for the rest of that mesh.

struct MaterialTable {
    float4 e[];
};

float3 material_emit(uint64_t addr, uint slot) {
    if (addr == 0ul) {
        return float3(0.0);
    }
    MaterialTable* tbl = (MaterialTable*)addr;
    float4 rec = tbl->e[slot * 2u];
    return rec.rgb * rec.a;
}
