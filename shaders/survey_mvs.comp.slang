// Engine v0.2 Milestone 5: Tiled Slang 2026 Compute Plane-Sweep ZNCC MVS & Wendland C^2 RBF
// Executes entirely over 64-bit Vulkan 1.4 Buffer Device Addresses (0 VkDescriptorSets):
//   1. Cooperative 8x8 workgroup SLM culling of Geodetic Control Points (GCPs) & Breaklines
//   2. Wendland C^2 compactly supported radial basis function (RBF) elevation fusion
//   3. Multi-View Stereo (MVS) 5x5 Zero-Mean Normalized Cross-Correlation (ZNCC) plane sweep
//      with 3-point parabolic sub-post elevation refinement
//   4. Brown-Conrady distortion-corrected projective orthorectification

static const uint k_mvs_magic      = 0x53564D47u; // "GMVS"
static const uint k_slm_max_pts    = 64u;
static const uint k_slm_max_bls    = 32u;

struct MvsPush {
    uint64_t hdr_bda;
    uint     flags;
    uint     pad0;
};

[[vk::push_constant]] MvsPush pc;

struct GpuMvsHeader {
    uint     magic;
    uint     tile_dim;
    uint     photo_count;
    uint     point_count;
    uint     breakline_count;
    uint     sweep_steps;
    float    world_min_x;
    float    world_min_y;
    float    span_x;
    float    span_y;
    float    mvs_range_m;
    float    mvs_step_m;
    float    mvs_min_ncc;
    float    min_kernel_r;
    uint     roi_min_x;
    uint     roi_max_x;
    uint     roi_min_y;
    uint     roi_max_y;
    uint     pad0;
    uint     pad1;
    uint64_t prior_elev_bda;
    uint64_t out_elev_bda;
    uint64_t io_rgba_bda;
    uint64_t photos_bda;
    uint64_t points_bda;
    uint64_t breaklines_bda;
};

struct GpuMvsPoint {
    float wx;
    float wy;
    float elev_m;
    float radius_m;
    float weight;
    uint  rgba;
    uint  has_color;
    uint  pad0;
};

struct GpuMvsBreakline {
    float ax;
    float ay;
    float ea;
    float bx;
    float by;
    float eb;
    float radius_m;
    float weight;
};

struct GpuMvsPhoto {
    uint64_t pixels_bda;
    uint     width_px;
    uint     height_px;
    float    fx_px;
    float    fy_px;
    float    cx_px;
    float    cy_px;
    float    k1;
    float    k2;
    float    k3;
    float    p1;
    float    p2;
    float    cam_x;
    float    cam_y;
    float    alt_m;
    float    right_x;
    float    right_y;
    float    right_z;
    float    blend_weight;
    float    up_x;
    float    up_y;
    float    up_z;
    float    enable_mvs;
    float    fwd_x;
    float    fwd_y;
    float    fwd_z;
    float    pad0;
    uint     pad_tail0;
    uint     pad_tail1;
    uint     pad_tail2;
    uint     pad_tail3;
};

groupshared uint s_pt_cnt;
groupshared uint s_bl_cnt;
groupshared uint s_pt_ids[k_slm_max_pts];
groupshared uint s_bl_ids[k_slm_max_bls];

float wendland_c2_gpu(float q) {
    if (q < 0.0 || q >= 1.0) {
        return 0.0;
    }
    float omq  = 1.0 - q;
    float omq2 = omq * omq;
    return (omq2 * omq2) * (4.0 * q + 1.0);
}

bool project_world_to_photo(GpuMvsPhoto ph, float3 world_pt, out float2 out_uv, out float out_depth) {
    out_uv    = float2(0.0);
    out_depth = 0.0;

    float3 rel = world_pt - float3(ph.cam_x, ph.cam_y, ph.alt_m);
    float3 r_v = float3(ph.right_x, ph.right_y, ph.right_z);
    float3 u_v = float3(ph.up_x, ph.up_y, ph.up_z);
    float3 f_v = float3(ph.fwd_x, ph.fwd_y, ph.fwd_z);

    float z_cam = dot(rel, f_v);
    if (z_cam <= 0.25) {
        return false;
    }
    float inv_z = 1.0 / z_cam;
    float xn    = dot(rel, r_v) * inv_z;
    float yn    = -dot(rel, u_v) * inv_z;

    float r2  = xn * xn + yn * yn;
    if (r2 > 6.25) {
        return false;
    }
    float r4  = r2 * r2;
    float r6  = r4 * r2;
    float rad = 1.0 + ph.k1 * r2 + ph.k2 * r4 + ph.k3 * r6;

    float xd = xn * rad + 2.0 * ph.p1 * xn * yn + ph.p2 * (r2 + 2.0 * xn * xn);
    float yd = yn * rad + ph.p1 * (r2 + 2.0 * yn * yn) + 2.0 * ph.p2 * xn * yn;

    float u = ph.cx_px + ph.fx_px * xd;
    float v = ph.cy_px + ph.fy_px * yd;
    if (u < 1.0 || u > float(ph.width_px) - 2.0 ||
        v < 1.0 || v > float(ph.height_px) - 2.0) {
        return false;
    }
    out_uv    = float2(u, v);
    out_depth = z_cam;
    return true;
}

float4 sample_photo_bilinear(GpuMvsPhoto ph, float2 uv_px) {
    if (ph.pixels_bda == 0ull || ph.width_px == 0u || ph.height_px == 0u) {
        return float4(0.0);
    }
    uint *px = (uint *)ph.pixels_bda;
    float cx = clamp(uv_px.x, 0.0, float(ph.width_px - 1u));
    float cy = clamp(uv_px.y, 0.0, float(ph.height_px - 1u));
    uint x0  = uint(floor(cx));
    uint y0  = uint(floor(cy));
    uint x1  = min(x0 + 1u, ph.width_px - 1u);
    uint y1  = min(y0 + 1u, ph.height_px - 1u);
    float fx = cx - float(x0);
    float fy = cy - float(y0);

    uint p00 = px[y0 * ph.width_px + x0];
    uint p10 = px[y0 * ph.width_px + x1];
    uint p01 = px[y1 * ph.width_px + x0];
    uint p11 = px[y1 * ph.width_px + x1];

    float4 c00 = float4(float(p00 & 0xFFu), float((p00 >> 8u) & 0xFFu), float((p00 >> 16u) & 0xFFu), float((p00 >> 24u) & 0xFFu));
    float4 c10 = float4(float(p10 & 0xFFu), float((p10 >> 8u) & 0xFFu), float((p10 >> 16u) & 0xFFu), float((p10 >> 24u) & 0xFFu));
    float4 c01 = float4(float(p01 & 0xFFu), float((p01 >> 8u) & 0xFFu), float((p01 >> 16u) & 0xFFu), float((p01 >> 24u) & 0xFFu));
    float4 c11 = float4(float(p11 & 0xFFu), float((p11 >> 8u) & 0xFFu), float((p11 >> 16u) & 0xFFu), float((p11 >> 24u) & 0xFFu));

    return lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fy);
}

float sample_photo_luma_gpu(GpuMvsPhoto ph, float2 uv_px) {
    float4 c = sample_photo_bilinear(ph, uv_px);
    return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
}

// 5x5 Zero-Mean Normalized Cross-Correlation (ZNCC) in single-pass algebraic form
float eval_pair_zncc_5x5(GpuMvsPhoto p0, GpuMvsPhoto p1, float2 world_xy, float elev_m, float patch_step_m) {
    float sum0  = 0.0;
    float sum1  = 0.0;
    float sum00 = 0.0;
    float sum11 = 0.0;
    float sum01 = 0.0;

    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            float3 wpt = float3(world_xy.x + float(dx) * patch_step_m,
                                world_xy.y + float(dy) * patch_step_m,
                                elev_m);
            float2 uv0, uv1;
            float  d0, d1;
            if (!project_world_to_photo(p0, wpt, uv0, d0) ||
                !project_world_to_photo(p1, wpt, uv1, d1)) {
                return -1.0;
            }
            float a = sample_photo_luma_gpu(p0, uv0);
            float b = sample_photo_luma_gpu(p1, uv1);
            sum0  += a;
            sum1  += b;
            sum00 += a * a;
            sum11 += b * b;
            sum01 += a * b;
        }
    }

    const float inv_n = 1.0 / 25.0;
    float var0 = sum00 - sum0 * sum0 * inv_n;
    float var1 = sum11 - sum1 * sum1 * inv_n;
    if (var0 < 9.0 || var1 < 9.0) {
        return -1.0;
    }
    float cov = sum01 - sum0 * sum1 * inv_n;
    return cov * rsqrt(var0 * var1 + 1.0e-5);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 grp : SV_GroupID, uint3 dtid : SV_DispatchThreadID) {
    if (pc.hdr_bda == 0ull) {
        return;
    }
    GpuMvsHeader *hdr = (GpuMvsHeader *)pc.hdr_bda;
    if (hdr->magic != k_mvs_magic || hdr->prior_elev_bda == 0ull || hdr->out_elev_bda == 0ull) {
        return;
    }

    uint dim   = hdr->tile_dim;
    uint local = gtid.y * 8u + gtid.x;
    if (local == 0u) {
        s_pt_cnt = 0u;
        s_bl_cnt = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Compute 8x8 tile sub-block world AABB for cooperative SLM feature culling
    float inv_dim_m1 = 1.0 / max(float(dim - 1u), 1.0);
    float blk_min_x  = hdr->world_min_x + float(grp.x * 8u) * inv_dim_m1 * hdr->span_x;
    float blk_max_x  = hdr->world_min_x + float(min(grp.x * 8u + 7u, dim - 1u)) * inv_dim_m1 * hdr->span_x;
    float blk_min_y  = hdr->world_min_y + float(grp.y * 8u) * inv_dim_m1 * hdr->span_y;
    float blk_max_y  = hdr->world_min_y + float(min(grp.y * 8u + 7u, dim - 1u)) * inv_dim_m1 * hdr->span_y;

    GpuMvsPoint     *pts = (GpuMvsPoint *)hdr->points_bda;
    GpuMvsBreakline *bls = (GpuMvsBreakline *)hdr->breaklines_bda;

    if (pts != nullptr && hdr->point_count > 0u) {
        for (uint i = local; i < hdr->point_count; i += 64u) {
            GpuMvsPoint pt = pts[i];
            float r = max(pt.radius_m, hdr->min_kernel_r);
            if (pt.wx + r >= blk_min_x && pt.wx - r <= blk_max_x &&
                pt.wy + r >= blk_min_y && pt.wy - r <= blk_max_y) {
                uint slot = 0u;
                InterlockedAdd(s_pt_cnt, 1u, slot);
                if (slot < k_slm_max_pts) {
                    s_pt_ids[slot] = i;
                }
            }
        }
    }

    if (bls != nullptr && hdr->breakline_count > 0u) {
        for (uint i = local; i < hdr->breakline_count; i += 64u) {
            GpuMvsBreakline bl = bls[i];
            float r     = max(bl.radius_m, hdr->min_kernel_r);
            float min_x = min(bl.ax, bl.bx) - r;
            float max_x = max(bl.ax, bl.bx) + r;
            float min_y = min(bl.ay, bl.by) - r;
            float max_y = max(bl.ay, bl.by) + r;
            if (max_x >= blk_min_x && min_x <= blk_max_x &&
                max_y >= blk_min_y && min_y <= blk_max_y) {
                uint slot = 0u;
                InterlockedAdd(s_bl_cnt, 1u, slot);
                if (slot < k_slm_max_bls) {
                    s_bl_ids[slot] = i;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint sx = dtid.x;
    uint sy = dtid.y;
    if (sx >= dim || sy >= dim) {
        return;
    }

    uint   idx       = sy * dim + sx;
    float *prior_arr = (float *)hdr->prior_elev_bda;
    float *out_arr   = (float *)hdr->out_elev_bda;
    float  prior_h   = prior_arr[idx];

    if (sx < hdr->roi_min_x || sx > hdr->roi_max_x ||
        sy < hdr->roi_min_y || sy > hdr->roi_max_y) {
        out_arr[idx] = prior_h;
        return;
    }

    float wx = hdr->world_min_x + float(sx) * inv_dim_m1 * hdr->span_x;
    float wy = hdr->world_min_y + float(sy) * inv_dim_m1 * hdr->span_y;

    // 1. Wendland C^2 Geodetic GCP + Breakline RBF Fusion
    float rbf_num   = 0.0;
    float rbf_den   = 0.0;
    float rbf_max_w = 0.0;
    float3 pt_rgb   = float3(0.0);
    float  pt_rgb_w = 0.0;

    uint n_pts = min(s_pt_cnt, k_slm_max_pts);
    for (uint k = 0u; k < n_pts; ++k) {
        GpuMvsPoint pt = pts[s_pt_ids[k]];
        float r = max(pt.radius_m, hdr->min_kernel_r);
        float d = length(float2(wx - pt.wx, wy - pt.wy));
        if (d < r) {
            float w = wendland_c2_gpu(d / r) * pt.weight;
            rbf_num += (pt.elev_m - prior_h) * w;
            rbf_den += w;
            rbf_max_w = max(rbf_max_w, w);
            if (pt.has_color != 0u) {
                float3 c = float3(float(pt.rgba & 0xFFu),
                                  float((pt.rgba >> 8u) & 0xFFu),
                                  float((pt.rgba >> 16u) & 0xFFu));
                pt_rgb   += c * w;
                pt_rgb_w += w;
            }
        }
    }

    uint n_bls = min(s_bl_cnt, k_slm_max_bls);
    for (uint k = 0u; k < n_bls; ++k) {
        GpuMvsBreakline bl = bls[s_bl_ids[k]];
        float2 ab   = float2(bl.bx - bl.ax, bl.by - bl.ay);
        float  len2 = max(dot(ab, ab), 1.0e-4);
        float  t    = clamp(dot(float2(wx - bl.ax, wy - bl.ay), ab) / len2, 0.0, 1.0);
        float2 proj = float2(bl.ax, bl.ay) + ab * t;
        float  d    = length(float2(wx, wy) - proj);
        float  r    = max(bl.radius_m, hdr->min_kernel_r);
        if (d < r) {
            float w      = wendland_c2_gpu(d / r) * bl.weight;
            float bl_el  = lerp(bl.ea, bl.eb, t);
            rbf_num     += (bl_el - prior_h) * w;
            rbf_den     += w;
            rbf_max_w    = max(rbf_max_w, w);
        }
    }

    float fused_h = prior_h;
    if (rbf_den > 1.0e-5) {
        float blend = clamp(rbf_max_w, 0.0, 1.0);
        fused_h = prior_h + (rbf_num / rbf_den) * blend;
    }

    // 2. Plane-Sweep 5x5 Patch-ZNCC Multi-View Stereo + Parabolic Sub-Post Refinement
    GpuMvsPhoto *photos = (GpuMvsPhoto *)hdr->photos_bda;
    if (photos != nullptr && hdr->photo_count >= 2u && hdr->sweep_steps >= 3u) {
        GpuMvsPhoto p0 = photos[0];
        GpuMvsPhoto p1 = photos[1];
        if (p0.enable_mvs > 0.5 && p1.enable_mvs > 0.5) {
            float post_step   = hdr->span_x * inv_dim_m1;
            float patch_step  = clamp(post_step * 0.65, 0.5, 18.0);
            uint  steps       = min(hdr->sweep_steps, 96u);
            float h_start     = fused_h - hdr->mvs_range_m;
            float dh          = hdr->mvs_step_m;

            float best_ncc    = -2.0;
            uint  best_s      = 0u;
            float prev_ncc    = -2.0;
            float best_m1_ncc = -2.0;
            float best_p1_ncc = -2.0;

            for (uint s = 0u; s < steps; ++s) {
                float cand_h = h_start + float(s) * dh;
                float ncc    = eval_pair_zncc_5x5(p0, p1, float2(wx, wy), cand_h, patch_step);
                if (s == best_s + 1u) {
                    best_p1_ncc = ncc;
                }
                if (ncc > best_ncc) {
                    best_ncc    = ncc;
                    best_s      = s;
                    best_m1_ncc = prev_ncc;
                    best_p1_ncc = -2.0;
                }
                prev_ncc = ncc;
            }

            if (best_ncc >= hdr->mvs_min_ncc) {
                float refined_h = h_start + float(best_s) * dh;
                if (best_s > 0u && best_s + 1u < steps &&
                    best_m1_ncc > -0.95 && best_p1_ncc > -0.95) {
                    float denom = best_m1_ncc - 2.0 * best_ncc + best_p1_ncc;
                    if (denom < -1.0e-4) {
                        float delta = clamp(0.5 * (best_m1_ncc - best_p1_ncc) / denom, -0.5, 0.5);
                        refined_h += delta * dh;
                    }
                }
                float mvs_conf = smoothstep(hdr->mvs_min_ncc, min(hdr->mvs_min_ncc + 0.25, 0.95), best_ncc);
                fused_h = lerp(fused_h, refined_h, mvs_conf * (1.0 - 0.65 * clamp(rbf_max_w, 0.0, 1.0)));
            }
        }
    }

    out_arr[idx] = fused_h;

    // 3. Projective Orthorectification into io_rgba_bda
    if (hdr->io_rgba_bda != 0ull && (hdr->photo_count > 0u || pt_rgb_w > 1.0e-4)) {
        uint  *rgba_arr = (uint *)hdr->io_rgba_bda;
        uint   orig     = rgba_arr[idx];
        uint   ao_u8    = (orig >> 24u) & 0xFFu;
        float3 base_rgb = float3(float(orig & 0xFFu),
                                 float((orig >> 8u) & 0xFFu),
                                 float((orig >> 16u) & 0xFFu));

        float3 accum_rgb = float3(0.0);
        float  accum_w   = 0.0;
        if (photos != nullptr) {
            uint n_ph = min(hdr->photo_count, 8u);
            for (uint pi = 0u; pi < n_ph; ++pi) {
                GpuMvsPhoto ph = photos[pi];
                float2 uv;
                float  depth;
                if (project_world_to_photo(ph, float3(wx, wy, fused_h), uv, depth)) {
                    float4 samp = sample_photo_bilinear(ph, uv);
                    float  edge = min(min(uv.x, float(ph.width_px) - uv.x),
                                      min(uv.y, float(ph.height_px) - uv.y));
                    float  fw   = smoothstep(1.0, 16.0, edge) * ph.blend_weight * (samp.a / 255.0);
                    accum_rgb  += samp.rgb * fw;
                    accum_w    += fw;
                }
            }
        }
        if (pt_rgb_w > 1.0e-4) {
            accum_rgb += pt_rgb * 0.35;
            accum_w   += pt_rgb_w * 0.35;
        }
        if (accum_w > 1.0e-3) {
            float  alpha = clamp(accum_w, 0.0, 1.0);
            float3 out_c = clamp(lerp(base_rgb, accum_rgb / accum_w, alpha), float3(0.0), float3(255.0));
            uint   r_u8  = uint(out_c.r + 0.5);
            uint   g_u8  = uint(out_c.g + 0.5);
            uint   b_u8  = uint(out_c.b + 0.5);
            rgba_arr[idx] = r_u8 | (g_u8 << 8u) | (b_u8 << 16u) | (ao_u8 << 24u);
        }
    }
}
