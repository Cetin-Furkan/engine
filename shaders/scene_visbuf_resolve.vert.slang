// Slang 2026 Fullscreen Visibility Buffer Resolve Vertex Shader

struct ResolveVSOut {
    float4 sv_pos : SV_Position;
};

[shader("vertex")]
ResolveVSOut main(uint vert_id : SV_VulkanVertexID) {
    ResolveVSOut out;
    float x = (vert_id == 1u) ? 3.0 : -1.0;
    float y = (vert_id == 2u) ? 3.0 : -1.0;
    out.sv_pos = float4(x, y, 0.0, 1.0);
    return out;
}
