// Slang 2026 SSILVB (Screen-Space Indirect Lighting with 32-Bit Visibility Bitmasks)
// + Cascade-0 HRC Multi-Bounce Fusion + SLM Cross-Bilateral Filter + Temporal Reprojection.

#include "lighting_gi_common.slang"

[[vk::push_constant]] LightingComputePush pc;

groupshared LightingFrameHeader s_hdr;
groupshared float  s_tile_vz[64];
groupshared float3 s_tile_nrm[64];
groupshared float  s_tile_ao[64];
groupshared float3 s_tile_bent[64];
groupshared float3 s_tile_irr[64];

// 4-FMA minimax atan2 approximation (< 0.0009 deg max error, zero SFU stalls)
float fast_atan2_gpu(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    float mx = max(ax, ay);
    if (mx < 1.0e-8) {
        return 0.0;
    }
    float u  = min(ax, ay) / mx;
    float u2 = u * u;
    float r  = ((-0.0464964749 * u2 + 0.15931422) * u2 - 0.327622764) * (u2 * u) + u;
    if (ay > ax) {
        r = k_gi_half_pi - r;
    }
    if (x < 0.0) {
        r = k_gi_pi - r;
    }
    return y < 0.0 ? -r : r;
}

// Exact 32-bit angular sector bitmask for normalized horizon interval [a_min, a_max] in [0, 1]
uint ssilvb_sector_mask_gpu(float a_min, float a_max) {
    float a0 = clamp(a_min, 0.0, 1.0);
    float a1 = clamp(a_max, 0.0, 1.0);
    if (a1 <= a0) {
        return 0u;
    }
    uint b0 = uint(floor(a0 * 32.0));
    uint b1 = min(uint(ceil(a1 * 32.0)), 32u);
    if (b0 >= 32u || b1 <= b0) {
        return 0u;
    }
    uint cnt = b1 - b0;
    if (cnt >= 32u) {
        return 0xFFFFFFFFu;
    }
    return ((1u << cnt) - 1u) << b0;
}

// Roberts R2 / Interleaved Gradient Noise spatio-temporal blue-like sequence
float2 ssilvb_noise(uint2 px, uint frame_idx) {
    uint  f  = frame_idx & 63u;
    float n1 = frac(52.9829189 * frac(0.06711056 * float(px.x + f * 5u) + 0.00583715 * float(px.y + f * 3u)));
    float n2 = frac(float(px.x) * 0.754877666 + float(px.y) * 0.569840291 + float(f) * 0.618033989);
    return float2(n1, n2);
}

float3 hrc_c0_dir(uint m) {
    float phi  = (float(m & 3u) + 0.5) * (0.5 * k_gi_pi);
    float z    = 0.26;
    float r_xy = sqrt(1.0 - z * z);
    return float3(r_xy * cos(phi), r_xy * sin(phi), z);
}

float3 sample_hrc_c0_irradiance(
    LightingFrameHeader hdr,
    float2              uv,
    float3              world_nrm,
    float               near_ao)
{
    if (hdr.hrc_bda == 0ull || hdr.hrc_w0 == 0u || hdr.hrc_h0 == 0u) {
        return float3(0.0);
    }

    LightingHrcRayGpu *hrc_buf = (LightingHrcRayGpu *)hdr.hrc_bda;
    uint   cw   = hdr.hrc_w0;
    uint   ch   = hdr.hrc_h0;
    float2 tc   = clamp(uv, float2(0.0), float2(1.0)) * float2(float(cw), float(ch)) - float2(0.5);
    int2   base = int2(floor(tc));
    float2 f    = tc - float2(base);
    int2   maxc = int2(int(cw) - 1, int(ch) - 1);

    float3 irr_accum = float3(0.0);
    float  w_accum   = 0.0;

    [unroll]
    for (uint m = 0u; m < 4u; ++m) {
        float3 w_dir = hrc_c0_dir(m);
        // Smooth wrap-cosine hemisphere kernel across the 4 Lambert solid-angle quadrants
        float cos_w = clamp(dot(world_nrm, w_dir) * 0.75 + 0.35, 0.0, 1.0);
        if (cos_w <= 1.0e-4) {
            continue;
        }

        float3 rad_bilin  = float3(0.0);
        float  beta_bilin = 0.0;
        [unroll]
        for (int dy = 0; dy <= 1; ++dy) {
            [unroll]
            for (int dx = 0; dx <= 1; ++dx) {
                int2  p   = clamp(base + int2(dx, dy), int2(0), maxc);
                float wb  = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
                uint  idx = hdr.hrc_offset_rays[0] + (uint(p.y) * cw + uint(p.x)) * 4u + m;
                LightingHrcRayGpu r = hrc_buf[idx];
                rad_bilin  += lighting_unpack_rgb9e5_gpu(r.rad_rgb9e5) * wb;
                beta_bilin += r.beta * wb;
            }
        }

        // Subtract unoccluded sky baseline (beta_bilin * sky_env) because surface shaders
        // already evaluate direct sky hemisphere ambient modulated by multibounce VBAO.
        // This isolates true surface-to-surface indirect bounce & emissive radiance!
        float3 sky_base   = lighting_eval_sky_env(hdr, w_dir) * clamp(beta_bilin, 0.0, 1.0);
        float3 bounce_rad = max(rad_bilin - sky_base, float3(0.0));
        irr_accum += bounce_rad * cos_w;
        w_accum   += cos_w;
    }

    float3 hrc_irr = w_accum > 1.0e-4 ? (irr_accum / w_accum) : float3(0.0);
    return hrc_irr * lerp(0.38, 1.0, near_ao);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    uint lidx = gtid.y * 8u + gtid.x;
    s_tile_vz[lidx]   = -1.0e20;
    s_tile_nrm[lidx]  = float3(0.0, 0.0, 1.0);
    s_tile_ao[lidx]   = 1.0;
    s_tile_bent[lidx] = float3(0.0, 0.0, 1.0);
    s_tile_irr[lidx]  = float3(0.0);

    if (pc.env_bda == 0ull) {
        return;
    }
    if (lidx == 0u) {
        s_hdr = ((LightingFrameHeader *)(pc.env_bda + k_lighting_header_offset))[0];
    }
    GroupMemoryBarrierWithGroupSync();

    LightingFrameHeader hdr = s_hdr;
    bool in_bounds = (hdr.magic == k_lighting_magic && hdr.gbuf_bda != 0ull &&
                      hdr.gi_curr_bda != 0ull && tid.x < hdr.gi_width && tid.y < hdr.gi_height);

    bool   valid_surface = false;
    float  center_vz     = -1.0e20;
    float3 center_vnrm   = float3(0.0, 0.0, 1.0);
    float3 center_wnrm   = float3(0.0, 0.0, 1.0);
    float3 center_vpos   = float3(0.0);
    float3 center_wpos   = float3(0.0);
    float2 center_uv     = float2(0.0);
    float  raw_ao        = 1.0;
    float3 raw_bent      = float3(0.0, 0.0, 1.0);
    float3 raw_irr       = float3(0.0);

    if (in_bounds) {
        LightingGbufTexelGpu *gbuf = (LightingGbufTexelGpu *)hdr.gbuf_bda;
        LightingGbufTexelGpu ctex  = gbuf[tid.y * hdr.gi_width + tid.x];
        center_vz = ctex.view_z;

        if (center_vz > -1.0e19) {
            valid_surface = true;
            center_uv     = (float2(tid.xy) + float2(0.5)) / float2(float(hdr.gi_width), float(hdr.gi_height));
            float2 fpx    = center_uv * float2(float(hdr.full_width), float(hdr.full_height));
            float2 vxy    = lighting_pixel_to_view_xy(hdr, fpx);
            center_vpos   = float3(vxy.x, vxy.y, center_vz);
            center_vnrm   = lighting_unpack_oct_snorm16_gpu(ctex.oct_nrm_s16);

            float4 q    = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
            center_wnrm = normalize(lighting_inv_view_rotate(q, center_vnrm));
            center_wpos = lighting_view_to_world(hdr, center_vpos);

            uint   num_slices = clamp(hdr.ssilvb_slices, 1u, 8u);
            uint   num_steps  = clamp(hdr.ssilvb_steps,  2u, 16u);
            float2 jitter     = ssilvb_noise(tid.xy, hdr.frame_index);

            float gi_scale  = float(hdr.gi_width) / max(float(hdr.full_width), 1.0);
            float max_r_tex = max(hdr.ssilvb_radius_px * gi_scale, 4.0);
            float max_r_w   = hdr.ssilvb_radius_px / max(hdr.zoom, 1.0e-4);
            float thick_w   = max(hdr.thickness_world, 1.0e-3);
            float2 step_to_vxy = float2(
                 float(hdr.full_width)  / (max(float(hdr.gi_width),  1.0) * max(hdr.zoom, 1.0e-5)),
                -float(hdr.full_height) / (max(float(hdr.gi_height), 1.0) * max(hdr.zoom, 1.0e-5))
            );

            float  vis_accum  = 0.0;
            float3 bent_accum = float3(0.0);
            float3 ind_accum  = float3(0.0);

            for (uint s = 0u; s < num_slices; ++s) {
                float  phi   = (float(s) + jitter.x) * (k_gi_pi / float(num_slices));
                float  scos  = cos(phi);
                float  ssin  = sin(phi);
                // Screen +Y points down while view +Y points up
                float2 s_step_dir = float2(scos, -ssin);

                // Project view normal onto slice plane spanned by T=(scos, ssin, 0) and V=(0, 0, 1)
                float  pnx      = -ssin;
                float  pny      =  scos;
                float  ndp      = center_vnrm.x * pnx + center_vnrm.y * pny;
                float3 proj_n   = float3(center_vnrm.x - ndp * pnx, center_vnrm.y - ndp * pny, center_vnrm.z);
                float  proj_len = length(proj_n);
                if (proj_len < 1.0e-4) {
                    continue;
                }
                float n_t   = (proj_n.x * scos + proj_n.y * ssin) / proj_len;
                float n_v   = proj_n.z / proj_len;
                float n_ang = fast_atan2_gpu(n_t, max(n_v, 1.0e-4));

                uint vis_mask = 0u;

                [unroll]
                for (int side = -1; side <= 1; side += 2) {
                    float dir_sgn = float(side);
                    for (uint st = 1u; st <= num_steps; ++st) {
                        float u_step = (float(st) - 1.0 + jitter.y) / float(num_steps);
                        float r_tex  = lerp(1.15, max_r_tex, u_step * sqrt(max(u_step, 1.0e-4)));
                        int2  s_coord = int2(round(float2(tid.xy) + s_step_dir * (dir_sgn * r_tex)));
                        if (s_coord.x < 0 || s_coord.y < 0 ||
                            s_coord.x >= int(hdr.gi_width) || s_coord.y >= int(hdr.gi_height)) {
                            break;
                        }

                        LightingGbufTexelGpu nb = gbuf[uint(s_coord.y) * hdr.gi_width + uint(s_coord.x)];
                        if (nb.view_z <= -1.0e19) {
                            continue;
                        }

                        float2 d_vxy = float2(s_coord - int2(tid.xy)) * step_to_vxy;
                        float3 delta = float3(d_vxy.x, d_vxy.y, nb.view_z - center_vz);

                        float dist2 = dot(delta, delta);
                        if (dist2 < 1.0e-6 || dist2 > (max_r_w * max_r_w * 2.25)) {
                            continue;
                        }

                        float dist     = sqrt(dist2);
                        float falloff  = saturate(1.0 - dist / (max_r_w * 1.35));
                        float eff_thk  = thick_w * lerp(0.55, 1.45, u_step);
                        float dz_front = delta.z;
                        float dz_back  = delta.z - eff_thk;
                        float d_tan    = delta.x * scos + delta.y * ssin;

                        float ang_f = fast_atan2_gpu(d_tan, dz_front) - n_ang;
                        float ang_b = fast_atan2_gpu(d_tan, dz_back)  - n_ang;
                        float a_min = (min(ang_f, ang_b) + k_gi_half_pi) * (1.0 / k_gi_pi);
                        float a_max = (max(ang_f, ang_b) + k_gi_half_pi) * (1.0 / k_gi_pi);
                        // Distance-attenuated horizon fade prevents hard cutoff rings at max radius
                        a_max = lerp(a_min, a_max, smoothstep(0.0, 0.25, falloff));

                        uint hit_bits       = ssilvb_sector_mask_gpu(a_min, a_max);
                        uint newly_occluded = hit_bits & ~vis_mask;
                        if (newly_occluded != 0u) {
                            float  sec_w    = float(countbits(newly_occluded)) * (1.0 / 32.0);
                            float3 dir_in   = delta / dist;
                            float3 nb_vnrm  = lighting_unpack_oct_snorm16_gpu(nb.oct_nrm_s16);
                            float3 nb_rad   = lighting_unpack_rgb9e5_gpu(nb.direct_rgb9e5);
                            float  recv_cos = saturate(dot(center_vnrm, dir_in) * 0.85 + 0.15);
                            float  emit_cos = saturate(dot(nb_vnrm, -dir_in) * 0.80 + 0.20);

                            ind_accum += nb_rad * (sec_w * recv_cos * emit_cos * falloff);
                            vis_mask  |= hit_bits;
                        }
                    }
                }

                uint  occ_bits = countbits(vis_mask);
                float sl_vis   = clamp(proj_len * (1.0 - float(occ_bits) * (1.0 / 32.0)), 0.0, 1.0);
                uint  unocc_lo = 16u - countbits(vis_mask & 0x0000FFFFu);
                uint  unocc_hi = 16u - countbits((vis_mask >> 16u) & 0x0000FFFFu);
                float tilt     = (float(unocc_hi) - float(unocc_lo)) * (1.0 / 16.0);

                vis_accum  += sl_vis;
                bent_accum += normalize(center_vnrm + float3(scos, ssin, 0.0) * (tilt * 0.85));
            }

            float inv_slices = 1.0 / float(num_slices);
            raw_ao   = clamp(vis_accum * inv_slices, 0.0, 1.0);
            raw_bent = dot(bent_accum, bent_accum) > 1.0e-5 ? normalize(bent_accum) : center_vnrm;

            float3 local_bounce = ind_accum * inv_slices * 1.65;
            float3 far_hrc_irr  = sample_hrc_c0_irradiance(hdr, center_uv, center_wnrm, raw_ao);
            raw_irr             = local_bounce + far_hrc_irr;

            s_tile_vz[lidx]   = center_vz;
            s_tile_nrm[lidx]  = center_vnrm;
            s_tile_ao[lidx]   = raw_ao;
            s_tile_bent[lidx] = raw_bent;
            s_tile_irr[lidx]  = raw_irr;
        }
    }

    // Zero-extra-DRAM-pass 8x8 SLM cross-bilateral edge-stopping spatial filter
    GroupMemoryBarrierWithGroupSync();

    if (!in_bounds) {
        return;
    }

    LightingGiTexel *curr_grid = (LightingGiTexel *)hdr.gi_curr_bda;
    uint out_linear = tid.y * hdr.gi_width + tid.x;

    if (!valid_surface) {
        LightingGiTexel sky_out;
        sky_out.view_z          = -1.0e20;
        sky_out.packed_nrm_bent = 0u;
        sky_out.irr_rgb9e5      = 0u;
        sky_out.ao_conf_u16     = 65535u;
        curr_grid[out_linear]   = sky_out;
        return;
    }

    float  inv_sigma_z = max(hdr.zoom * 0.09, 0.005);
    float  sw_sum      = 1.0;
    float  ao_filt     = raw_ao;
    float3 bent_filt   = raw_bent;
    float3 irr_filt    = raw_irr;

    [unroll]
    for (int ky = -1; ky <= 1; ++ky) {
        [unroll]
        for (int kx = -1; kx <= 1; ++kx) {
            if (kx == 0 && ky == 0) {
                continue;
            }
            int2 lp = int2(gtid.xy) + int2(kx, ky);
            if (lp.x < 0 || lp.y < 0 || lp.x >= 8 || lp.y >= 8) {
                continue;
            }
            uint   nlidx = uint(lp.y) * 8u + uint(lp.x);
            float  nvz   = s_tile_vz[nlidx];
            if (nvz <= -1.0e19) {
                continue;
            }
            float3 nnrm = s_tile_nrm[nlidx];
            float  kw   = (kx == 0 || ky == 0) ? 0.55 : 0.32;
            float  dz_s = abs(nvz - center_vz) * inv_sigma_z;
            float  wz   = rcp(1.0 + dz_s + 0.48 * dz_s * dz_s);
            float  nd   = max(dot(nnrm, center_vnrm), 0.0);
            float  wn   = nd * nd * nd * nd;
            float  w    = kw * wz * wn;

            sw_sum    += w;
            ao_filt   += s_tile_ao[nlidx]   * w;
            bent_filt += s_tile_bent[nlidx] * w;
            irr_filt  += s_tile_irr[nlidx]  * w;
        }
    }

    float inv_sw = 1.0 / sw_sum;
    ao_filt      = clamp(ao_filt * inv_sw, 0.0, 1.0);
    bent_filt    = dot(bent_filt, bent_filt) > 1.0e-5 ? normalize(bent_filt * inv_sw) : center_vnrm;
    irr_filt     = max(irr_filt * inv_sw, float3(0.0));

    // Camera-motion temporal reprojection & confidence-weighted exponential accumulation
    float conf = 1.0 / 16.0;
    if (hdr.history_valid != 0u && hdr.gi_prev_bda != 0ull) {
        float  prev_vz = 0.0;
        float2 puv     = lighting_world_to_prev_gi_uv(hdr, center_wpos, prev_vz);
        if (all(puv >= float2(0.0)) && all(puv <= float2(1.0))) {
            LightingGiTexel *prev_grid = (LightingGiTexel *)hdr.gi_prev_bda;
            float2 ptc   = puv * float2(float(hdr.gi_width), float(hdr.gi_height)) - float2(0.5);
            int2   pbase = int2(floor(ptc));
            float2 pf    = ptc - float2(pbase);
            int2   pmax  = int2(int(hdr.gi_width) - 1, int(hdr.gi_height) - 1);

            float4 pq       = float4(hdr.prev_qx, hdr.prev_qy, hdr.prev_qz, hdr.prev_qw);
            float3 exp_pnrm = normalize(view_rotate(pq, center_wnrm));
            float  pw_sum   = 0.0;
            float  p_ao     = 0.0;
            float3 p_bent   = float3(0.0);
            float3 p_irr    = float3(0.0);
            float  p_conf   = 0.0;

            [unroll]
            for (int dy = 0; dy <= 1; ++dy) {
                [unroll]
                for (int dx = 0; dx <= 1; ++dx) {
                    int2 pp = clamp(pbase + int2(dx, dy), int2(0), pmax);
                    LightingGiTexel pt = prev_grid[uint(pp.y) * hdr.gi_width + uint(pp.x)];
                    if (pt.view_z <= -1.0e19) {
                        continue;
                    }
                    float3 ptnrm = lighting_unpack_oct_snorm8_gpu(pt.packed_nrm_bent & 0xFFFFu);
                    float  dz_ok = abs(pt.view_z - prev_vz) * max(hdr.prev_zoom, 0.02);
                    float  nd_ok = dot(ptnrm, exp_pnrm);
                    if (dz_ok > 14.0 || nd_ok < 0.72) {
                        continue;
                    }
                    float wb = (dx == 0 ? (1.0 - pf.x) : pf.x) * (dy == 0 ? (1.0 - pf.y) : pf.y);
                    float w  = max(wb, 0.01) * rcp(1.0 + dz_ok * 0.14) * (nd_ok * nd_ok);
                    pw_sum  += w;
                    p_ao    += (float(pt.ao_conf_u16 & 0xFFFFu) * (1.0 / 65535.0)) * w;
                    p_bent  += lighting_unpack_oct_snorm8_gpu((pt.packed_nrm_bent >> 16u) & 0xFFFFu) * w;
                    p_irr   += lighting_unpack_rgb9e5_gpu(pt.irr_rgb9e5) * w;
                    p_conf  += (float((pt.ao_conf_u16 >> 16u) & 0xFFFFu) * (1.0 / 65535.0)) * w;
                }
            }

            if (pw_sum > 1.0e-4) {
                float inv_pw   = 1.0 / pw_sum;
                float hist_ao  = p_ao   * inv_pw;
                float3 hist_bn = normalize(lighting_inv_view_rotate(pq, p_bent * inv_pw));
                float3 hist_bv = normalize(view_rotate(float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw), hist_bn));
                float3 hist_ir = p_irr  * inv_pw;
                float hist_cf  = p_conf * inv_pw;

                conf        = min(hist_cf + (1.0 / 16.0), 1.0);
                float alpha = lerp(0.38, 0.14, conf);

                // Soft variance-aware clamp so moving glowing cubes & bouncing solids leave zero ghost trails
                float3 irr_max = max(irr_filt * 2.2 + float3(0.18), raw_irr * 1.8 + float3(0.12));
                hist_ir        = clamp(hist_ir, float3(0.0), irr_max);

                ao_filt   = lerp(hist_ao, ao_filt, alpha);
                bent_filt = normalize(lerp(hist_bv, bent_filt, alpha));
                irr_filt  = lerp(hist_ir, irr_filt, alpha);
            }
        }
    }

    uint nrm_u16  = lighting_pack_oct_snorm8_gpu(center_vnrm);
    uint bent_u16 = lighting_pack_oct_snorm8_gpu(bent_filt);
    uint ao_u16   = uint(clamp(round(ao_filt * 65535.0), 0.0, 65535.0));
    uint conf_u16 = uint(clamp(round(conf    * 65535.0), 0.0, 65535.0));

    LightingGiTexel out_gi;
    out_gi.view_z          = center_vz;
    out_gi.packed_nrm_bent = nrm_u16 | (bent_u16 << 16u);
    out_gi.irr_rgb9e5      = lighting_pack_rgb9e5_gpu(irr_filt);
    out_gi.ao_conf_u16     = ao_u16 | (conf_u16 << 16u);
    curr_grid[out_linear]  = out_gi;
}
