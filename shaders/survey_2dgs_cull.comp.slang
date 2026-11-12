// Engine v0.2 Milestone 5: Subgroup-Compacted Oriented 2DGS Surfel Culling Compute Shader
// Evaluates 32-byte TerrainSurfel2DGS records via 64-bit Vulkan 1.4 BDA:
//   - Frustum + Elevation AABB + Screen-Space Pixel Footprint Band-Pass Culling
//   - Normal-Cone Back-Face / Extreme Grazing Culling
//   - Deterministic Single-Workgroup WavePrefixCountBits Compaction (0 atomic race flicker!)
//   - Emits VkDrawIndirectCommand (vertexCount = 6, instanceCount = visible_count)

#include "camera.slang"

struct SurfelCullPush {
    uint64_t surfels_bda;
    uint64_t indirect_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    float    elev_base_m;
    float    vertical_scale;
    uint     width;
    uint     height;
    uint     surfel_count;
    uint     flags;
};

[[vk::push_constant]] SurfelCullPush pc;

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

groupshared uint s_wave_counts[32];

float3 unpack_surfel_oct_snorm8(uint packed16) {
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

bool surfel_visible(TerrainSurfel2DGSGpu s) {
    uint opa_u8 = (s.rgba_opa_packed >> 24u) & 0xFFu;
    if (opa_u8 == 0u) {
        return false;
    }

    float su    = max(f16tof32(s.scale_uv_f16 & 0xFFFFu), 0.05);
    float sv    = max(f16tof32((s.scale_uv_f16 >> 16u) & 0xFFFFu), 0.05);
    float max_r = 2.75 * max(su, sv);

    // Screen-space pixel footprint band-pass: skip sub-pixel micro-surfels when zoomed far out
    float px_radius = max_r * pc.zoom;
    uint  s_flags   = (s.flags_node_id >> 16u) & 0xFFFFu;
    float min_px    = (s_flags & 0xEu) != 0u ? 0.65 : 1.15; // Keep GCP/tiepoint markers visible longer
    if (px_radius < min_px || px_radius > 1600.0) {
        return false;
    }

    float4 q   = float4(pc.qx, pc.qy, pc.qz, pc.qw);
    float3 cam = float3(pc.cam_x, pc.cam_y, pc.cam_z);

    // Normal-cone back-face & extreme grazing cull (view +Z points toward viewer)
    float3 world_nrm = unpack_surfel_oct_snorm8(s.nrm_tan_packed & 0xFFFFu);
    float3 view_nrm  = view_rotate(q, world_nrm);
    if (view_nrm.z < 0.05) {
        return false;
    }

    float  wz   = (s.elev_m - pc.elev_base_m) * pc.vertical_scale;
    float3 vpos = view_of(float3(s.center_x, s.center_y, wz), q, cam);
    float4 ndc  = view_ndc(vpos, pc.zoom, pc.width, pc.height, 0.0);
    float  ndc_z = 0.5 - vpos.z / 65536.0;

    float rx_ndc = (max_r * pc.zoom) / max(float(pc.width) * 0.5, 1.0);
    float ry_ndc = (max_r * pc.zoom) / max(float(pc.height) * 0.5, 1.0);
    if (ndc.x + rx_ndc < -1.04 || ndc.x - rx_ndc > 1.04 ||
        ndc.y + ry_ndc < -1.04 || ndc.y - ry_ndc > 1.04 ||
        ndc_z < 0.0005 || ndc_z > 0.9995) {
        return false;
    }
    return true;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID) {
    uint tid = gtid.x;
    if (pc.indirect_bda == 0ull) {
        return;
    }

    uint *ind_words = (uint *)pc.indirect_bda;
    if (pc.surfels_bda == 0ull || pc.surfel_count == 0u || (pc.flags & 1u) == 0u) {
        if (tid == 0u) {
            ind_words[0u] = 0u;
            ind_words[1u] = pc.surfel_count;
            ind_words[2u] = pc.surfel_count;
            ind_words[3u] = 0u;
            ind_words[4u] = 6u;
            ind_words[5u] = 0u;
            ind_words[6u] = 0u;
            ind_words[7u] = 0u;
        }
        return;
    }

    if (tid < 32u) {
        s_wave_counts[tid] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    TerrainSurfel2DGSGpu *surfels = (TerrainSurfel2DGSGpu *)pc.surfels_bda;
    uint wave_size     = max(WaveGetLaneCount(), 1u);
    uint wave_id       = tid / wave_size;
    uint num_waves     = (256u + wave_size - 1u) / wave_size;
    uint total_cap     = pc.surfel_count;
    uint running_total = 0u;

    for (uint base = 0u; base < total_cap; base += 256u) {
        uint idx = base + tid;
        bool vis = (idx < total_cap) ? surfel_visible(surfels[idx]) : false;

        uint lane_off = WavePrefixCountBits(vis);
        uint wave_cnt = WaveActiveCountBits(vis);
        if (WaveIsFirstLane()) {
            s_wave_counts[wave_id] = wave_cnt;
        }
        GroupMemoryBarrierWithGroupSync();

        uint wave_base   = running_total;
        uint batch_count = 0u;
        for (uint w = 0u; w < num_waves; ++w) {
            uint wc = s_wave_counts[w];
            if (w < wave_id) {
                wave_base += wc;
            }
            batch_count += wc;
        }
        running_total += batch_count;

        if (vis) {
            uint dst_slot = wave_base + lane_off;
            // visible_indices[] starts at byte offset 64 (word 16)
            ind_words[16u + dst_slot] = idx;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0u) {
        uint vis_total = running_total;
        ind_words[0u] = vis_total;
        ind_words[1u] = total_cap - vis_total;
        ind_words[2u] = total_cap;
        ind_words[3u] = 0u;
        // VkDrawIndirectCommand at byte offset 16 (words 4..7): 6 vertices per 2DGS tangent quad
        ind_words[4u] = 6u;
        ind_words[5u] = vis_total;
        ind_words[6u] = 0u;
        ind_words[7u] = 0u;
    }
}
