// Joint 2x2 Bilateral SSILVB + Split HRC GI Upsampler & Hardware PCF Shadow Samplers.

bool lighting_sample_gi(
    uint64_t   env_bda,
    float2     sv_pos_xy,
    float      view_z,
    float3     view_nrm,
    out float  out_ao,
    out float3 out_bent_nrm,
    out float3 out_indirect_rgb)
{
    out_ao           = 1.0;
    out_bent_nrm     = view_nrm;
    out_indirect_rgb = float3(0.0);

    if (env_bda == 0ull) {
        return false;
    }
    LightingFrameHeader *hdr = (LightingFrameHeader *)(env_bda + k_lighting_header_offset);
    if (hdr->magic != k_lighting_magic || hdr->gi_enabled == 0u) {
        return false;
    }
    uint64_t gi_bda = ((hdr->scene_flags & k_lighting_flag_filt_in_gbuf) != 0u)
                    ? hdr->gbuf_bda
                    : hdr->gi_curr_bda;
    uint gi_w = hdr->gi_width;
    uint gi_h = hdr->gi_height;
    if (gi_bda == 0ull || gi_w == 0u || gi_h == 0u) {
        return false;
    }

    float full_w      = max(float(hdr->full_width),  1.0);
    float full_h      = max(float(hdr->full_height), 1.0);
    float zoom        = hdr->zoom;
    float ao_strength = hdr->ao_strength;
    float gi_intens   = hdr->gi_intensity;

    float2 uv   = sv_pos_xy / float2(full_w, full_h);
    float2 tc   = clamp(uv, float2(0.0), float2(1.0)) * float2(float(gi_w), float(gi_h)) - float2(0.5);
    int2   base = int2(floor(tc));
    float2 f    = tc - float2(base);

    int2 max_c = int2(int(gi_w) - 1, int(gi_h) - 1);
    LightingGiTexel *grid = (LightingGiTexel *)gi_bda;

    float  inv_sigma_z = max(zoom * 0.085, 0.004);
    float  w_sum       = 0.0;
    float  ao_sum      = 0.0;
    float3 bent_sum    = float3(0.0);
    float3 irr_sum     = float3(0.0);

    float  fb_w_sum    = 0.0;
    float  fb_ao_sum   = 0.0;
    float3 fb_bent_sum = float3(0.0);
    float3 fb_irr_sum  = float3(0.0);

    [unroll]
    for (int dy = 0; dy <= 1; ++dy) {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx) {
            int2 p = clamp(base + int2(dx, dy), int2(0), max_c);
            LightingGiTexel tex = grid[uint(p.y) * gi_w + uint(p.x)];
            if (tex.view_z <= -1.0e19) {
                continue;
            }
            float  wb    = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y);
            float3 tnrm  = lighting_unpack_oct_snorm8_gpu(tex.packed_nrm_bent & 0xFFFFu);
            float3 tbent = lighting_unpack_oct_snorm8_gpu((tex.packed_nrm_bent >> 16u) & 0xFFFFu);
            float3 tirr  = lighting_unpack_rgb9e5_gpu(tex.irr_rgb9e5);
            float  tao   = float(tex.ao_conf_u16 & 0xFFFFu) * (1.0 / 65535.0);

            float dz = abs(tex.view_z - view_z) * inv_sigma_z;
            float wz = rcp(1.0 + dz + 0.48 * dz * dz);
            float nd = max(dot(tnrm, view_nrm), 0.0);
            float wn = nd * nd;
            wn       = wn * wn;

            float w = max(wb, 0.02) * wz * max(wn, 0.002);
            w_sum    += w;
            ao_sum   += tao * w;
            bent_sum += tbent * w;
            irr_sum  += tirr * w;

            float fbw = max(wb, 0.05);
            fb_w_sum    += fbw;
            fb_ao_sum   += tao * fbw;
            fb_bent_sum += tbent * fbw;
            fb_irr_sum  += tirr * fbw;
        }
    }

    if (w_sum > 1.0e-5) {
        float inv_w      = 1.0 / w_sum;
        out_ao           = clamp(lerp(1.0, ao_sum * inv_w, ao_strength), 0.0, 1.0);
        float3 b_dir     = bent_sum * inv_w;
        out_bent_nrm     = dot(b_dir, b_dir) > 1.0e-6 ? normalize(b_dir) : view_nrm;
        out_indirect_rgb = (irr_sum * inv_w) * gi_intens;
        return true;
    }
    if (fb_w_sum > 1.0e-5) {
        float inv_fb     = 1.0 / fb_w_sum;
        out_ao           = clamp(lerp(1.0, fb_ao_sum * inv_fb, ao_strength * 0.65), 0.0, 1.0);
        float3 b_dir     = fb_bent_sum * inv_fb;
        out_bent_nrm     = dot(b_dir, b_dir) > 1.0e-6 ? normalize(b_dir) : view_nrm;
        out_indirect_rgb = (fb_irr_sum * inv_fb) * (gi_intens * 0.75);
        return true;
    }
    return false;
}

// Fast 1-tap hardware PCF lookup for GI compute pass
float lighting_sample_shadow_1tap(
    LightingShadowBlock sb,
    Sampler2DShadow     shadow_map,
    float3              world_pos,
    float3              world_nrm)
{
    if (sb.shadow_enabled == 0u) {
        return 1.0;
    }
    float ndl = dot(world_nrm, sb.sun_dir);
    if (ndl <= 0.002) {
        return 0.0;
    }
    float3 biased = world_pos + world_nrm * (sb.normal_bias * (1.25 - 0.55 * ndl));
    float4 wp     = float4(biased, 1.0);
    float3 suv    = float3(dot(sb.light_vp0, wp), dot(sb.light_vp1, wp), dot(sb.light_vp2, wp));
    if (suv.x <= 0.002 || suv.x >= 0.998 || suv.y <= 0.002 || suv.y >= 0.998 ||
        suv.z <= 0.0001 || suv.z >= 0.9999) {
        return 1.0;
    }
    return shadow_map.SampleCmpLevelZero(suv.xy, suv.z);
}

// 9-tap Interleaved-Gradient-Noise rotated Vogel-disk Hardware PCF soft shadow sampler
float lighting_sample_shadow(
    LightingShadowBlock sb,
    Sampler2DShadow     shadow_map,
    float3              world_pos,
    float3              world_nrm,
    float2              sv_pos_xy)
{
    if (sb.shadow_enabled == 0u) {
        return 1.0;
    }
    float ndl = dot(world_nrm, sb.sun_dir);
    if (ndl <= 0.001) {
        return 0.0;
    }
    float3 biased = world_pos + world_nrm * (sb.normal_bias * (1.30 - 0.60 * ndl));
    float4 wp     = float4(biased, 1.0);
    float3 suv    = float3(dot(sb.light_vp0, wp), dot(sb.light_vp1, wp), dot(sb.light_vp2, wp));

    float edge_d = min(min(suv.x, 1.0 - suv.x), min(suv.y, 1.0 - suv.y));
    if (edge_d <= 0.0 || suv.z <= 0.0001 || suv.z >= 0.9999) {
        return 1.0;
    }

    float ign      = frac(52.9829189 * frac(dot(sv_pos_xy, float2(0.06711056, 0.00583715))));
    float phi0     = ign * 6.28318530718;
    float filter_r = sb.inv_shadow_res * 2.25;
    float vis      = 0.0;

    [unroll]
    for (int i = 0; i < 9; ++i) {
        float  r     = sqrt((float(i) + 0.5) * (1.0 / 9.0)) * filter_r;
        float  theta = float(i) * 2.3999632297 + phi0;
        float2 off   = float2(cos(theta), sin(theta)) * r;
        vis += shadow_map.SampleCmpLevelZero(suv.xy + off, suv.z);
    }
    vis *= (1.0 / 9.0);
    float border_fade = smoothstep(0.0, 0.04, edge_d);
    return lerp(1.0, vis, border_fade);
}

// Reconstruct world position and evaluate hardware sun shadow visibility in one shared helper
void lighting_eval_world_shadow(
    float3          view_nrm,
    float2          sv_pos_xy,
    float           view_z,
    uint64_t        env_bda,
    Sampler2DShadow shadow_map,
    out float3      out_wpos,
    out bool        out_have_wp,
    out float       out_sun_vis)
{
    out_wpos    = float3(0.0);
    out_have_wp = false;
    out_sun_vis = 1.0;
    if (env_bda == 0ull) {
        return;
    }
    LightingFrameHeader hdr = ((LightingFrameHeader *)(env_bda + k_lighting_header_offset))[0];
    if (hdr.magic != k_lighting_magic) {
        return;
    }
    LightingShadowBlock sb = ((LightingShadowBlock *)(env_bda + k_lighting_shadow_offset))[0];
    out_wpos    = lighting_sv_to_world_persp(hdr, sb.persp, sv_pos_xy, view_z);
    out_have_wp = true;
    if (sb.shadow_enabled != 0u) {
        float4 q      = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
        float3 eval_n = normalize(lighting_q_inv_rotate(q, view_nrm));
        out_sun_vis   = lighting_sample_shadow(sb, shadow_map, out_wpos, eval_n, sv_pos_xy);
    }
}
