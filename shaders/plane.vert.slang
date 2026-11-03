// One triangle covers the framebuffer. The metric is computed in the
// fragment shader from FragCoord, so yaw and pitch never tilt the sheet
// and the lines have no far edge to fall off.

static const float2 k_ndc[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct VSOutput {
    float4 sv_pos : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID) {
    VSOutput out;
    out.sv_pos = float4(k_ndc[vert_id], 1.0, 1.0);
    return out;
}
