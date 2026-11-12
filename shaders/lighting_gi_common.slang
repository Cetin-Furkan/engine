// Shared Slang 2026 Compute Helpers for SSILVB + Split Radiance Cascades (HRC)
// Matches include/lighting/lighting.h byte-for-byte.

#include "camera.slang"
import terrain_math;
#include "lighting.slang"

static const uint k_proxy_obb       = 0u;
static const uint k_proxy_ellipsoid = 1u;
static const float k_gi_pi          = 3.14159265359;
static const float k_gi_half_pi     = 1.57079632679;
static const float k_gi_ray_near    = 65536.0;

struct LightingProxyGpu {
    float3 center;
    uint   kind;
    float3 extents;
    float  roughness;
    float  pitch_rad;
    float  yaw_rad;
    uint   alb_rg;
    uint   alb_b_flags;
    uint   emi_rg;
    uint   emi_b_rad;
    uint   trig_pitch;
    uint   trig_yaw;
};

struct LightingGbufTexelGpu {
    float view_z;
    uint  oct_nrm_s16;
    uint  direct_rgb9e5;
    uint  alb_flags;
};

struct LightingHrcRayGpu {
    uint  rad_rgb9e5;
    float beta;
};

struct LightingComputePush {
    uint64_t env_bda;
    uint     cascade_idx;
    uint     flags;
};

float3 proxy_albedo(LightingProxyGpu p) {
    return float3(
        f16tof32(p.alb_rg & 0xFFFFu),
        f16tof32((p.alb_rg >> 16u) & 0xFFFFu),
        f16tof32(p.alb_b_flags & 0xFFFFu)
    );
}

float3 proxy_emissive(LightingProxyGpu p) {
    return float3(
        f16tof32(p.emi_rg & 0xFFFFu),
        f16tof32((p.emi_rg >> 16u) & 0xFFFFu),
        f16tof32(p.emi_b_rad & 0xFFFFu)
    );
}

float proxy_bound_radius(LightingProxyGpu p) {
    float r = f16tof32((p.emi_b_rad >> 16u) & 0xFFFFu);
    return r > 1.0e-4 ? r : (length(p.extents) * 1.02);
}

uint lighting_pack_rgb9e5_gpu(float3 rgb) {
    float3 c = clamp(rgb, float3(0.0), float3(65408.0));
    float max_c = max(c.r, max(c.g, c.b));
    if (max_c < 1.52587890625e-5) {
        return 0u;
    }
    int exp_unbiased = int(floor(log2(max_c))) + 1;
    int exp_shared   = clamp(exp_unbiased + 15, 0, 31);
    float denom      = exp2(float(exp_shared - 15 - 9));
    uint max_m       = uint(floor(max_c / denom + 0.5));
    if (max_m >= 512u && exp_shared < 31) {
        exp_shared += 1;
        denom *= 2.0;
    }
    uint rm = uint(clamp(floor(c.r / denom + 0.5), 0.0, 511.0));
    uint gm = uint(clamp(floor(c.g / denom + 0.5), 0.0, 511.0));
    uint bm = uint(clamp(floor(c.b / denom + 0.5), 0.0, 511.0));
    return rm | (gm << 9u) | (bm << 18u) | (uint(exp_shared) << 27u);
}

uint lighting_pack_rgba8_gpu(float3 rgb, uint flags8) {
    uint r = uint(clamp(floor(rgb.r * 255.0 + 0.5), 0.0, 255.0));
    uint g = uint(clamp(floor(rgb.g * 255.0 + 0.5), 0.0, 255.0));
    uint b = uint(clamp(floor(rgb.b * 255.0 + 0.5), 0.0, 255.0));
    return r | (g << 8u) | (b << 16u) | ((flags8 & 0xFFu) << 24u);
}

float3 lighting_unpack_rgb8_gpu(uint packed) {
    return float3(
        float(packed & 0xFFu),
        float((packed >> 8u) & 0xFFu),
        float((packed >> 16u) & 0xFFu)
    ) * (1.0 / 255.0);
}

float2 lighting_oct_encode_unit(float3 n) {
    float len = length(n);
    if (len < 1.0e-8) {
        return float2(0.0);
    }
    n /= len;
    float inv_l1 = 1.0 / (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy * inv_l1;
    if (n.z < 0.0) {
        float2 s = float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
        e = (float2(1.0) - abs(e.yx)) * s;
    }
    return clamp(e, float2(-1.0), float2(1.0));
}

uint lighting_pack_oct_snorm16_gpu(float3 n) {
    float2 e = lighting_oct_encode_unit(n);
    int qx = int(round(e.x * 32767.0));
    int qy = int(round(e.y * 32767.0));
    return (uint(qx) & 0xFFFFu) | ((uint(qy) & 0xFFFFu) << 16u);
}

float3 lighting_unpack_oct_snorm16_gpu(uint packed) {
    int qx = (int(packed << 16u)) >> 16;
    int qy = int(packed) >> 16;
    float2 e = clamp(float2(float(qx), float(qy)) * (1.0 / 32767.0), float2(-1.0), float2(1.0));
    float3 v = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        float2 s = float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
        v.xy = (float2(1.0) - abs(v.yx)) * s;
    }
    return normalize(v);
}

uint lighting_pack_oct_snorm8_gpu(float3 n) {
    float2 e = lighting_oct_encode_unit(n);
    int qx = int(round(e.x * 127.0));
    int qy = int(round(e.y * 127.0));
    return (uint(qx) & 0xFFu) | ((uint(qy) & 0xFFu) << 8u);
}

float3 lighting_inv_view_rotate(float4 q, float3 v) {
    return view_rotate(float4(-q.xyz, q.w), v);
}

float2 lighting_pixel_to_view_xy(LightingFrameHeader hdr, float2 full_px) {
    float z = max(hdr.zoom, 1.0e-5);
    return float2(
        (full_px.x - float(hdr.full_width) * 0.5) / z,
        (float(hdr.full_height) * 0.5 - full_px.y) / z
    );
}

float3 lighting_view_to_world(LightingFrameHeader hdr, float3 vpos) {
    float4 q = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 c = float3(hdr.cam_x, hdr.cam_y, hdr.cam_z);
    return c + lighting_inv_view_rotate(q, vpos);
}

float2 lighting_world_to_prev_gi_uv(LightingFrameHeader hdr, float3 world_pos, out float out_prev_vz) {
    float4 pq = float4(hdr.prev_qx, hdr.prev_qy, hdr.prev_qz, hdr.prev_qw);
    float3 pc = float3(hdr.prev_cam_x, hdr.prev_cam_y, hdr.prev_cam_z);
    float3 pv = view_of(world_pos, pq, pc);
    out_prev_vz = pv.z;
    float pz = max(hdr.prev_zoom, 1.0e-5);
    float px = float(hdr.full_width) * 0.5 + pv.x * pz;
    float py = float(hdr.full_height) * 0.5 - pv.y * pz;
    return float2(px / max(float(hdr.full_width), 1.0), py / max(float(hdr.full_height), 1.0));
}

float3 lighting_pose_to_local_fast(float3 v, float sx, float cx, float sy, float cy) {
    float3 ry = float3(v.x * cy - v.z * sy, v.y, v.x * sy + v.z * cy);
    return float3(ry.x, ry.y * cx + ry.z * sx, -ry.y * sx + ry.z * cx);
}

float3 lighting_local_to_pose_fast(float3 v, float sx, float cx, float sy, float cy) {
    float3 rx = float3(v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx);
    return float3(rx.x * cy + rx.z * sy, rx.y, -rx.x * sy + rx.z * cy);
}

bool lighting_ray_proxy_gpu(
    float3           ro,
    float3           rd,
    float            t_min,
    float            t_max,
    LightingProxyGpu proxy,
    out float        out_t,
    out float3       out_world_nrm)
{
    out_t         = t_max;
    out_world_nrm = float3(0.0, 0.0, 1.0);

    float3 rel = ro - proxy.center;
    // Tight exact circumsphere early reject (zero SFU trig instructions!)
    float r_sph = proxy_bound_radius(proxy);
    float b_sph = dot(rel, rd);
    float c_sph = dot(rel, rel) - r_sph * r_sph;
    if (c_sph > 0.0 && b_sph > 0.0) {
        return false;
    }
    float disc_sph = b_sph * b_sph - c_sph;
    if (disc_sph < 0.0) {
        return false;
    }
    // Interval reject: sphere span [-b_sph - sqrt(disc), -b_sph + sqrt(disc)] must overlap [t_min, t_max]
    if (-b_sph > t_max + r_sph || -b_sph < t_min - r_sph) {
        return false;
    }

    float sx = f16tof32(proxy.trig_pitch & 0xFFFFu);
    float cx = f16tof32((proxy.trig_pitch >> 16u) & 0xFFFFu);
    float sy = f16tof32(proxy.trig_yaw   & 0xFFFFu);
    float cy = f16tof32((proxy.trig_yaw   >> 16u) & 0xFFFFu);

    float3 lo = lighting_pose_to_local_fast(rel, sx, cx, sy, cy);
    float3 ld = lighting_pose_to_local_fast(rd,  sx, cx, sy, cy);

    if (proxy.kind == k_proxy_obb) {
        float t0 = t_min;
        float t1 = t_max;
        int   hit_axis = -1;
        float hit_sign = 1.0;

        [unroll]
        for (int a = 0; a < 3; ++a) {
            float ext = max(proxy.extents[a], 1.0e-4);
            if (abs(ld[a]) < 1.0e-7) {
                if (lo[a] < -ext || lo[a] > ext) {
                    return false;
                }
            } else {
                float inv_d = 1.0 / ld[a];
                float s0    = (-ext - lo[a]) * inv_d;
                float s1    = ( ext - lo[a]) * inv_d;
                float sgn   = -1.0;
                if (s0 > s1) {
                    float tmp = s0; s0 = s1; s1 = tmp;
                    sgn = 1.0;
                }
                if (s0 > t0) {
                    t0       = s0;
                    hit_axis = a;
                    hit_sign = sgn;
                }
                t1 = min(t1, s1);
                if (t0 > t1) {
                    return false;
                }
            }
        }
        if (t0 < t_min || t0 > t_max) {
            return false;
        }
        float3 ln = float3(0.0);
        if (hit_axis == 0)      ln = float3(hit_sign, 0.0, 0.0);
        else if (hit_axis == 1) ln = float3(0.0, hit_sign, 0.0);
        else if (hit_axis == 2) ln = float3(0.0, 0.0, hit_sign);
        else {
            float3 hp  = lo + ld * t0;
            float3 rem = abs(proxy.extents - abs(hp));
            if (rem.x <= rem.y && rem.x <= rem.z)      ln = float3(hp.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
            else if (rem.y <= rem.z)                   ln = float3(0.0, hp.y >= 0.0 ? 1.0 : -1.0, 0.0);
            else                                       ln = float3(0.0, 0.0, hp.z >= 0.0 ? 1.0 : -1.0);
        }
        // Subtle bevel-edge normal softening near cube edges to match beveled cube shading
        float3 hp_b = clamp((lo + ld * t0) / max(proxy.extents, float3(1.0e-4)), float3(-1.0), float3(1.0));
        float3 edge = smoothstep(float3(0.84), float3(1.0), abs(hp_b)) * sign(hp_b);
        ln = normalize(lerp(ln, normalize(ln + edge * 0.65), 0.35));

        out_t         = t0;
        out_world_nrm = normalize(lighting_local_to_pose_fast(ln, sx, cx, sy, cy));
        return true;
    }

    float3 inv_r = 1.0 / max(proxy.extents, float3(1.0e-4));
    float3 so    = lo * inv_r;
    float3 sd    = ld * inv_r;
    float  qa    = dot(sd, sd);
    if (qa < 1.0e-10) {
        return false;
    }
    float qb   = dot(so, sd);
    float qc   = dot(so, so) - 1.0;
    float disc = qb * qb - qa * qc;
    if (disc < 0.0) {
        return false;
    }
    float root  = sqrt(disc);
    float t_hit = (-qb - root) / qa;
    if (t_hit < t_min || t_hit > t_max) {
        t_hit = (-qb + root) / qa;
        if (t_hit < t_min || t_hit > t_max) {
            return false;
        }
    }
    float3 hp = lo + ld * t_hit;
    float3 ln = normalize(hp * inv_r * inv_r);
    out_t         = t_hit;
    out_world_nrm = normalize(lighting_local_to_pose_fast(ln, sx, cx, sy, cy));
    return true;
}

// Ultra-fast 1-tap scalar DEM height query for ray-interval bracketing (zero BC1 / gradient decode!)
bool lighting_sample_terrain_height_fast(
    uint64_t  terrain_dir_bda,
    float     elev_base_m,
    float     vertical_scale,
    float2    world_xy,
    int       max_lod,
    out float out_world_z)
{
    out_world_z = 0.0;
    if (terrain_dir_bda == 0ull || abs(world_xy.x) > 12000.0 || abs(world_xy.y) > 12000.0) {
        return false;
    }

    float u_map    = clamp((world_xy.x + 12000.0) * (1.0 / 24000.0), 0.0, 0.999999);
    float v_map    = clamp((world_xy.y + 12000.0) * (1.0 / 24000.0), 0.0, 0.999999);
    float fy_north = 1.0 - v_map;

    static const uint k_lvl_base[5] = { 0u, 1u, 5u, 21u, 85u };
    int start_lod = clamp(max_lod, 0, 4);
    for (int lod = start_lod; lod >= 0; --lod) {
        uint side = 1u << uint(lod);
        uint gx   = min(uint(u_map * float(side)), side - 1u);
        uint gy   = min(uint(fy_north * float(side)), side - 1u);
        uint nidx = k_lvl_base[lod] + gy * side + gx;
        GpuMathQuadNodeGpu node = load_quad_node(terrain_dir_bda, nidx);
        uint resident = (node.packed_ids2 >> 8u) & 0xFFu;
        if (resident != 0u && node.rel_gpu_payload != 0ull) {
            uint64_t tile_bda = relptr2d_resolve_gpu(terrain_dir_bda, node.rel_gpu_payload);
            float span_x = max(node.world_max_x - node.world_min_x, 1.0);
            float span_y = max(node.world_max_y - node.world_min_y, 1.0);
            float ux = clamp((world_xy.x - node.world_min_x) / span_x, 0.0, 1.0);
            float uy = clamp((world_xy.y - node.world_min_y) / span_y, 0.0, 1.0);
            uint  sx = min(uint(ux * 255.0 + 0.5), 255u);
            uint  sy = min(uint(uy * 255.0 + 0.5), 255u);
            float elev_m = tile_load_height_m(tile_bda, sx, sy);
            out_world_z  = (elev_m - elev_base_m) * vertical_scale;
            return true;
        }
    }
    return false;
}

// Full .epack quadtree DEM normal + BC1 satellite albedo lookup (called ONCE on final hit)
bool lighting_sample_terrain_dem(
    LightingFrameHeader hdr,
    float2              world_xy,
    int                 max_lod,
    out float           out_world_z,
    out float3          out_world_nrm,
    out float3          out_albedo)
{
    out_world_z   = 0.0;
    out_world_nrm = float3(0.0, 0.0, 1.0);
    out_albedo    = float3(0.35, 0.38, 0.30);

    if (hdr.terrain_dir_bda == 0ull || abs(world_xy.x) > 12000.0 || abs(world_xy.y) > 12000.0) {
        return false;
    }

    float u_map    = clamp((world_xy.x + 12000.0) * (1.0 / 24000.0), 0.0, 0.999999);
    float v_map    = clamp((world_xy.y + 12000.0) * (1.0 / 24000.0), 0.0, 0.999999);
    float fy_north = 1.0 - v_map;

    static const uint k_lvl_base[5] = { 0u, 1u, 5u, 21u, 85u };
    int start_lod = clamp(max_lod, 0, 4);
    for (int lod = start_lod; lod >= 0; --lod) {
        uint side = 1u << uint(lod);
        uint gx   = min(uint(u_map * float(side)), side - 1u);
        uint gy   = min(uint(fy_north * float(side)), side - 1u);
        uint nidx = k_lvl_base[lod] + gy * side + gx;
        GpuMathQuadNodeGpu node = load_quad_node(hdr.terrain_dir_bda, nidx);
        uint resident = (node.packed_ids2 >> 8u) & 0xFFu;
        if (resident != 0u && node.rel_gpu_payload != 0ull) {
            uint64_t tile_bda = relptr2d_resolve_gpu(hdr.terrain_dir_bda, node.rel_gpu_payload);
            float span_x = max(node.world_max_x - node.world_min_x, 1.0);
            float span_y = max(node.world_max_y - node.world_min_y, 1.0);
            float2 uv = clamp(
                float2((world_xy.x - node.world_min_x) / span_x,
                       (world_xy.y - node.world_min_y) / span_y),
                float2(0.0), float2(1.0));
            float2 dh_duv = float2(0.0);
            float  elev_m = tile_sample_bilinear_height_grad_m(tile_bda, uv, dh_duv);
            float4 img    = tile_sample_bilinear_imagery(tile_bda, uv);
            float  dzdx   = (dh_duv.x / span_x) * hdr.vertical_scale;
            float  dzdy   = (dh_duv.y / span_y) * hdr.vertical_scale;
            out_world_z   = (elev_m - hdr.elev_base_m) * hdr.vertical_scale;
            out_world_nrm = normalize(float3(-dzdx, -dzdy, 1.0));
            out_albedo    = srgb_to_linear(img.rgb);
            return true;
        }
    }
    return false;
}

// Evaluates direct outgoing diffuse + emissive radiance for a hit surface
float3 lighting_eval_direct_radiance(
    LightingFrameHeader hdr,
    float3              world_pos,
    float3              world_nrm,
    float3              view_nrm,
    float3              albedo,
    float3              emissive,
    bool                is_terrain)
{
    float3 direct_lit = float3(0.0);
    if (is_terrain) {
        const float3 sun_dir  = normalize(float3(-0.50, -0.52, 0.69));
        const float3 fill_dir = normalize(float3(0.55, 0.45, 0.70));
        float ndl  = max(dot(world_nrm, sun_dir), 0.0);
        float fill = max(dot(world_nrm, fill_dir), 0.0);
        float3 sun_light = float3(1.06, 0.99, 0.92) * (0.68 * ndl);
        float3 sky_fill  = float3(0.42, 0.48, 0.56) * (0.36 + 0.20 * world_nrm.z + 0.14 * fill);
        direct_lit = albedo * (sun_light + sky_fill);
    } else {
        const float3 to_sun  = normalize(float3(0.28, 0.86, 0.42));
        const float3 to_fill = normalize(float3(-0.62, -0.24, 0.74));
        float hemi   = clamp(view_nrm.y * 0.5 + 0.5, 0.0, 1.0);
        float3 amb   = lerp(float3(0.11, 0.13, 0.18), float3(0.24, 0.30, 0.40), hemi);
        float ndl    = max(dot(view_nrm, to_sun), 0.0);
        float ndfill = max(dot(view_nrm, to_fill), 0.0) * 0.22;
        direct_lit   = albedo * (amb + float3(1.00, 0.96, 0.88) * ndl + float3(0.20, 0.30, 0.46) * ndfill);
    }

    // Multi-bounce feedback: inject previous frame's diffuse irradiance attenuated by albedo!
    if (hdr.history_valid != 0u && hdr.gi_prev_bda != 0ull) {
        float prev_vz = 0.0;
        float2 puv = lighting_world_to_prev_gi_uv(hdr, world_pos, prev_vz);
        if (all(puv >= float2(0.0)) && all(puv <= float2(1.0))) {
            uint px = min(uint(puv.x * float(hdr.gi_width)), hdr.gi_width - 1u);
            uint py = min(uint(puv.y * float(hdr.gi_height)), hdr.gi_height - 1u);
            LightingGiTexel prev_tex = ((LightingGiTexel *)hdr.gi_prev_bda)[py * hdr.gi_width + px];
            if (prev_tex.view_z > -1.0e19 && abs(prev_tex.view_z - prev_vz) * max(hdr.prev_zoom, 0.02) < 18.0) {
                float3 prev_irr = lighting_unpack_rgb9e5_gpu(prev_tex.irr_rgb9e5);
                direct_lit += min(albedo, float3(0.82)) * min(prev_irr, float3(4.0)) * 0.55;
            }
        }
    }

    return direct_lit + emissive * 2.35;
}

float3 lighting_eval_sky_env(LightingFrameHeader hdr, float3 world_dir) {
    if (hdr.terrain_dir_bda != 0ull) {
        const float3 sun_dir = normalize(float3(-0.50, -0.52, 0.69));
        float up  = clamp(world_dir.z * 0.5 + 0.5, 0.0, 1.0);
        float sun = pow(max(dot(world_dir, sun_dir), 0.0), 8.0);
        float3 sky = lerp(float3(0.18, 0.16, 0.14), float3(0.36, 0.46, 0.62), up);
        return sky + float3(0.55, 0.48, 0.36) * sun;
    }
    float4 q = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 vdir = view_rotate(q, world_dir);
    const float3 to_sun = normalize(float3(0.28, 0.86, 0.42));
    float up  = clamp(world_dir.z * 0.5 + 0.5, 0.0, 1.0);
    float sun = pow(max(dot(vdir, to_sun), 0.0), 6.0);
    float3 sky = lerp(float3(0.08, 0.12, 0.20), float3(0.22, 0.30, 0.44), up);
    return sky + float3(0.42, 0.38, 0.30) * sun;
}
