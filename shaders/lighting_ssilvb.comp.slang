// Slang 2026 SSILVB (32-Bit Visibility Bitmask) + HRC C0 Fusion + Temporal + Seamless 10x10 Halo Denoise
// Pass 0 (pc.flags == 0u): SSILVB + HRC C0 + variance-clamped temporal accumulation -> gi_curr_bda
// Pass 1 (pc.flags == 1u): Seamless 10x10 padded-SLM cross-bilateral filter: gi_curr_bda -> gbuf_bda (0 seams, 0 extra VRAM)

#include "lighting_gi_common.slang"

[[vk::push_constant]] LightingComputePush pc;

groupshared LightingFrameHeader s_hdr;
groupshared LightingShadowBlock s_sb;
// 10x10 padded halo tile for seam-free 3x3 cross-bilateral filtering across all workgroup borders
groupshared float  s_halo_vz[100];
groupshared float3 s_halo_nrm[100];
groupshared float  s_halo_ao[100];
groupshared float3 s_halo_bent[100];
groupshared float3 s_halo_irr[100];

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

uint ssilvb_sector_mask_gpu(float a_min, float a_max, float bit_jit) {
    float a0 = clamp(a_min, 0.0, 1.0);
    float a1 = clamp(a_max, 0.0, 1.0);
    if (a1 - a0 <= 1.0e-4) {
        return 0u;
    }
    uint b0 = min(uint(floor(a0 * 32.0 + bit_jit)), 32u);
    uint b1 = min(uint(floor(a1 * 32.0 + bit_jit)), 32u);
    if (b0 >= 32u || b1 <= b0) {
        return 0u;
    }
    uint cnt = b1 - b0;
    if (cnt >= 32u) {
        return 0xFFFFFFFFu;
    }
    return ((1u << cnt) - 1u) << b0;
}

float ssilvb_ign(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float3 ssilvb_noise(uint2 px, uint frame_idx) {
    float  f  = float(frame_idx & 63u);
    float2 q  = float2(px) + float2(f * 5.588238, f * 3.141592);
    float  n1 = ssilvb_ign(q);
    float  n2 = ssilvb_ign(q + float2(17.0, 29.0));
    float  n3 = ssilvb_ign(q + float2(43.0, 11.0));
    return float3(n1, n2, n3);
}

float3 hrc_c0_dir_jit(uint m, uint2 probe, uint frame_idx, bool ground_probe) {
    uint  bayer4 = ((probe.x & 1u) | ((probe.y & 1u) << 1u));
    uint  perm   = (bayer4 ^ (bayer4 >> 1u)) + ((probe.y & 1u) != 0u ? 1u : 0u);
    float jit_p  = frac(0.25 * (float(perm & 3u) + 0.5) + float(frame_idx & 15u) * 0.6180339887);
    float u_phi  = (float(m & 3u) + 0.15 + 0.70 * jit_p) * 0.25;
    float phi    = u_phi * (2.0 * k_gi_pi);
    float z      = ground_probe ? 0.30 : 0.27;
    float r_xy   = sqrt(1.0 - z * z);
    return float3(r_xy * cos(phi), r_xy * sin(phi), z);
}

float3 sample_hrc_c0_irradiance(
    LightingFrameHeader hdr,
    float2              uv,
    float3              world_nrm,
    float               near_ao,
    bool                ground_probe)
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
    for (int dy = 0; dy <= 1; ++dy) {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx) {
            int2  p     = clamp(base + int2(dx, dy), int2(0), maxc);
            float wb    = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            uint  pbase = hdr.hrc_offset_rays[0] + (uint(p.y) * cw + uint(p.x)) * 4u;

            [unroll]
            for (uint m = 0u; m < 4u; ++m) {
                float3 w_dir = hrc_c0_dir_jit(m, uint2(p), hdr.frame_index, ground_probe);
                float  cos_w = clamp(dot(world_nrm, w_dir) * 0.75 + 0.35, 0.0, 1.0);
                if (cos_w <= 1.0e-4) {
                    continue;
                }
                LightingHrcRayGpu r = hrc_buf[pbase + m];
                float3 rad          = lighting_unpack_rgb9e5_gpu(r.rad_rgb9e5);
                float3 sky_base     = lighting_eval_sky_env(hdr, w_dir) * clamp(r.beta, 0.0, 1.0);
                float3 bounce_rad   = max(rad - sky_base, float3(0.0));
                float  w_total      = wb * cos_w;
                irr_accum += bounce_rad * w_total;
                w_accum   += w_total;
            }
        }
    }

    float3 hrc_irr = w_accum > 1.0e-4 ? (irr_accum / w_accum) : float3(0.0);
    return hrc_irr * lerp(0.68, 1.0, near_ao);
}

void run_seamless_denoise_pass(LightingFrameHeader hdr, uint3 tid, uint3 gtid, uint3 gid) {
    uint lidx = gtid.y * 8u + gtid.x;
    LightingGiTexel *src_grid = (LightingGiTexel *)hdr.gi_curr_bda;
    LightingGiTexel *dst_grid = (LightingGiTexel *)hdr.gbuf_bda;

    int2 tile_origin = int2(gid.xy * 8u) - int2(1, 1);
    int2 max_coord   = int2(int(hdr.gi_width) - 1, int(hdr.gi_height) - 1);

    // Cooperatively load 10x10 padded halo (100 texels across 64 threads -> at most 2 loads/thread)
    for (uint slot = lidx; slot < 100u; slot += 64u) {
        int hy = int(slot / 10u);
        int hx = int(slot - uint(hy) * 10u);
        int2 sc = clamp(tile_origin + int2(hx, hy), int2(0), max_coord);
        LightingGiTexel tex = src_grid[uint(sc.y) * hdr.gi_width + uint(sc.x)];
        s_halo_vz[slot] = tex.view_z;
        if (tex.view_z > -1.0e19) {
            s_halo_nrm[slot]  = lighting_unpack_oct_snorm8_gpu(tex.packed_nrm_bent & 0xFFFFu);
            s_halo_bent[slot] = lighting_unpack_oct_snorm8_gpu((tex.packed_nrm_bent >> 16u) & 0xFFFFu);
            s_halo_irr[slot]  = lighting_unpack_rgb9e5_gpu(tex.irr_rgb9e5);
            s_halo_ao[slot]   = float(tex.ao_conf_u16 & 0xFFFFu) * (1.0 / 65535.0);
        } else {
            s_halo_nrm[slot]  = float3(0.0, 0.0, 1.0);
            s_halo_bent[slot] = float3(0.0, 0.0, 1.0);
            s_halo_irr[slot]  = float3(0.0);
            s_halo_ao[slot]   = 1.0;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid.x >= hdr.gi_width || tid.y >= hdr.gi_height) {
        return;
    }

    uint out_linear = tid.y * hdr.gi_width + tid.x;
    uint cslot      = (gtid.y + 1u) * 10u + (gtid.x + 1u);
    float center_vz = s_halo_vz[cslot];
    if (center_vz <= -1.0e19) {
        LightingGiTexel sky_out;
        sky_out.view_z          = -1.0e20;
        sky_out.packed_nrm_bent = 0u;
        sky_out.irr_rgb9e5      = 0u;
        sky_out.ao_conf_u16     = 65535u;
        dst_grid[out_linear]    = sky_out;
        return;
    }

    float3 center_nrm = s_halo_nrm[cslot];
    float  inv_sig_z  = max(hdr.zoom * 0.055, 0.004);
    float  sw_sum     = 1.0;
    float  ao_filt    = s_halo_ao[cslot];
    float3 bent_filt  = s_halo_bent[cslot];
    float3 irr_filt   = s_halo_irr[cslot];

    [unroll]
    for (int ky = -1; ky <= 1; ++ky) {
        [unroll]
        for (int kx = -1; kx <= 1; ++kx) {
            if (kx == 0 && ky == 0) {
                continue;
            }
            uint  nslot = uint(int(gtid.y) + 1 + ky) * 10u + uint(int(gtid.x) + 1 + kx);
            float nvz   = s_halo_vz[nslot];
            if (nvz <= -1.0e19) {
                continue;
            }
            float  kw   = (kx == 0 || ky == 0) ? 0.58 : 0.34;
            float  dz_s = abs(nvz - center_vz) * inv_sig_z;
            float  wz   = rcp(1.0 + dz_s + 0.45 * dz_s * dz_s);
            float  nd   = max(dot(s_halo_nrm[nslot], center_nrm), 0.0);
            float  wn   = nd * nd * nd;
            float  w    = kw * wz * wn;

            sw_sum    += w;
            ao_filt   += s_halo_ao[nslot]   * w;
            bent_filt += s_halo_bent[nslot] * w;
            irr_filt  += s_halo_irr[nslot]  * w;
        }
    }

    float inv_sw = 1.0 / sw_sum;
    ao_filt      = clamp(ao_filt * inv_sw, 0.0, 1.0);
    bent_filt    = dot(bent_filt, bent_filt) > 1.0e-5 ? normalize(bent_filt * inv_sw) : center_nrm;
    irr_filt     = max(irr_filt * inv_sw, float3(0.0));

    uint nrm_u16  = lighting_pack_oct_snorm8_gpu(center_nrm);
    uint bent_u16 = lighting_pack_oct_snorm8_gpu(bent_filt);
    uint ao_u16   = uint(clamp(round(ao_filt * 65535.0), 0.0, 65535.0));

    LightingGiTexel out_gi;
    out_gi.view_z          = center_vz;
    out_gi.packed_nrm_bent = nrm_u16 | (bent_u16 << 16u);
    out_gi.irr_rgb9e5      = lighting_pack_rgb9e5_gpu(irr_filt);
    out_gi.ao_conf_u16     = ao_u16 | (65535u << 16u);
    dst_grid[out_linear]   = out_gi;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    if (pc.env_bda == 0ull) {
        return;
    }
    uint lidx = gtid.y * 8u + gtid.x;
    if (lidx == 0u) {
        s_hdr = ((LightingFrameHeader *)(pc.env_bda + k_lighting_header_offset))[0];
        s_sb  = ((LightingShadowBlock *)(pc.env_bda + k_lighting_shadow_offset))[0];
    }
    GroupMemoryBarrierWithGroupSync();

    LightingFrameHeader hdr = s_hdr;
    LightingShadowBlock sb  = s_sb;
    if (hdr.magic != k_lighting_magic || hdr.gbuf_bda == 0ull || hdr.gi_curr_bda == 0ull ||
        hdr.gi_width == 0u || hdr.gi_height == 0u) {
        return;
    }

    if (pc.flags != 0u) {
        run_seamless_denoise_pass(hdr, tid, gtid, gid);
        return;
    }

    if (tid.x >= hdr.gi_width || tid.y >= hdr.gi_height) {
        return;
    }

    LightingGbufTexelGpu *gbuf      = (LightingGbufTexelGpu *)hdr.gbuf_bda;
    LightingGiTexel      *curr_grid = (LightingGiTexel *)hdr.gi_curr_bda;
    uint out_linear = tid.y * hdr.gi_width + tid.x;
    LightingGbufTexelGpu ctex = gbuf[out_linear];
    float center_vz = ctex.view_z;

    if (center_vz <= -1.0e19) {
        LightingGiTexel sky_out;
        sky_out.view_z          = -1.0e20;
        sky_out.packed_nrm_bent = 0u;
        sky_out.irr_rgb9e5      = 0u;
        sky_out.ao_conf_u16     = 65535u;
        curr_grid[out_linear]   = sky_out;
        return;
    }

    float2 center_uv   = (float2(tid.xy) + float2(0.5)) / float2(float(hdr.gi_width), float(hdr.gi_height));
    float2 fpx         = center_uv * float2(float(hdr.full_width), float(hdr.full_height));
    float3 center_vpos = lighting_pixel_to_view_pos(hdr, sb.persp, fpx, center_vz);
    float3 center_vnrm = lighting_unpack_oct_snorm16_gpu(ctex.oct_nrm_s16);

    float4 q           = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 center_wnrm = normalize(lighting_inv_view_rotate(q, center_vnrm));
    float3 center_wpos = lighting_view_to_world(hdr, center_vpos);

    uint   num_slices = clamp(hdr.ssilvb_slices, 1u, 8u);
    uint   num_steps  = clamp(hdr.ssilvb_steps,  2u, 16u);
    float3 jitter     = ssilvb_noise(tid.xy, hdr.frame_index);

    float gi_scale   = float(hdr.gi_width) / max(float(hdr.full_width), 1.0);
    float max_r_tex  = max(hdr.ssilvb_radius_px * gi_scale, 4.0);
    float center_wc  = (sb.persp > 1.0e-7) ? max(1.0 - center_vz * sb.persp, 0.08) : 1.0;
    float max_r_w    = (hdr.ssilvb_radius_px / max(hdr.zoom, 1.0e-4)) * center_wc;
    float thick_w    = max(hdr.thickness_world, 1.0e-3);
    float2 inv_gi_wh = float2(float(hdr.full_width)  / max(float(hdr.gi_width),  1.0),
                              float(hdr.full_height) / max(float(hdr.gi_height), 1.0));
    float3 view_v    = (sb.persp > 1.0e-7)
        ? normalize(float3(0.0, 0.0, 1.0 / sb.persp) - center_vpos)
        : float3(0.0, 0.0, 1.0);

    float  vis_accum  = 0.0;
    float  vis_weight = 0.0;
    float3 bent_accum = float3(0.0);
    float3 ind_accum  = float3(0.0);

    for (uint s = 0u; s < num_slices; ++s) {
        float  phi        = (float(s) + jitter.x) * (k_gi_pi / float(num_slices));
        float  scos       = cos(phi);
        float  ssin       = sin(phi);
        float2 s_step_dir = float2(scos, -ssin);
        float3 slice_v    = float3(scos, ssin, 0.0);

        float3 side_raw = cross(view_v, slice_v);
        float  side_l2  = dot(side_raw, side_raw);
        if (side_l2 < 1.0e-6) {
            continue;
        }
        float3 side_n   = side_raw * rsqrt(side_l2);
        float3 proj_n   = center_vnrm - side_n * dot(center_vnrm, side_n);
        float  proj_len = length(proj_n);
        if (proj_len < 1.0e-4) {
            continue;
        }
        float3 n_dir = proj_n / proj_len;
        float3 t_nrm = cross(side_n, n_dir);

        uint vis_mask = 0u;

        [unroll]
        for (int side = -1; side <= 1; side += 2) {
            float dir_sgn = float(side);
            for (uint st = 1u; st <= num_steps; ++st) {
                float u_step  = (float(st) - 1.0 + jitter.y) / float(num_steps);
                float r_tex   = lerp(1.15, max_r_tex, u_step * sqrt(max(u_step, 1.0e-4)));
                int2  s_coord = int2(round(float2(tid.xy) + s_step_dir * (dir_sgn * r_tex)));
                if (s_coord.x < 0 || s_coord.y < 0 ||
                    s_coord.x >= int(hdr.gi_width) || s_coord.y >= int(hdr.gi_height)) {
                    break;
                }

                LightingGbufTexelGpu nb = gbuf[uint(s_coord.y) * hdr.gi_width + uint(s_coord.x)];
                if (nb.view_z <= -1.0e19) {
                    continue;
                }

                float2 nb_fpx  = (float2(s_coord) + float2(0.5)) * inv_gi_wh;
                float3 nb_vpos = lighting_pixel_to_view_pos(hdr, sb.persp, nb_fpx, nb.view_z);
                float3 delta   = nb_vpos - center_vpos;

                float dist2 = dot(delta, delta);
                if (dist2 < 1.0e-6 || dist2 > (max_r_w * max_r_w * 2.25)) {
                    continue;
                }

                float dist = sqrt(dist2);
                // Reject coplanar & convex self-occlusion below the surface tangent plane
                if (dot(delta, center_vnrm) <= dist * 0.045) {
                    continue;
                }

                float fn = dot(delta, n_dir);
                if (fn <= 1.0e-5) {
                    continue;
                }

                float  falloff    = saturate(1.0 - dist / (max_r_w * 1.35));
                float  eff_thk    = thick_w * lerp(0.55, 1.45, u_step);
                float3 delta_back = delta - view_v * eff_thk;
                float  ft         = dot(delta, t_nrm);
                float  bn         = dot(delta_back, n_dir);
                float  bt         = dot(delta_back, t_nrm);

                // Angles measured directly in (n_dir, t_nrm) stay in [-pi/2, +pi/2] with zero branch-cut wrap
                float ang_f = fast_atan2_gpu(ft, fn);
                float ang_b = (bn > 1.0e-5) ? fast_atan2_gpu(bt, bn)
                                            : ((ft >= 0.0) ? k_gi_half_pi : -k_gi_half_pi);
                float a_min = (min(ang_f, ang_b) + k_gi_half_pi) * (1.0 / k_gi_pi);
                float a_max = (max(ang_f, ang_b) + k_gi_half_pi) * (1.0 / k_gi_pi);
                a_max = lerp(a_min, a_max, smoothstep(0.0, 0.25, falloff));

                uint hit_bits       = ssilvb_sector_mask_gpu(a_min, a_max, jitter.z);
                uint newly_occluded = hit_bits & ~vis_mask;
                if (newly_occluded != 0u) {
                    float  sec_w    = float(countbits(newly_occluded)) * (1.0 / 32.0);
                    float3 dir_in   = delta / dist;
                    float3 nb_vnrm  = lighting_unpack_oct_snorm16_gpu(nb.oct_nrm_s16);
                    float3 nb_rad   = lighting_unpack_rgb9e5_gpu(nb.direct_rgb9e5);
                    float  recv_cos = saturate(dot(center_vnrm, dir_in) * 0.85 + 0.15);
                    float  emit_cos = saturate(dot(nb_vnrm, -dir_in) * 0.80 + 0.20);

                    ind_accum += nb_rad * (sec_w * recv_cos * emit_cos * falloff * proj_len);
                    vis_mask  |= hit_bits;
                }
            }
        }

        uint  occ_bits = countbits(vis_mask);
        float sl_vis   = 1.0 - float(occ_bits) * (1.0 / 32.0);
        uint  unocc_lo = 16u - countbits(vis_mask & 0x0000FFFFu);
        uint  unocc_hi = 16u - countbits((vis_mask >> 16u) & 0x0000FFFFu);
        float tilt     = (float(unocc_hi) - float(unocc_lo)) * (1.0 / 16.0);

        vis_accum  += sl_vis * proj_len;
        vis_weight += proj_len;
        bent_accum += normalize(center_vnrm + t_nrm * (tilt * 0.72)) * proj_len;
    }

    float  inv_w        = (vis_weight > 1.0e-4) ? (1.0 / vis_weight) : (1.0 / float(num_slices));
    float  ao_cur       = (vis_weight > 1.0e-4) ? clamp(vis_accum * inv_w, 0.0, 1.0) : 1.0;
    float3 bent_cur     = dot(bent_accum, bent_accum) > 1.0e-5 ? normalize(bent_accum) : center_vnrm;
    bool   ground_probe = (hdr.has_ground != 0u && hdr.terrain_dir_bda == 0ull &&
                           (ctex.alb_flags >> 24u) == 3u && center_wnrm.z > 0.5);
    float3 local_bounce = ind_accum * inv_w * 1.65;
    float3 far_hrc_irr  = sample_hrc_c0_irradiance(hdr, center_uv, center_wnrm, ao_cur, ground_probe);
    float3 irr_cur      = max(local_bounce + far_hrc_irr, float3(0.0));

    // Camera-motion temporal reprojection & confidence-weighted accumulation
    float conf = 1.0 / 16.0;
    if (hdr.history_valid != 0u && hdr.gi_prev_bda != 0ull) {
        float  prev_vz = 0.0;
        float2 puv     = lighting_world_to_prev_gi_uv(hdr, sb.prev_persp, center_wpos, prev_vz);
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
                float  inv_pw  = 1.0 / pw_sum;
                float  hist_ao = p_ao   * inv_pw;
                float3 hist_bn = normalize(lighting_inv_view_rotate(pq, p_bent * inv_pw));
                float3 hist_bv = normalize(view_rotate(q, hist_bn));
                float3 hist_ir = p_irr  * inv_pw;
                float  hist_cf = p_conf * inv_pw;

                conf        = min(hist_cf + (1.0 / 16.0), 1.0);
                float alpha = lerp(0.36, 0.14, conf);

                float3 irr_max = irr_cur * 2.2 + float3(0.18);
                hist_ir        = clamp(hist_ir, float3(0.0), irr_max);

                ao_cur   = lerp(hist_ao, ao_cur, alpha);
                bent_cur = normalize(lerp(hist_bv, bent_cur, alpha));
                irr_cur  = lerp(hist_ir, irr_cur, alpha);
            }
        }
    }

    uint nrm_u16  = lighting_pack_oct_snorm8_gpu(center_vnrm);
    uint bent_u16 = lighting_pack_oct_snorm8_gpu(bent_cur);
    uint ao_u16   = uint(clamp(round(ao_cur * 65535.0), 0.0, 65535.0));
    uint conf_u16 = uint(clamp(round(conf   * 65535.0), 0.0, 65535.0));

    LightingGiTexel out_gi;
    out_gi.view_z          = center_vz;
    out_gi.packed_nrm_bent = nrm_u16 | (bent_u16 << 16u);
    out_gi.irr_rgb9e5      = lighting_pack_rgb9e5_gpu(irr_cur);
    out_gi.ao_conf_u16     = ao_u16 | (conf_u16 << 16u);
    curr_grid[out_linear]  = out_gi;
}
