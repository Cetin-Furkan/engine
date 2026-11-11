// Shared Slang 2026 GPU-Math Module for 2D Self-Relative Pointers (RelPtr2D),
// Lilliput 64-Bit Unified Tile Headers, and 4KB-Aligned 256x256 BDA Terrain Tiles.

module terrain_math;

public static const uint k_tile_dim          = 256u;
public static const uint k_tile_samples      = 65536u;
public static const uint k_tile_height_bytes = 131072u;
public static const uint k_tile_bytes        = 393216u;
public static const uint k_dir_bytes         = 256u;
public static const uint k_node_bytes        = 64u;
public static const uint k_skirt_quads       = 64u;
public static const uint k_skirt_verts       = 384u;

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

// Read one u16 height sample (in 1/16th meter units) from a 4KB-aligned tile block
public float tile_load_height_m(uint64_t tile_bda, uint sx, uint sy) {
    uint cx = min(sx, k_tile_dim - 1u);
    uint cy = min(sy, k_tile_dim - 1u);
    uint idx = cy * k_tile_dim + cx;
    uint *words = (uint *)tile_bda;
    uint w = words[idx >> 1u];
    uint u16_val = (idx & 1u) != 0u ? (w >> 16u) : (w & 0xFFFFu);
    return float(u16_val) * 0.0625;
}

// Fast 4-tap bilinear elevation + analytical (dh/du, dh/dv) gradient (4 BDA loads instead of 16)
public float tile_sample_bilinear_height_grad_m(uint64_t tile_bda, float2 uv, out float2 out_dh_duv) {
    float scale = float(k_tile_dim - 1u);
    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * scale;
    uint x0 = uint(floor(tc.x));
    uint y0 = uint(floor(tc.y));
    uint x1 = min(x0 + 1u, k_tile_dim - 1u);
    uint y1 = min(y0 + 1u, k_tile_dim - 1u);
    float fx = tc.x - float(x0);
    float fy = tc.y - float(y0);

    float h00 = tile_load_height_m(tile_bda, x0, y0);
    float h10 = tile_load_height_m(tile_bda, x1, y0);
    float h01 = tile_load_height_m(tile_bda, x0, y1);
    float h11 = tile_load_height_m(tile_bda, x1, y1);

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
    int max_i = int(k_tile_dim) - 1;

    float h_sum  = 0.0;
    float dx_sum = 0.0;
    float dy_sum = 0.0;

    [unroll]
    for (int dy = -1; dy <= 2; ++dy) {
        uint cy = uint(clamp(iy + dy, 0, max_i));
        float row_h  = 0.0;
        float row_dx = 0.0;
        [unroll]
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

// Fast 4-tap bilinear RGBA8 satellite imagery sample (4 BDA loads instead of 16)
public float4 tile_sample_bilinear_imagery(uint64_t tile_bda, float2 uv) {
    uint64_t img_bda = tile_bda + uint64_t(k_tile_height_bytes);
    uint *texels = (uint *)img_bda;

    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * float(k_tile_dim - 1u);
    uint x0 = uint(floor(tc.x));
    uint y0 = uint(floor(tc.y));
    uint x1 = min(x0 + 1u, k_tile_dim - 1u);
    uint y1 = min(y0 + 1u, k_tile_dim - 1u);
    float fx = tc.x - float(x0);
    float fy = tc.y - float(y0);

    float4 c00 = unpack_rgba8(texels[y0 * k_tile_dim + x0]);
    float4 c10 = unpack_rgba8(texels[y0 * k_tile_dim + x1]);
    float4 c01 = unpack_rgba8(texels[y1 * k_tile_dim + x0]);
    float4 c11 = unpack_rgba8(texels[y1 * k_tile_dim + x1]);
    return lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fy);
}

// 16-tap Catmull-Rom bicubic RGBA8 satellite imagery sample from a 4KB-aligned tile block
public float4 tile_sample_bicubic_imagery(uint64_t tile_bda, float2 uv) {
    uint64_t img_bda = tile_bda + uint64_t(k_tile_height_bytes);
    uint *texels = (uint *)img_bda;

    float2 tc = clamp(uv, float2(0.0), float2(1.0)) * float(k_tile_dim - 1u);
    int ix = int(floor(tc.x));
    int iy = int(floor(tc.y));
    float fx = tc.x - float(ix);
    float fy = tc.y - float(iy);

    float4 wx = catmull_rom_weights(fx);
    float4 wy = catmull_rom_weights(fy);
    int max_i = int(k_tile_dim) - 1;

    float4 sum = float4(0.0);
    [unroll]
    for (int dy = -1; dy <= 2; ++dy) {
        uint cy = uint(clamp(iy + dy, 0, max_i));
        float4 row = float4(0.0);
        [unroll]
        for (int dx = -1; dx <= 2; ++dx) {
            uint cx = uint(clamp(ix + dx, 0, max_i));
            float4 tex = unpack_rgba8(texels[cy * k_tile_dim + cx]);
            row += tex * wx[dx + 1];
        }
        sum += row * wy[dy + 1];
    }
    return clamp(sum, float4(0.0), float4(1.0));
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
