// Slang 2026 GPU-Math Compute Shader: Resolves newly streamed 256x256 SSD tiles via
// RelPtr2D block offsets and computes 3x3 Scharr surface gradients, Laplacian
// ridge/canyon curvature, and 4-direction horizon ambient occlusion into the
// alpha channel of imagery_rgba8.

import terrain_math;

struct FilterPush {
    uint64_t dir_bda;
    uint64_t rel_tile_ptr;
    float    sample_spacing_m;
    uint     slot_index;
    uint     pad0;
    uint     pad1;
};

[[vk::push_constant]] FilterPush pc;

groupshared uint s_ao[8][8];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    uint ax = tid.x;
    uint ay = tid.y;
    if (ax >= k_ao_dim || ay >= k_ao_dim || pc.dir_bda == 0ull || pc.rel_tile_ptr == 0ull) {
        return;
    }

    uint64_t tile_bda = relptr2d_resolve_gpu(pc.dir_bda, pc.rel_tile_ptr);
    if (tile_bda == 0ull) {
        return;
    }

    // Map 128x128 AO grid coordinates onto the 256x256 .epack elevation posts (0 -> 0, 127 -> 255)
    uint sx = min(uint(float(ax) * (255.0 / 127.0) + 0.5), k_tile_dim - 1u);
    uint sy = min(uint(float(ay) * (255.0 / 127.0) + 0.5), k_tile_dim - 1u);

    uint xm = sx > 0u ? sx - 1u : 0u;
    uint xp = sx + 1u < k_tile_dim ? sx + 1u : k_tile_dim - 1u;
    uint ym = sy > 0u ? sy - 1u : 0u;
    uint yp = sy + 1u < k_tile_dim ? sy + 1u : k_tile_dim - 1u;

    float h00 = tile_load_height_m(tile_bda, xm, ym);
    float h10 = tile_load_height_m(tile_bda, sx, ym);
    float h20 = tile_load_height_m(tile_bda, xp, ym);
    float h01 = tile_load_height_m(tile_bda, xm, sy);
    float hc  = tile_load_height_m(tile_bda, sx, sy);
    float h21 = tile_load_height_m(tile_bda, xp, sy);
    float h02 = tile_load_height_m(tile_bda, xm, yp);
    float h12 = tile_load_height_m(tile_bda, sx, yp);
    float h22 = tile_load_height_m(tile_bda, xp, yp);

    float step_m = max(pc.sample_spacing_m, 1.0);
    // 3x3 Scharr operator for rotationally invariant elevation gradient
    float dzdx = ((3.0 * h20 + 10.0 * h21 + 3.0 * h22) - (3.0 * h00 + 10.0 * h01 + 3.0 * h02)) / (32.0 * step_m);
    float dzdy = ((3.0 * h02 + 10.0 * h12 + 3.0 * h22) - (3.0 * h00 + 10.0 * h10 + 3.0 * h20)) / (32.0 * step_m);
    float grad = sqrt(dzdx * dzdx + dzdy * dzdy);

    // Multi-radius horizon occlusion for deep mountain canyons & gullies
    uint xm3 = sx >= 3u ? sx - 3u : 0u;
    uint xp3 = sx + 3u < k_tile_dim ? sx + 3u : k_tile_dim - 1u;
    uint ym3 = sy >= 3u ? sy - 3u : 0u;
    uint yp3 = sy + 3u < k_tile_dim ? sy + 3u : k_tile_dim - 1u;
    float h_ring = 0.25 * (
        tile_load_height_m(tile_bda, xm3, sy) +
        tile_load_height_m(tile_bda, xp3, sy) +
        tile_load_height_m(tile_bda, sx, ym3) +
        tile_load_height_m(tile_bda, sx, yp3)
    );
    float canyon_depth = clamp((h_ring - hc) / (3.0 * step_m), -0.6, 0.8);

    // Laplacian curvature (positive on exposed ridges, negative in gullies)
    float laplacian = (h01 + h21 + h10 + h12 - 4.0 * hc) / step_m;
    float ao = saturate(0.84 - 0.14 * grad - 0.22 * canyon_depth - 0.08 * clamp(-laplacian, -1.2, 1.2));

    s_ao[gtid.y][gtid.x] = uint(clamp(ao * 255.0 + 0.5, 32.0, 255.0));
    GroupMemoryBarrierWithGroupSync();

    // Coalesce 4 adjacent uint8_t AO values into a single aligned 32-bit BDA store (zero atomics)
    if ((gtid.x & 3u) == 0u) {
        uint packed = s_ao[gtid.y][gtid.x + 0u]
                    | (s_ao[gtid.y][gtid.x + 1u] << 8u)
                    | (s_ao[gtid.y][gtid.x + 2u] << 16u)
                    | (s_ao[gtid.y][gtid.x + 3u] << 24u);
        uint *words = (uint *)tile_bda;
        uint linear_idx = ay * k_ao_dim + ax;
        words[k_ao_word_offset + (linear_idx >> 2u)] = packed;
    }
}
