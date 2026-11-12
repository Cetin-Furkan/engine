// Engine v0.2 Milestone 5: Oriented 2D Gaussian Splatting (2DGS) Geodetic Surfel Vertex Shader
// Reconstructs exact orthonormal Duff-Frisvad tangent basis (T_u, T_v, N) from 32B BDA surfel,
// applies Mip-2DGS energy-conserving screen-space low-pass dilation, and expands a 6-vertex
// world-space tangent quad (0 affine EWA warping error; hardware Early-Z preserved).

#include "camera.slang"

struct SurfelDrawPush {
    uint64_t surfels_bda;
    uint64_t indirect_bda;
    uint     width;
    uint     height;
    float    elev_base_m;
    float    vertical_scale;
    ViewPush view;
};

[[vk::push_constant]] SurfelDrawPush pc;

struct TerrainSurfel2DGSGpu {
    float center_x;
    float center_y;
    float elev_m;
    uint  nrm_tan_packed;
    uint  scale_uv_f16;
    uint  rgba_opa_packed;
    uint  flags_node_id;
    uint  reserved_pad;
};

struct VSOutput {
    float4 sv_pos     : SV_Position;
    float3 world_nrm  : TEXCOORD0;
    float3 world_pos  : TEXCOORD1;
    float2 local_uv   : TEXCOORD2; // Normalized tangent-plane (u / eff_su, v / eff_sv) in [-2.75, +2.75]
    float4 color_opa  : TEXCOORD3; // rgb = sRGB albedo, a = base_opacity * mip_norm
    float2 view_z_flg : TEXCOORD4; // x = view_pos.z, y = float(surfel_flags)
};

static const float  k_two_pi    = 6.28318530717958647692;
static const float  k_quad_ext  = 2.75;
static const float2 k_quad_uv[6] = {
    float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
    float2( 1.0, -1.0), float2( 1.0,  1.0), float2(-1.0,  1.0)
};

float3 unpack_oct_snorm8_vs(uint packed16) {
    int    qx = (int(packed16 << 24u)) >> 24;
    int    qy = (int((packed16 >> 8u) << 24u)) >> 24;
    float2 e  = clamp(float2(float(qx), float(qy)) * (1.0 / 127.0), float2(-1.0), float2(1.0));
    float3 v  = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        float2 s = float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
        v.xy = (float2(1.0) - abs(v.yx)) * s;
    }
    return normalize(v);
}

void unpack_frisvad_basis_vs(uint nrm_tan, out float3 out_n, out float3 out_tu, out float3 out_tv) {
    float3 n = unpack_oct_snorm8_vs(nrm_tan & 0xFFFFu);
    float  s = (n.z >= 0.0) ? 1.0 : -1.0;
    float  a = -1.0 / (s + n.z);
    float  b = n.x * n.y * a;

    float3 b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    float3 b2 = float3(b, s + n.y * n.y * a, -n.y);

    float  phi = (float((nrm_tan >> 16u) & 0xFFFFu) * (1.0 / 65535.0)) * k_two_pi;
    float  cp  = cos(phi);
    float  sp  = sin(phi);

    out_n  = n;
    out_tu = cp * b1 + sp * b2;
    out_tv = -sp * b1 + cp * b2;
}

[shader("vertex")]
VSOutput main(uint vert_id : SV_VulkanVertexID, uint inst_id : SV_VulkanInstanceID) {
    VSOutput out;

    uint *ind_words  = (uint *)pc.indirect_bda;
    uint  surfel_idx = ind_words[16u + inst_id];

    TerrainSurfel2DGSGpu *surfels = (TerrainSurfel2DGSGpu *)pc.surfels_bda;
    TerrainSurfel2DGSGpu  s       = surfels[surfel_idx];

    float3 world_nrm, tan_u, tan_v;
    unpack_frisvad_basis_vs(s.nrm_tan_packed, world_nrm, tan_u, tan_v);

    float su = max(f16tof32(s.scale_uv_f16 & 0xFFFFu), 0.05);
    float sv = max(f16tof32((s.scale_uv_f16 >> 16u) & 0xFFFFu), 0.05);

    // Mip-2DGS energy-conserving screen-space low-pass filter (~0.42 px Gaussian reconstruction kernel)
    float sigma_aa = 0.42 / max(pc.view.zoom, 1.0e-4);
    float eff_su   = sqrt(su * su + sigma_aa * sigma_aa);
    float eff_sv   = sqrt(sv * sv + sigma_aa * sigma_aa);
    float mip_norm = sqrt((su * sv) / max(eff_su * eff_sv, 1.0e-8));

    float2 corner_uv = k_quad_uv[vert_id % 6u] * k_quad_ext;
    float  center_z  = (s.elev_m - pc.elev_base_m) * pc.vertical_scale;
    float3 center_w  = float3(s.center_x, s.center_y, center_z) + world_nrm * 0.35;

    // Exact planar tangent-space vertex position: affine rasterizer interpolates exact ray-plane (u, v)
    float3 world_pos = center_w + tan_u * (corner_uv.x * eff_su) + tan_v * (corner_uv.y * eff_sv);
    float3 view_pos  = view_of(world_pos, pc.view.q, pc.view.cam);

    float4 ndc = view_ndc(view_pos, pc.view.zoom, pc.width, pc.height, 0.0);
    // +0.45 m view-depth bias prevents self-z-fighting against parent DEM triangles while foreground ridges occlude cleanly
    ndc.z = clamp(0.5 - (view_pos.z + 0.45) / 65536.0, 0.001, 0.999);

    float4 rgba = float4(
        float( s.rgba_opa_packed         & 0xFFu) * (1.0 / 255.0),
        float((s.rgba_opa_packed >>  8u) & 0xFFu) * (1.0 / 255.0),
        float((s.rgba_opa_packed >> 16u) & 0xFFu) * (1.0 / 255.0),
        float((s.rgba_opa_packed >> 24u) & 0xFFu) * (1.0 / 255.0) * mip_norm
    );

    uint s_flags   = (s.flags_node_id >> 16u) & 0xFFFFu;
    out.sv_pos     = ndc;
    out.world_nrm  = world_nrm;
    out.world_pos  = world_pos;
    out.local_uv   = corner_uv;
    out.color_opa  = rgba;
    out.view_z_flg = float2(view_pos.z, float(s_flags));
    return out;
}
