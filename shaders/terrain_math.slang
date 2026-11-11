// Shared Slang 2026 GPU-Math Module for 2D Self-Relative Pointers (RelPtr2D),
// Lilliput 64-Bit Unified Tile Headers, and 4KB-Aligned 256x256 BDA Terrain Tiles.

module terrain_math;

public static const uint k_tile_dim             = 256u;
public static const uint k_tile_samples         = 65536u;
public static const uint k_tile_bytes           = 102400u; // .epack v1: 25 x 4 KiB (100 KiB, 3.84x smaller)
public static const uint k_tile_words           = 25600u;
public static const uint k_patch_grid           = 32u;
public static const uint k_color_grid           = 64u;
public static const uint k_ao_dim               = 128u;
public static const uint k_border_h_word_offset = 12288u;  // 1024 * 12 words (48 KiB)
public static const uint k_border_c_word_offset = 12800u;  // + 512 words (2 KiB)
public static const uint k_color_word_offset    = 13312u;  // + 512 words (2 KiB)
public static const uint k_ao_word_offset       = 21504u;  // + 8192 words (32 KiB)
public static const uint k_dir_bytes            = 256u;
public static const uint k_node_bytes           = 64u;
public static const uint k_skirt_quads          = 64u;
public static const uint k_skirt_verts          = 384u;

public struct GpuMathQuadNodeGpu {
    public uint64_t rel_ssd_payload;
    public uint64_t rel_gpu_payload;
    public uint64_t rel_children;
    public uint64_t header;
    public float    world_min_x;
    public float    world_min_y;
    public float    world_max_x;
    public float    world_max_y;
    public uint     packed_ids0; // [15:0]=node_index, [31:16]=parent_index
    public uint     packed_ids1; // [15:0]=gpu_slot, [23:16]=lod, [31:24]=grid_x
    public uint     packed_ids2; // [7:0]=grid_y, [15:8]=resident
    public float    elev_mean_m;
};

// Resolve a 64-bit 2D Self-Relative Spatial Block Pointer (RelPtr2D)
// directly in GPU register math without any pointer fixup table:
//   Addr_target = self_base_addr + (sign_extend_20(bits[51:32]) << 12)
public uint64_t relptr2d_resolve_gpu(uint64_t self_base_addr, uint64_t rel_ptr) {
    if (rel_ptr == 0ull) {
        return 0ull;
    }
    uint raw20 = uint((rel_ptr >> 32) & 0xFFFFFull);
    int signed_delta = (raw20 & 0x80000u) != 0u
        ? int(raw20 | 0xFFF00000u)
        : int(raw20);
    int64_t byte_offset = int64_t(signed_delta) << 12;
    return uint64_t(int64_t(self_base_addr) + byte_offset);
}

// Address of quadtree node `idx` inside the BDA archive header (immediately after 256B PULSE window)
public GpuMathQuadNodeGpu load_quad_node(uint64_t dir_bda, uint idx) {
    GpuMathQuadNodeGpu *nodes = (GpuMathQuadNodeGpu *)(dir_bda + uint64_t(k_dir_bytes));
    return nodes[idx];
}

// Read active_leaves[leaf_i] from the 256-byte PULSE Active GPU Window (offset 112)
public uint load_active_leaf_index(uint64_t dir_bda, uint leaf_i) {
    uint *packed_leaves = (uint *)(dir_bda + 112ull);
    uint word = packed_leaves[leaf_i >> 1u];
    return (leaf_i & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
}

// Read compacted post-cull visible_nodes[inst_i] from the BDA indirect draw buffer (offset 32)
public uint load_visible_leaf_index(uint64_t dir_bda, uint64_t indirect_bda, uint inst_i) {
    if (indirect_bda != 0ull) {
        uint *visible_nodes = (uint *)(indirect_bda + 32ull);
        return visible_nodes[inst_i];
    }
    return load_active_leaf_index(dir_bda, inst_i);
}

// Read per-instance dynamic vertex count from cmds[inst_i].vertex_count (word 72 + inst_i * 4)
public uint load_visible_leaf_quads_side(uint64_t indirect_bda, uint inst_i, uint default_quads) {
    if (indirect_bda != 0ull) {
        uint *words = (uint *)indirect_bda;
        uint vcnt = words[72u + inst_i * 4u];
        if (vcnt <= (16u * 16u + k_skirt_quads) * 6u) {
            return 16u;
        }
        if (vcnt <= (32u * 32u + k_skirt_quads) * 6u) {
            return 32u;
        }
        if (vcnt <= (64u * 64u + k_skirt_quads) * 6u) {
            return 64u;
        }
    }
    return default_quads;
}

// Branchless interior post height decode (valid for sx, sy in 1..254)
float epack_load_height_interior(uint *words, uint cx, uint cy) {
    uint px    = cx >> 3u;
    uint py    = cy >> 3u;
    uint lx    = cx & 7u;
    uint ly    = cy & 7u;
    uint bit   = (ly << 3u) | lx;
    uint pbase = ((py << 5u) + px) * 12u;

    uint w0 = words[pbase + 0u];
    uint w1 = words[pbase + 1u];
    int  slope_x_i16 = int(w0) >> 16;
    int  slope_y_i16 = int(w1 << 16u) >> 16;

    uint lo4 = (words[pbase + 2u + ly] >> (lx << 2u)) & 0xFu;
    uint hi1 = (words[pbase + 10u + (bit >> 5u)] >> (bit & 31u)) & 1u;
    uint q5  = lo4 | (hi1 << 4u);

    float base_m = float(w0 & 0xFFFFu) * 0.125 - 2048.0;
    float sx_m   = float(slope_x_i16) * (1.0 / 256.0);
    float sy_m   = float(slope_y_i16) * (1.0 / 256.0);
    float step_m = float(w1 >> 16u) * (1.0 / 1024.0);
    return base_m + sx_m * float(lx) + sy_m * float(ly) + step_m * float(q5);
}

// Decode one elevation post (meters) directly from a 100 KiB .epack v1 BDA tile block:
//   - Perimeter posts (sx,sy in {0,255}) read the lossless u16 seam ring (0.0000 m seam error)
//   - Interior posts decode the 48-byte 8x8 planar + 5-bit residual patch in O(1) register math
public float tile_load_height_m(uint64_t tile_bda, uint sx, uint sy) {
    uint cx = min(sx, k_tile_dim - 1u);
    uint cy = min(sy, k_tile_dim - 1u);
    uint *words = (uint *)tile_bda;

    if (cx == 0u || cx == (k_tile_dim - 1u) || cy == 0u || cy == (k_tile_dim - 1u)) {
        bool on_y0 = (cy == 0u);
        bool on_y1 = (cy == (k_tile_dim - 1u));
        bool on_x0 = (cx == 0u);
        uint edge  = on_y0 ? 0u : (on_y1 ? 1u : (on_x0 ? 2u : 3u));
        uint coord = (on_y0 || on_y1) ? cx : cy;
        uint bidx  = edge * k_tile_dim + coord;
        uint bw    = words[k_border_h_word_offset + (bidx >> 1u)];
        uint u16_v = (bidx & 1u) != 0u ? (bw >> 16u) : (bw & 0xFFFFu);
        return float(u16_v) * 0.0625;
    }
    return epack_load_height_interior(words, cx, cy);
}

// Fast 4-tap bilinear elevation + analytical (dh/du, dh/dv) gradient
// Branchless interior decode avoids SIMD16/32 subgroup divergence across 8x8 patch borders.
public float tile_sample_bilinear_height_grad_m(uint64_t tile_bda, float2 uv, out float2 out_dh_duv) {
    float scale = float(k_tile_dim - 1u);
    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * scale;
    uint x0 = uint(floor(tc.x));
    uint y0 = uint(floor(tc.y));
    uint x1 = min(x0 + 1u, k_tile_dim - 1u);
    uint y1 = min(y0 + 1u, k_tile_dim - 1u);
    float fx = tc.x - float(x0);
    float fy = tc.y - float(y0);
    uint *words = (uint *)tile_bda;

    float h00, h10, h01, h11;
    if (x0 > 0u && x1 < (k_tile_dim - 1u) && y0 > 0u && y1 < (k_tile_dim - 1u)) {
        h00 = epack_load_height_interior(words, x0, y0);
        h10 = epack_load_height_interior(words, x1, y0);
        h01 = epack_load_height_interior(words, x0, y1);
        h11 = epack_load_height_interior(words, x1, y1);
    } else {
        h00 = tile_load_height_m(tile_bda, x0, y0);
        h10 = tile_load_height_m(tile_bda, x1, y0);
        h01 = tile_load_height_m(tile_bda, x0, y1);
        h11 = tile_load_height_m(tile_bda, x1, y1);
    }

    float hx0 = lerp(h00, h10, fx);
    float hx1 = lerp(h01, h11, fx);
    out_dh_duv = float2(
        lerp(h10 - h00, h11 - h01, fy) * scale,
        (hx1 - hx0) * scale
    );
    return lerp(hx0, hx1, fy);
}

// Catmull-Rom 1D cubic weights and analytical first derivatives
float4 catmull_rom_weights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return float4(
        -0.5 * t + t2 - 0.5 * t3,
         1.0 - 2.5 * t2 + 1.5 * t3,
         0.5 * t + 2.0 * t2 - 1.5 * t3,
        -0.5 * t2 + 0.5 * t3
    );
}

float4 catmull_rom_deriv_weights(float t) {
    float t2 = t * t;
    return float4(
        -0.5 + 2.0 * t - 1.5 * t2,
        -5.0 * t + 4.5 * t2,
         0.5 + 4.0 * t - 4.5 * t2,
        -1.0 * t + 1.5 * t2
    );
}

// 16-tap C1-continuous Catmull-Rom bicubic elevation + exact analytical (dh/du, dh/dv) gradient
// Uniform branch-free 2x2 EpackElevPatch prefetch eliminates SIMD16 subgroup divergence and GRF spills.
public float tile_sample_bicubic_height_grad_m(uint64_t tile_bda, float2 uv, out float2 out_dh_duv) {
    float scale = float(k_tile_dim - 1u);
    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * scale;
    int ix = int(floor(tc.x));
    int iy = int(floor(tc.y));
    float fx = tc.x - float(ix);
    float fy = tc.y - float(iy);

    float4 wx  = catmull_rom_weights(fx);
    float4 wy  = catmull_rom_weights(fy);
    float4 dwx = catmull_rom_deriv_weights(fx);
    float4 dwy = catmull_rom_deriv_weights(fy);
    uint *words = (uint *)tile_bda;

    float h_sum  = 0.0;
    float dx_sum = 0.0;
    float dy_sum = 0.0;

    if (ix >= 2 && ix <= 252 && iy >= 2 && iy <= 252) {
        uint x_s = uint(ix - 1);
        uint y_s = uint(iy - 1);
        uint px0 = x_s >> 3u;
        uint px1 = (x_s + 3u) >> 3u;
        uint py0 = y_s >> 3u;
        uint py1 = (y_s + 3u) >> 3u;

        uint p00 = ((py0 << 5u) + px0) * 12u;
        uint p10 = ((py0 << 5u) + px1) * 12u;
        uint p01 = ((py1 << 5u) + px0) * 12u;
        uint p11 = ((py1 << 5u) + px1) * 12u;

        uint w0_00 = words[p00 + 0u], w1_00 = words[p00 + 1u], h0_00 = words[p00 + 10u], h1_00 = words[p00 + 11u];
        uint w0_10 = words[p10 + 0u], w1_10 = words[p10 + 1u], h0_10 = words[p10 + 10u], h1_10 = words[p10 + 11u];
        uint w0_01 = words[p01 + 0u], w1_01 = words[p01 + 1u], h0_01 = words[p01 + 10u], h1_01 = words[p01 + 11u];
        uint w0_11 = words[p11 + 0u], w1_11 = words[p11 + 1u], h0_11 = words[p11 + 10u], h1_11 = words[p11 + 11u];

        [unroll]
        for (uint ry = 0u; ry < 4u; ++ry) {
            uint cy    = y_s + ry;
            bool y_hi  = (cy >> 3u) != py0;
            uint ly    = cy & 7u;
            uint hi_sh = (ly & 3u) << 3u;

            uint pL   = y_hi ? p01 : p00;
            uint pR   = y_hi ? p11 : p10;
            uint w0_L = y_hi ? w0_01 : w0_00;
            uint w1_L = y_hi ? w1_01 : w1_00;
            uint w0_R = y_hi ? w0_11 : w0_10;
            uint w1_R = y_hi ? w1_11 : w1_10;
            uint hi_L = (ly >= 4u) ? (y_hi ? h1_01 : h1_00) : (y_hi ? h0_01 : h0_00);
            uint hi_R = (ly >= 4u) ? (y_hi ? h1_11 : h1_10) : (y_hi ? h0_11 : h0_10);

            uint lo4_L = words[pL + 2u + ly];
            uint lo4_R = words[pR + 2u + ly];

            float base_L = float(w0_L & 0xFFFFu) * 0.125 - 2048.0 + float(int(w1_L << 16u) >> 16) * (1.0 / 256.0) * float(ly);
            float sx_L   = float(int(w0_L) >> 16) * (1.0 / 256.0);
            float step_L = float(w1_L >> 16u) * (1.0 / 1024.0);

            float base_R = float(w0_R & 0xFFFFu) * 0.125 - 2048.0 + float(int(w1_R << 16u) >> 16) * (1.0 / 256.0) * float(ly);
            float sx_R   = float(int(w0_R) >> 16) * (1.0 / 256.0);
            float step_R = float(w1_R >> 16u) * (1.0 / 1024.0);

            float row_h  = 0.0;
            float row_dx = 0.0;
            [unroll]
            for (uint rx = 0u; rx < 4u; ++rx) {
                uint cx   = x_s + rx;
                bool x_hi = (cx >> 3u) != px0;
                uint lx   = cx & 7u;
                uint lo4  = x_hi ? lo4_R : lo4_L;
                uint hi_w = x_hi ? hi_R  : hi_L;
                uint q5   = ((lo4 >> (lx << 2u)) & 0xFu) | (((hi_w >> (hi_sh + lx)) & 1u) << 4u);
                float base_v = x_hi ? base_R : base_L;
                float sx_v   = x_hi ? sx_R   : sx_L;
                float step_v = x_hi ? step_R : step_L;
                float h = base_v + sx_v * float(lx) + step_v * float(q5);
                row_h  += h * wx[rx];
                row_dx += h * dwx[rx];
            }
            h_sum  += row_h  * wy[ry];
            dx_sum += row_dx * wy[ry];
            dy_sum += row_h  * dwy[ry];
        }
        out_dh_duv = float2(dx_sum * scale, dy_sum * scale);
        return h_sum;
    }

    int max_i = int(k_tile_dim) - 1;
    for (int dy = -1; dy <= 2; ++dy) {
        uint cy = uint(clamp(iy + dy, 0, max_i));
        float row_h  = 0.0;
        float row_dx = 0.0;
        for (int dx = -1; dx <= 2; ++dx) {
            uint cx = uint(clamp(ix + dx, 0, max_i));
            float h = tile_load_height_m(tile_bda, cx, cy);
            row_h  += h * wx[dx + 1];
            row_dx += h * dwx[dx + 1];
        }
        h_sum  += row_h  * wy[dy + 1];
        dx_sum += row_dx * wy[dy + 1];
        dy_sum += row_h  * dwy[dy + 1];
    }
    out_dh_duv = float2(dx_sum * scale, dy_sum * scale);
    return h_sum;
}

public float4 unpack_rgba8(uint packed) {
    return float4(
        float(packed & 0xFFu) / 255.0,
        float((packed >> 8u) & 0xFFu) / 255.0,
        float((packed >> 16u) & 0xFFu) / 255.0,
        float((packed >> 24u) & 0xFFu) / 255.0
    );
}

// Exact IEC 61966-2-1 sRGB to linear light conversion so VK_FORMAT_B8G8R8A8_SRGB
// framebuffers never double-gamma-correct satellite photography
public float3 srgb_to_linear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, float3(0.0)), float3(2.4));
    return select(c <= float3(0.04045), lo, hi);
}

float3 unpack_rgb565(uint c) {
    return float3(
        float(c & 31u) * (1.0 / 31.0),
        float((c >> 5u) & 63u) * (1.0 / 63.0),
        float((c >> 11u) & 31u) * (1.0 / 31.0)
    );
}

float3 epack_load_rgb_interior(uint *words, uint cx, uint cy) {
    uint bx    = cx >> 2u;
    uint by    = cy >> 2u;
    uint lx    = cx & 3u;
    uint ly    = cy & 3u;
    uint cbase = k_color_word_offset + ((by << 6u) + bx) * 2u;
    uint cw0   = words[cbase + 0u];
    uint cw1   = words[cbase + 1u];
    float3 c0  = unpack_rgb565(cw0 & 0xFFFFu);
    float3 c1  = unpack_rgb565(cw0 >> 16u);
    uint sel   = (cw1 >> (((ly << 2u) | lx) << 1u)) & 3u;
    return lerp(c0, c1, float(sel) * (1.0 / 3.0));
}

// Decode one sRGB texel from .epack v1 (lossless RGB565 border ring + 4x4 BC1 interior blocks)
float3 epack_load_rgb(uint *words, uint sx, uint sy) {
    uint cx = min(sx, k_tile_dim - 1u);
    uint cy = min(sy, k_tile_dim - 1u);

    if (cx == 0u || cx == (k_tile_dim - 1u) || cy == 0u || cy == (k_tile_dim - 1u)) {
        bool on_y0 = (cy == 0u);
        bool on_y1 = (cy == (k_tile_dim - 1u));
        bool on_x0 = (cx == 0u);
        uint edge  = on_y0 ? 0u : (on_y1 ? 1u : (on_x0 ? 2u : 3u));
        uint coord = (on_y0 || on_y1) ? cx : cy;
        uint bidx  = edge * k_tile_dim + coord;
        uint bw    = words[k_border_c_word_offset + (bidx >> 1u)];
        uint raw16 = (bidx & 1u) != 0u ? (bw >> 16u) : (bw & 0xFFFFu);
        return unpack_rgb565(raw16);
    }
    return epack_load_rgb_interior(words, cx, cy);
}

float epack_load_ao(uint *words, uint ax, uint ay) {
    uint cx = min(ax, k_ao_dim - 1u);
    uint cy = min(ay, k_ao_dim - 1u);
    uint idx = (cy << 7u) + cx;
    uint w = words[k_ao_word_offset + (idx >> 2u)];
    uint byte_val = (w >> ((idx & 3u) << 3u)) & 0xFFu;
    return float(byte_val) * (1.0 / 255.0);
}

float epack_sample_bilinear_ao(uint *words, float2 uv) {
    float2 ta = clamp(uv, float2(0.0), float2(1.0)) * float(k_ao_dim - 1u);
    uint ax0 = uint(floor(ta.x));
    uint ay0 = uint(floor(ta.y));
    uint ax1 = min(ax0 + 1u, k_ao_dim - 1u);
    uint ay1 = min(ay0 + 1u, k_ao_dim - 1u);
    float fx = ta.x - float(ax0);
    float fy = ta.y - float(ay0);

    float a00 = epack_load_ao(words, ax0, ay0);
    float a10 = epack_load_ao(words, ax1, ay0);
    float a01 = epack_load_ao(words, ax0, ay1);
    float a11 = epack_load_ao(words, ax1, ay1);
    return lerp(lerp(a00, a10, fx), lerp(a01, a11, fx), fy);
}

// Fast 4-tap bilinear .epack v1 satellite imagery + 128x128 horizon AO sample
// Branchless interior decode avoids SIMD16/32 subgroup divergence across 4x4 BC1 block borders.
public float4 tile_sample_bilinear_imagery(uint64_t tile_bda, float2 uv) {
    uint *words = (uint *)tile_bda;

    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * float(k_tile_dim - 1u);
    uint x0 = uint(floor(tc.x));
    uint y0 = uint(floor(tc.y));
    uint x1 = min(x0 + 1u, k_tile_dim - 1u);
    uint y1 = min(y0 + 1u, k_tile_dim - 1u);
    float fx = tc.x - float(x0);
    float fy = tc.y - float(y0);

    float3 rgb;
    if (x0 > 0u && x1 < (k_tile_dim - 1u) && y0 > 0u && y1 < (k_tile_dim - 1u)) {
        float3 c00 = epack_load_rgb_interior(words, x0, y0);
        float3 c10 = epack_load_rgb_interior(words, x1, y0);
        float3 c01 = epack_load_rgb_interior(words, x0, y1);
        float3 c11 = epack_load_rgb_interior(words, x1, y1);
        rgb = lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fy);
    } else {
        float3 c00 = epack_load_rgb(words, x0, y0);
        float3 c10 = epack_load_rgb(words, x1, y0);
        float3 c01 = epack_load_rgb(words, x0, y1);
        float3 c11 = epack_load_rgb(words, x1, y1);
        rgb = lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fy);
    }
    float ao = epack_sample_bilinear_ao(words, uv);
    return float4(rgb, ao);
}

// 16-tap Catmull-Rom bicubic .epack v1 albedo + fused 5-tap CAS unsharp mask + bilinear 128x128 AO
// Exploits linearity of BC1 endpoint interpolation: fuses bicubic + CAS scalar weights inside the 4x4
// selector loop (only 12 scalar registers live!), then unpacks the 2x2 RGB565 block endpoints once at the end.
public float4 tile_sample_bicubic_imagery(uint64_t tile_bda, float2 uv, float cas_strength) {
    uint *words = (uint *)tile_bda;

    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * float(k_tile_dim - 1u);
    int ix = int(floor(tc.x));
    int iy = int(floor(tc.y));
    float fx = tc.x - float(ix);
    float fy = tc.y - float(iy);

    float4 wx = catmull_rom_weights(fx);
    float4 wy = catmull_rom_weights(fy);

    float3 rgb;
    if (ix >= 2 && ix <= 252 && iy >= 2 && iy <= 252) {
        uint x_s = uint(ix - 1);
        uint y_s = uint(iy - 1);
        uint bx0 = x_s >> 2u;
        uint bx1 = (x_s + 3u) >> 2u;
        uint by0 = y_s >> 2u;
        uint by1 = (y_s + 3u) >> 2u;

        uint b00_off = k_color_word_offset + ((by0 << 6u) + bx0) * 2u;
        uint b10_off = k_color_word_offset + ((by0 << 6u) + bx1) * 2u;
        uint b01_off = k_color_word_offset + ((by1 << 6u) + bx0) * 2u;
        uint b11_off = k_color_word_offset + ((by1 << 6u) + bx1) * 2u;

        uint sel_00 = words[b00_off + 1u];
        uint sel_10 = words[b10_off + 1u];
        uint sel_01 = words[b01_off + 1u];
        uint sel_11 = words[b11_off + 1u];

        float2 a00 = float2(0.0);
        float2 a10 = float2(0.0);
        float2 a01 = float2(0.0);
        float2 a11 = float2(0.0);

        [unroll]
        for (uint ry = 0u; ry < 4u; ++ry) {
            uint   cy    = y_s + ry;
            bool   y_hi  = (cy >> 2u) != by0;
            uint   ly_sh = (cy & 3u) << 2u;
            uint   sel_L = y_hi ? sel_01 : sel_00;
            uint   sel_R = y_hi ? sel_11 : sel_10;
            float  wy_v  = wy[ry];
            float2 rL    = float2(0.0);
            float2 rR    = float2(0.0);
            [unroll]
            for (uint rx = 0u; rx < 4u; ++rx) {
                uint  cx    = x_s + rx;
                bool  x_hi  = (cx >> 2u) != bx0;
                uint  sel_w = x_hi ? sel_R : sel_L;
                uint  s2    = (sel_w >> ((ly_sh | (cx & 3u)) << 1u)) & 3u;
                float w     = wx[rx] * wy_v;
                if (rx == 1u && ry == 1u) {
                    w += cas_strength;
                } else if ((rx == 1u && (ry == 0u || ry == 2u)) ||
                           (ry == 1u && (rx == 0u || rx == 2u))) {
                    w -= 0.25 * cas_strength;
                }
                float2 contrib = float2(w, w * float(s2));
                rL += x_hi ? float2(0.0) : contrib;
                rR += x_hi ? contrib : float2(0.0);
            }
            a00 += y_hi ? float2(0.0) : rL;
            a10 += y_hi ? float2(0.0) : rR;
            a01 += y_hi ? rL : float2(0.0);
            a11 += y_hi ? rR : float2(0.0);
        }

        uint cw0_00 = words[b00_off + 0u];
        uint cw0_10 = words[b10_off + 0u];
        uint cw0_01 = words[b01_off + 0u];
        uint cw0_11 = words[b11_off + 0u];

        const float k_inv3 = 1.0 / 3.0;
        float3 c0_00 = unpack_rgb565(cw0_00 & 0xFFFFu);
        float3 d_00  = unpack_rgb565(cw0_00 >> 16u) - c0_00;
        float3 c0_10 = unpack_rgb565(cw0_10 & 0xFFFFu);
        float3 d_10  = unpack_rgb565(cw0_10 >> 16u) - c0_10;
        float3 c0_01 = unpack_rgb565(cw0_01 & 0xFFFFu);
        float3 d_01  = unpack_rgb565(cw0_01 >> 16u) - c0_01;
        float3 c0_11 = unpack_rgb565(cw0_11 & 0xFFFFu);
        float3 d_11  = unpack_rgb565(cw0_11 >> 16u) - c0_11;

        float3 sum = (c0_00 * a00.x + d_00 * (a00.y * k_inv3))
                   + (c0_10 * a10.x + d_10 * (a10.y * k_inv3))
                   + (c0_01 * a01.x + d_01 * (a01.y * k_inv3))
                   + (c0_11 * a11.x + d_11 * (a11.y * k_inv3));
        rgb = clamp(sum, float3(0.0), float3(1.0));
    } else {
        float3 sum      = float3(0.0);
        float3 c_center = float3(0.0);
        float3 c_cross  = float3(0.0);
        int max_i = int(k_tile_dim) - 1;
        for (int dy = -1; dy <= 2; ++dy) {
            uint cy = uint(clamp(iy + dy, 0, max_i));
            float3 row = float3(0.0);
            for (int dx = -1; dx <= 2; ++dx) {
                uint cx = uint(clamp(ix + dx, 0, max_i));
                float3 tex = epack_load_rgb(words, cx, cy);
                row += tex * wx[dx + 1];
                if (dx == 0 && dy == 0) {
                    c_center = tex;
                } else if ((abs(dx) + abs(dy)) == 1) {
                    c_cross += tex * 0.25;
                }
            }
            sum += row * wy[dy + 1];
        }
        rgb = clamp(sum, float3(0.0), float3(1.0));
        if (cas_strength > 0.0) {
            rgb = clamp(rgb + (c_center - c_cross) * cas_strength, float3(0.0), float3(1.0));
        }
    }

    float ao = epack_sample_bilinear_ao(words, uv);
    return float4(rgb, ao);
}

// 64-Byte Interactive Bird's-Eye Orthophoto Placement Descriptor (at offset 22080 of GpuMathArchiveHeader)
public struct GpuMathOrthoOverlayGpu {
    public uint64_t pixels_bda;
    public uint     width_px;
    public uint     height_px;
    public float    center_x;
    public float    center_y;
    public float    half_span_x;
    public float    half_span_y;
    public float    opacity;
    public uint     active;
    public float    scale_pct_x;
    public float    scale_pct_y;
    public uint     committed_cnt;
    public uint     pad0;
    public uint     pad1;
    public uint     pad2;
};

public GpuMathOrthoOverlayGpu load_ortho_overlay(uint64_t dir_bda) {
    GpuMathOrthoOverlayGpu *ov = (GpuMathOrthoOverlayGpu *)(dir_bda + 22080ull);
    return ov[0];
}

public uint load_committed_ortho_count(uint64_t dir_bda) {
    uint *cnt_ptr = (uint *)(dir_bda + 22144ull);
    return min(cnt_ptr[0], 16u);
}

public GpuMathOrthoOverlayGpu load_committed_ortho_layer(uint64_t dir_bda, uint idx) {
    GpuMathOrthoOverlayGpu *layers = (GpuMathOrthoOverlayGpu *)(dir_bda + 22208ull);
    return layers[idx];
}

// 16-tap Catmull-Rom bicubic RGBA8 sampler + texel-space CAS sharpening for full-resolution ortho imagery
public float4 overlay_sample_bicubic_rgba(uint64_t pixels_bda, uint width_px, uint height_px, float2 uv) {
    if (pixels_bda == 0ull || width_px == 0u || height_px == 0u) {
        return float4(0.0);
    }
    uint *texels = (uint *)pixels_bda;
    int max_x = int(width_px) - 1;
    int max_y = int(height_px) - 1;
    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * float2(float(max_x), float(max_y));
    int ix = int(floor(tc.x));
    int iy = int(floor(tc.y));
    float fx = tc.x - float(ix);
    float fy = tc.y - float(iy);

    float4 wx = catmull_rom_weights(fx);
    float4 wy = catmull_rom_weights(fy);

    float4 sum = float4(0.0);
    float3 c_center = float3(0.0);
    float3 c_cross  = float3(0.0);
    [unroll]
    for (int dy = -1; dy <= 2; ++dy) {
        uint cy = uint(clamp(iy + dy, 0, max_y));
        float4 row = float4(0.0);
        [unroll]
        for (int dx = -1; dx <= 2; ++dx) {
            uint cx = uint(clamp(ix + dx, 0, max_x));
            float4 tex = unpack_rgba8(texels[cy * width_px + cx]);
            row += tex * wx[dx + 1];
            if (dx == 0 && dy == 0) {
                c_center = tex.rgb;
            } else if ((abs(dx) + abs(dy)) == 1) {
                c_cross += tex.rgb * 0.25;
            }
        }
        sum += row * wy[dy + 1];
    }
    float4 bicubic = clamp(sum, float4(0.0), float4(1.0));
    float3 detail  = c_center - c_cross;
    bicubic.rgb    = clamp(bicubic.rgb + detail * 0.32, float3(0.0), float3(1.0));
    return bicubic;
}
