// Slang 2026 GI Primary Surface & Direct-Radiance G-Buffer Compute Pass
// Uses 8x8 tile-frustum SLM analytical proxy culling + 1-tap hardware shadow map
// + perspective/orthographic primary rays + 1-tap scalar DEM heightfield solver.

#include "lighting_gi_common.slang"

[[vk::binding(1, 0)]]
Sampler2DShadow g_shadow_map;

[[vk::push_constant]] LightingComputePush pc;

groupshared LightingFrameHeader s_hdr;
groupshared LightingShadowBlock s_sb;
groupshared uint                s_tile_proxy_cnt;
groupshared LightingProxyGpu    s_tile_proxies[32];

bool ray_terrain_slab(uint64_t terrain_dir_bda,
                      float    elev_base_m,
                      float    vertical_scale,
                      float3   ro,
                      float3   rd,
                      float    max_t,
                      out float out_t0,
                      out float out_t1) {
    float  half_ext = dir_world_half_extent(terrain_dir_bda);
    float2 elev_rng = dir_elev_range_m(terrain_dir_bda);
    float  zmin     = (elev_rng.x - elev_base_m - 64.0) * vertical_scale;
    float  zmax     = (elev_rng.y - elev_base_m + 64.0) * vertical_scale;
    if (!dir_is_v2(terrain_dir_bda)) {
        zmin = min(zmin, -140.0);
        zmax = max(zmax, 1400.0);
    }
    const float3 bmin = float3(-half_ext, -half_ext, zmin);
    const float3 bmax = float3( half_ext,  half_ext, zmax);
    float t0 = 0.01;
    float t1 = max_t;

    [unroll]
    for (int a = 0; a < 3; ++a) {
        if (abs(rd[a]) < 1.0e-7) {
            if (ro[a] < bmin[a] || ro[a] > bmax[a]) {
                out_t0 = 0.0;
                out_t1 = 0.0;
                return false;
            }
        } else {
            float inv_d = 1.0 / rd[a];
            float s0    = (bmin[a] - ro[a]) * inv_d;
            float s1    = (bmax[a] - ro[a]) * inv_d;
            if (s0 > s1) {
                float tmp = s0; s0 = s1; s1 = tmp;
            }
            t0 = max(t0, s0);
            t1 = min(t1, s1);
            if (t0 > t1) {
                out_t0 = 0.0;
                out_t1 = 0.0;
                return false;
            }
        }
    }
    out_t0 = t0;
    out_t1 = t1;
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    if (pc.env_bda == 0ull) {
        return;
    }

    uint lidx = gtid.y * 8u + gtid.x;
    if (lidx == 0u) {
        s_hdr            = ((LightingFrameHeader *)(pc.env_bda + k_lighting_header_offset))[0];
        s_sb             = ((LightingShadowBlock *)(pc.env_bda + k_lighting_shadow_offset))[0];
        s_tile_proxy_cnt = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    LightingFrameHeader hdr = s_hdr;
    LightingShadowBlock sb  = s_sb;
    if (hdr.magic != k_lighting_magic || hdr.gbuf_bda == 0ull ||
        hdr.gi_width == 0u || hdr.gi_height == 0u) {
        return;
    }

    float4 q     = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 cam   = float3(hdr.cam_x, hdr.cam_y, hdr.cam_z);
    uint   p_cnt = (hdr.proxies_bda != 0ull) ? min(hdr.proxy_count, 128u) : 0u;

    // Cooperative 8x8 tile-frustum sphere culling into zero-latency on-chip SLM
    if (p_cnt > 0u) {
        float2 scale_px = float2(float(hdr.full_width), float(hdr.full_height)) /
                          float2(float(hdr.gi_width),   float(hdr.gi_height));
        float2 fpx0 = float2(gid.xy * 8u) * scale_px;
        float2 fpx1 = float2(min(gid.xy * 8u + uint2(8u, 8u), uint2(hdr.gi_width, hdr.gi_height))) * scale_px;
        float2 va   = lighting_pixel_to_view_xy(hdr, fpx0);
        float2 vb   = lighting_pixel_to_view_xy(hdr, fpx1);
        float2 vmin = min(va, vb);
        float2 vmax = max(va, vb);

        LightingProxyGpu *proxies = (LightingProxyGpu *)hdr.proxies_bda;
        for (uint i = lidx; i < p_cnt; i += 64u) {
            LightingProxyGpu pr = proxies[i];
            float  r  = proxy_bound_radius(pr);
            float3 vc = view_of(pr.center, q, cam);
            float  w_c = (sb.persp > 1.0e-7) ? max(1.0 - vc.z * sb.persp, 0.08) : 1.0;
            float2 pc_proj = vc.xy / w_c;
            float  r_proj  = r / max(w_c - r * sb.persp, 0.08);
            if (pc_proj.x + r_proj >= vmin.x && pc_proj.x - r_proj <= vmax.x &&
                pc_proj.y + r_proj >= vmin.y && pc_proj.y - r_proj <= vmax.y) {
                uint slot = 0u;
                InterlockedAdd(s_tile_proxy_cnt, 1u, slot);
                if (slot < 32u) {
                    s_tile_proxies[slot] = pr;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid.x >= hdr.gi_width || tid.y >= hdr.gi_height) {
        return;
    }

    float2 uv      = (float2(tid.xy) + float2(0.5)) / float2(float(hdr.gi_width), float(hdr.gi_height));
    float2 full_px = uv * float2(float(hdr.full_width), float(hdr.full_height));

    float3 ro, rd;
    float  best_t;
    lighting_primary_ray(hdr, sb.persp, full_px, ro, rd, best_t);

    bool   hit_any         = false;
    bool   best_is_terrain = false;
    float3 best_wnrm       = float3(0.0, 0.0, 1.0);
    float3 best_alb        = float3(0.0);
    float3 best_emi        = float3(0.0);
    uint   best_flags      = 0u;

    // 1. Evaluate tile-culled SLM analytical proxies (cubes, solids, custom meshes — zero triangle loops!)
    uint tile_cnt = s_tile_proxy_cnt;
    if (tile_cnt > 0u && tile_cnt <= 32u) {
        for (uint i = 0u; i < tile_cnt; ++i) {
            LightingProxyGpu pr = s_tile_proxies[i];
            float  t_hit = 0.0;
            float3 wnrm  = float3(0.0);
            if (lighting_ray_proxy_gpu(ro, rd, 0.05, best_t, pr, t_hit, wnrm)) {
                best_t          = t_hit;
                best_wnrm       = wnrm;
                best_alb        = proxy_albedo(pr);
                best_emi        = proxy_emissive(pr);
                best_is_terrain = false;
                best_flags      = 1u;
                hit_any         = true;
            }
        }
    } else if (tile_cnt > 32u) {
        LightingProxyGpu *proxies = (LightingProxyGpu *)hdr.proxies_bda;
        for (uint i = 0u; i < p_cnt; ++i) {
            LightingProxyGpu pr = proxies[i];
            float  t_hit = 0.0;
            float3 wnrm  = float3(0.0);
            if (lighting_ray_proxy_gpu(ro, rd, 0.05, best_t, pr, t_hit, wnrm)) {
                best_t          = t_hit;
                best_wnrm       = wnrm;
                best_alb        = proxy_albedo(pr);
                best_emi        = proxy_emissive(pr);
                best_is_terrain = false;
                best_flags      = 1u;
                hit_any         = true;
            }
        }
    }

    // 2. SSD-streamed .epack DEM heightfield (1-tap scalar search + 1 full hit decode)
    if (hdr.terrain_dir_bda != 0ull) {
        float t0 = 0.0, t1 = 0.0;
        if (ray_terrain_slab(hdr.terrain_dir_bda, hdr.elev_base_m, hdr.vertical_scale,
                             ro, rd, best_t, t0, t1)) {
            uint  steps     = hdr.oracle_mode != 0u ? 12u : 7u;
            float dt        = (t1 - t0) / float(steps);
            float t_prev    = t0;
            bool  bracketed = false;
            float t_hi      = t1;

            for (uint s = 0u; s <= steps; ++s) {
                float  tc = t0 + dt * float(s);
                float3 wp = ro + rd * tc;
                float  dz = 0.0;
                if (lighting_sample_terrain_height_fast(
                        hdr.terrain_dir_bda, hdr.elev_base_m, hdr.vertical_scale, wp.xy, 3, dz)) {
                    if (wp.z <= dz) {
                        t_hi      = tc;
                        bracketed = true;
                        break;
                    }
                    t_prev = tc;
                }
            }

            if (bracketed) {
                float ta = t_prev;
                float tb = t_hi;
                [unroll]
                for (int r = 0; r < 3; ++r) {
                    float  tm = 0.5 * (ta + tb);
                    float3 wp = ro + rd * tm;
                    float  dz = 0.0;
                    if (lighting_sample_terrain_height_fast(
                            hdr.terrain_dir_bda, hdr.elev_base_m, hdr.vertical_scale, wp.xy, 4, dz) &&
                        wp.z <= dz) {
                        tb = tm;
                    } else {
                        ta = tm;
                    }
                }
                float  t_final = 0.5 * (ta + tb);
                float3 wp_hit  = ro + rd * t_final;
                float  dz_hit  = 0.0;
                float3 dn_hit  = float3(0.0, 0.0, 1.0);
                float3 da_hit  = float3(0.35, 0.38, 0.30);
                if (t_final < best_t &&
                    lighting_sample_terrain_dem(hdr, wp_hit.xy, 4, dz_hit, dn_hit, da_hit)) {
                    best_t          = t_final;
                    best_wnrm       = dn_hit;
                    best_alb        = da_hit;
                    best_emi        = float3(0.0);
                    best_is_terrain = true;
                    best_flags      = 2u;
                    hit_any         = true;
                }
            }
        }
    }

    // 3. True 3D World-Space Ground Plane z = hdr.ground_z
    if (hdr.has_ground != 0u && rd.z < -0.004) {
        float tg = (hdr.ground_z - ro.z) / rd.z;
        if (tg > 0.05 && tg < best_t - 0.05) {
            float3 gwp = ro + rd * tg;
            float  gd  = length(gwp.xy - cam.xy);
            if (gd < 8500.0) {
                best_t          = tg;
                best_wnrm       = float3(0.0, 0.0, 1.0);
                best_alb        = float3(hdr.ground_r, hdr.ground_g, hdr.ground_b);
                best_emi        = float3(0.0);
                best_is_terrain = false;
                best_flags      = 3u;
                hit_any         = true;
            }
        }
    }

    LightingGbufTexelGpu out_tex;
    if (hit_any) {
        float3 world_pos = ro + rd * best_t;
        float3 vpos      = view_of(world_pos, q, cam);
        float3 view_nrm  = normalize(view_rotate(q, best_wnrm));
        float  sun_vis   = best_is_terrain
                         ? 1.0
                         : lighting_sample_shadow_1tap(sb, g_shadow_map, world_pos, best_wnrm);
        float3 rad       = lighting_eval_direct_radiance_fast(
            hdr, sb, world_pos, best_wnrm, best_alb, best_emi, sun_vis, best_is_terrain);

        out_tex.view_z        = vpos.z;
        out_tex.oct_nrm_s16   = lighting_pack_oct_snorm16_gpu(view_nrm);
        out_tex.direct_rgb9e5 = lighting_pack_rgb9e5_gpu(rad);
        out_tex.alb_flags     = lighting_pack_rgba8_gpu(best_alb, best_flags);
    } else {
        out_tex.view_z        = -1.0e20;
        out_tex.oct_nrm_s16   = 0u;
        out_tex.direct_rgb9e5 = 0u;
        out_tex.alb_flags     = 0u;
    }

    ((LightingGbufTexelGpu *)hdr.gbuf_bda)[tid.y * hdr.gi_width + tid.x] = out_tex;
}
