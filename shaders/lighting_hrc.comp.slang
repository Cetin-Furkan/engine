// Slang 2026 Split / Holographic Radiance Cascades (HRC) Compute Pass
// Constant space-angle hierarchy c in {3, 2, 1, 0} with Lambert equal-area S^2
// binary KD subdivision + cooperative SLM proxy cache & scalar DEM interval oracle.

#include "lighting_gi_common.slang"

[[vk::push_constant]] LightingComputePush pc;

groupshared LightingFrameHeader s_hdr;
groupshared LightingProxyGpu    s_proxies[32];

// Exact Lambert Cylindrical Equal-Area S^2 KD-partition for ray m in cascade c (M_c = 4 << c):
// - Root quadrant q = m >> c in {0, 1, 2, 3} splits azimuth [0, 2*pi) into 4 quadrants.
// - Subsequent cascade levels alternate splitting elevation z in [-0.42, +0.94] and azimuth phi,
//   guaranteeing Omega(c, m) = Omega(c+1, 2m) U Omega(c+1, 2m+1) with equal measure dOmega!
float3 hrc_world_ray_dir(uint c, uint m) {
    uint q   = (m >> c) & 3u;
    uint sub = m & ((1u << c) - 1u);

    uint phi_bins = 4u;
    uint z_bins   = 1u;
    uint phi_idx  = q;
    uint z_idx    = 0u;

    [unroll]
    for (uint lv = 1u; lv <= 3u; ++lv) {
        if (lv <= c) {
            uint bit = (sub >> (c - lv)) & 1u;
            if ((lv & 1u) != 0u) {
                z_idx   = (z_idx << 1u) | bit;
                z_bins <<= 1u;
            } else {
                phi_idx   = (phi_idx << 1u) | bit;
                phi_bins <<= 1u;
            }
        }
    }

    float u_phi = (float(phi_idx) + 0.5) / float(phi_bins);
    float u_z   = (float(z_idx)   + 0.5) / float(z_bins);

    float phi  = u_phi * (2.0 * k_gi_pi);
    float z    = lerp(-0.42, 0.94, u_z);
    float r_xy = sqrt(max(1.0 - z * z, 0.0));
    return float3(r_xy * cos(phi), r_xy * sin(phi), z);
}

void hrc_interval_bounds(LightingFrameHeader hdr, uint c, out float t_start, out float t_end) {
    float px_w = 1.0 / max(hdr.zoom, 1.0e-4);
    if (c == 0u) {
        t_start = 12.0 * px_w;
        t_end   = 58.0 * px_w;
    } else if (c == 1u) {
        t_start = 58.0  * px_w;
        t_end   = 220.0 * px_w;
    } else if (c == 2u) {
        t_start = 220.0 * px_w;
        t_end   = 760.0 * px_w;
    } else {
        t_start = 760.0  * px_w;
        t_end   = 2400.0 * px_w;
    }
}

LightingHrcRayGpu sample_parent_cascade_bilinear(
    LightingHrcRayGpu *hrc_buf,
    uint               parent_offset,
    uint               pw,
    uint               ph,
    uint               pm_cnt,
    float2             uv,
    uint               child_m)
{
    float2 tc   = clamp(uv, float2(0.0), float2(1.0)) * float2(float(pw), float(ph)) - float2(0.5);
    int2   base = int2(floor(tc));
    float2 f    = tc - float2(base);
    int2   maxc = int2(int(pw) - 1, int(ph) - 1);

    float3 rad_sum  = float3(0.0);
    float  beta_sum = 0.0;

    [unroll]
    for (int dy = 0; dy <= 1; ++dy) {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx) {
            int2  p   = clamp(base + int2(dx, dy), int2(0), maxc);
            float w   = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            uint  idx = parent_offset + (uint(p.y) * pw + uint(p.x)) * pm_cnt + child_m;
            LightingHrcRayGpu r = hrc_buf[idx];
            rad_sum  += lighting_unpack_rgb9e5_gpu(r.rad_rgb9e5) * w;
            beta_sum += r.beta * w;
        }
    }

    LightingHrcRayGpu res;
    res.rad_rgb9e5 = lighting_pack_rgb9e5_gpu(rad_sum);
    res.beta       = beta_sum;
    return res;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    if (pc.env_bda == 0ull) {
        return;
    }

    uint lidx = gtid.y * 8u + gtid.x;
    if (lidx == 0u) {
        s_hdr = ((LightingFrameHeader *)(pc.env_bda + k_lighting_header_offset))[0];
    }
    GroupMemoryBarrierWithGroupSync();

    LightingFrameHeader hdr = s_hdr;
    uint c = pc.cascade_idx;
    if (hdr.magic != k_lighting_magic || hdr.hrc_bda == 0ull || hdr.gbuf_bda == 0ull || c >= 4u) {
        return;
    }

    uint p_cnt = (hdr.proxies_bda != 0ull) ? min(hdr.proxy_count, 128u) : 0u;
    if (p_cnt > 0u && p_cnt <= 32u && lidx < p_cnt) {
        s_proxies[lidx] = ((LightingProxyGpu *)hdr.proxies_bda)[lidx];
    }
    GroupMemoryBarrierWithGroupSync();

    uint cw    = max(hdr.hrc_w0 >> c, 1u);
    uint ch    = max(hdr.hrc_h0 >> c, 1u);
    uint m_cnt = 4u << c;

    uint probe_x = tid.x / m_cnt;
    uint dir_m   = tid.x - probe_x * m_cnt;
    uint probe_y = tid.y;
    if (probe_x >= cw || probe_y >= ch) {
        return;
    }

    float2 uv = (float2(float(probe_x), float(probe_y)) + float2(0.5)) / float2(float(cw), float(ch));
    uint   gx = min(uint(uv.x * float(hdr.gi_width)),  hdr.gi_width  - 1u);
    uint   gy = min(uint(uv.y * float(hdr.gi_height)), hdr.gi_height - 1u);

    LightingGbufTexelGpu *gbuf = (LightingGbufTexelGpu *)hdr.gbuf_bda;
    LightingGbufTexelGpu anchor = gbuf[gy * hdr.gi_width + gx];

    // Search 4 cardinal GI neighbors if exact probe texel fell on sky near a silhouette edge
    if (anchor.view_z <= -1.0e19) {
        static const int2 k_cardinal[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
        [unroll]
        for (int k = 0; k < 4; ++k) {
            if (anchor.view_z <= -1.0e19) {
                int nx = clamp(int(gx) + k_cardinal[k].x, 0, int(hdr.gi_width)  - 1);
                int ny = clamp(int(gy) + k_cardinal[k].y, 0, int(hdr.gi_height) - 1);
                LightingGbufTexelGpu cand = gbuf[uint(ny) * hdr.gi_width + uint(nx)];
                if (cand.view_z > -1.0e19) {
                    anchor = cand;
                    gx     = uint(nx);
                    gy     = uint(ny);
                }
            }
        }
    }

    LightingHrcRayGpu *hrc_buf = (LightingHrcRayGpu *)hdr.hrc_bda;
    uint out_idx = hdr.hrc_offset_rays[c] + (probe_y * cw + probe_x) * m_cnt + dir_m;

    if (anchor.view_z <= -1.0e19) {
        LightingHrcRayGpu empty_ray;
        empty_ray.rad_rgb9e5 = 0u;
        empty_ray.beta       = 1.0;
        hrc_buf[out_idx]     = empty_ray;
        return;
    }

    float2 anchor_uv = (float2(float(gx), float(gy)) + float2(0.5)) / float2(float(hdr.gi_width), float(hdr.gi_height));
    float2 full_px   = anchor_uv * float2(float(hdr.full_width), float(hdr.full_height));
    float2 vxy       = lighting_pixel_to_view_xy(hdr, full_px);
    float3 view_pos  = float3(vxy.x, vxy.y, anchor.view_z);

    float4 q         = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 view_nrm  = lighting_unpack_oct_snorm16_gpu(anchor.oct_nrm_s16);
    float3 world_nrm = normalize(lighting_inv_view_rotate(q, view_nrm));
    float  px_w      = 1.0 / max(hdr.zoom, 1.0e-4);
    float3 world_pos = lighting_view_to_world(hdr, view_pos) + world_nrm * (1.5 * px_w);

    float3 rd      = hrc_world_ray_dir(c, dir_m);
    float  t_start = 0.0;
    float  t_end   = 0.0;
    hrc_interval_bounds(hdr, c, t_start, t_end);

    bool   hit_interval = false;
    bool   hit_terrain  = false;
    float  best_t       = t_end;
    float3 best_wnrm    = float3(0.0, 0.0, 1.0);
    float3 best_alb     = float3(0.0);
    float3 best_emi     = float3(0.0);

    // 1. Evaluate 3D Proxy Volumes from on-chip SLM (or global fallback if >32 proxies)
    if (p_cnt > 0u && p_cnt <= 32u) {
        for (uint i = 0u; i < p_cnt; ++i) {
            LightingProxyGpu pr = s_proxies[i];
            float  th = 0.0;
            float3 wn = float3(0.0);
            if (lighting_ray_proxy_gpu(world_pos, rd, t_start, best_t, pr, th, wn)) {
                best_t       = th;
                best_wnrm    = wn;
                best_alb     = proxy_albedo(pr);
                best_emi     = proxy_emissive(pr);
                hit_terrain  = false;
                hit_interval = true;
            }
        }
    } else if (p_cnt > 32u) {
        LightingProxyGpu *proxies = (LightingProxyGpu *)hdr.proxies_bda;
        for (uint i = 0u; i < p_cnt; ++i) {
            LightingProxyGpu pr = proxies[i];
            float  th = 0.0;
            float3 wn = float3(0.0);
            if (lighting_ray_proxy_gpu(world_pos, rd, t_start, best_t, pr, th, wn)) {
                best_t       = th;
                best_wnrm    = wn;
                best_alb     = proxy_albedo(pr);
                best_emi     = proxy_emissive(pr);
                hit_terrain  = false;
                hit_interval = true;
            }
        }
    }

    // 2. Evaluate World 8 Terrain DEM Heightfield over [t_start, best_t]
    //    Skip upward sky rays that start above peak elevation (1400.0 world units)
    if (hdr.terrain_dir_bda != 0ull && !(rd.z >= 0.0 && (world_pos.z + rd.z * t_start) > 1400.0)) {
        uint  sub_steps = hdr.oracle_mode != 0u ? 4u : 2u;
        int   dem_lod   = max(3 - int(c), 0);
        float dt        = (best_t - t_start) / float(sub_steps);
        for (uint s = 1u; s <= sub_steps; ++s) {
            float  tc = t_start + dt * float(s);
            float3 wp = world_pos + rd * tc;
            if (wp.z > 1400.0 && rd.z >= 0.0) {
                break;
            }
            float dz = 0.0;
            if (lighting_sample_terrain_height_fast(
                    hdr.terrain_dir_bda, hdr.elev_base_m, hdr.vertical_scale, wp.xy, dem_lod, dz) &&
                wp.z <= dz) {
                float  dz_hit = 0.0;
                float3 dn_hit = float3(0.0, 0.0, 1.0);
                float3 da_hit = float3(0.35, 0.38, 0.30);
                if (lighting_sample_terrain_dem(hdr, wp.xy, dem_lod, dz_hit, dn_hit, da_hit)) {
                    best_t       = tc;
                    best_wnrm    = dn_hit;
                    best_alb     = da_hit;
                    best_emi     = float3(0.0);
                    hit_terrain  = true;
                    hit_interval = true;
                }
                break;
            }
        }
    }

    // 3. Evaluate Ground Plane z = hdr.ground_z over [t_start, best_t]
    if (hdr.has_ground != 0u && rd.z < -1.0e-4) {
        float tg = (hdr.ground_z - world_pos.z) / rd.z;
        if (tg >= t_start && tg <= best_t) {
            best_t       = tg;
            best_wnrm    = float3(0.0, 0.0, 1.0);
            best_alb     = float3(hdr.ground_r, hdr.ground_g, hdr.ground_b);
            best_emi     = float3(0.0);
            hit_terrain  = false;
            hit_interval = true;
        }
    }

    float3 near_rad  = float3(0.0);
    float  near_beta = 1.0;

    if (hit_interval) {
        float3 hit_wp = world_pos + rd * best_t;
        if (dot(best_wnrm, rd) > 0.0 && !hit_terrain) {
            best_wnrm = -best_wnrm;
        }
        float3 hit_vn = normalize(view_rotate(q, best_wnrm));
        near_rad      = lighting_eval_direct_radiance(
            hdr, hit_wp, best_wnrm, hit_vn, best_alb, best_emi, hit_terrain);
        near_beta     = 0.0;
    }

    // Exact Split / Holographic Radiance Cascade coarse-to-fine interval merge:
    // L_c(x, m) = L_near + beta_near * 0.5 * (I(L_{c+1, 2m}) + I(L_{c+1, 2m+1}))
    float3 merged_rad  = near_rad;
    float  merged_beta = near_beta;

    if (c == 3u) {
        if (near_beta > 0.0) {
            merged_rad += near_beta * lighting_eval_sky_env(hdr, rd);
        }
    } else if (near_beta > 0.0) {
        uint pw     = max(hdr.hrc_w0 >> (c + 1u), 1u);
        uint ph     = max(hdr.hrc_h0 >> (c + 1u), 1u);
        uint pm_cnt = 4u << (c + 1u);
        uint poff   = hdr.hrc_offset_rays[c + 1u];

        LightingHrcRayGpu r0 = sample_parent_cascade_bilinear(hrc_buf, poff, pw, ph, pm_cnt, uv, dir_m * 2u);
        LightingHrcRayGpu r1 = sample_parent_cascade_bilinear(hrc_buf, poff, pw, ph, pm_cnt, uv, dir_m * 2u + 1u);
        float3 far_avg = 0.5 * (lighting_unpack_rgb9e5_gpu(r0.rad_rgb9e5) +
                                lighting_unpack_rgb9e5_gpu(r1.rad_rgb9e5));
        merged_rad  = near_rad + near_beta * far_avg;
        merged_beta = near_beta * 0.5 * (r0.beta + r1.beta);
    }

    LightingHrcRayGpu out_ray;
    out_ray.rad_rgb9e5 = lighting_pack_rgb9e5_gpu(merged_rad);
    out_ray.beta       = merged_beta;
    hrc_buf[out_idx]   = out_ray;
}
