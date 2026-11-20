// Shared Slang 2026 Compute Helpers for SSILVB + Split Radiance Cascades (HRC)
// Matches include/lighting/lighting.h byte-for-byte.

#include "camera.slang"
import terrain_math;
#include "lighting.slang"

static const uint  k_proxy_obb       = k_lighting_proxy_obb;
static const uint  k_proxy_ellipsoid = k_lighting_proxy_ellipsoid;
static const float k_gi_pi           = 3.14159265359;
static const float k_gi_half_pi      = 1.57079632679;
static const float k_gi_ray_near     = 65536.0;

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
    return lighting_proxy_albedo(p);
}

float3 proxy_emissive(LightingProxyGpu p) {
    return lighting_proxy_emissive(p);
}

float proxy_bound_radius(LightingProxyGpu p) {
    return lighting_proxy_bound_radius(p);
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

float3 lighting_pixel_to_view_pos(LightingFrameHeader hdr, float persp, float2 full_px, float view_z) {
    float2 vxy    = lighting_pixel_to_view_xy(hdr, full_px);
    float  w_clip = (persp > 1.0e-7) ? max(1.0 - view_z * persp, 1.0e-4) : 1.0;
    return float3(vxy.x * w_clip, vxy.y * w_clip, view_z);
}

float3 lighting_view_to_world(LightingFrameHeader hdr, float3 vpos) {
    float4 q = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 c = float3(hdr.cam_x, hdr.cam_y, hdr.cam_z);
    return c + lighting_inv_view_rotate(q, vpos);
}

// Builds exact world-space camera ray for full-res pixel coordinate (perspective or orthographic)
void lighting_primary_ray(
    LightingFrameHeader hdr,
    float               persp,
    float2              full_px,
    out float3          out_ro,
    out float3          out_rd,
    out float           out_max_t)
{
    float4 q   = float4(hdr.qx, hdr.qy, hdr.qz, hdr.qw);
    float3 cam = float3(hdr.cam_x, hdr.cam_y, hdr.cam_z);
    float2 vxy = lighting_pixel_to_view_xy(hdr, full_px);

    if (persp > 1.0e-7) {
        float  d_cam = 1.0 / persp;
        float3 rd_v  = normalize(float3(vxy.x * persp, vxy.y * persp, -1.0));
        out_ro       = cam + lighting_inv_view_rotate(q, float3(0.0, 0.0, d_cam));
        out_rd       = normalize(lighting_inv_view_rotate(q, rd_v));
        out_max_t    = d_cam * 24.0;
    } else {
        out_ro       = cam + lighting_inv_view_rotate(q, float3(vxy.x, vxy.y, k_gi_ray_near));
        out_rd       = normalize(lighting_inv_view_rotate(q, float3(0.0, 0.0, -1.0)));
        out_max_t    = k_gi_ray_near + 32768.0;
    }
}

float2 lighting_world_to_prev_gi_uv(LightingFrameHeader hdr, float prev_persp, float3 world_pos, out float out_prev_vz) {
    float4 pq   = float4(hdr.prev_qx, hdr.prev_qy, hdr.prev_qz, hdr.prev_qw);
    float3 pc   = float3(hdr.prev_cam_x, hdr.prev_cam_y, hdr.prev_cam_z);
    float3 pv   = view_of(world_pos, pq, pc);
    out_prev_vz = pv.z;
    float pz    = max(hdr.prev_zoom, 1.0e-5);
    float pw    = (prev_persp > 1.0e-7) ? max(1.0 - pv.z * prev_persp, 1.0e-3) : 1.0;
    float px    = float(hdr.full_width)  * 0.5 + (pv.x / pw) * pz;
    float py    = float(hdr.full_height) * 0.5 - (pv.y / pw) * pz;
    return float2(px / max(float(hdr.full_width), 1.0), py / max(float(hdr.full_height), 1.0));
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
    return lighting_ray_proxy(ro, rd, t_min, t_max, proxy, out_t, out_world_nrm);
}

// Ultra-fast 1-tap scalar DEM height query for ray-interval bracketing
bool lighting_sample_terrain_height_fast(
    uint64_t  terrain_dir_bda,
    float     elev_base_m,
    float     vertical_scale,
    float2    world_xy,
    int       max_lod,
    out float out_world_z)
{
    out_world_z = 0.0;
    if (terrain_dir_bda == 0ull) {
        return false;
    }

    if (dir_is_v2(terrain_dir_bda)) {
        float half_ext = dir_world_half_extent(terrain_dir_bda);
        if (abs(world_xy.x) > half_ext || abs(world_xy.y) > half_ext) {
            return false;
        }
        GpuMathQuadNodeGpu root = load_quad_node(terrain_dir_bda, 0u);
        if (root.rel_ssd_payload == 0ull) {
            return false;
        }
        uint64_t dem_bda = relptr2d_resolve_gpu(terrain_dir_bda, root.rel_ssd_payload);
        float dem_span = 0.0;
        float2 uv = relptr2d_uv_and_span(terrain_dir_bda, root.rel_ssd_payload, world_xy, dem_span);
        uint  sx = min(uint(uv.x * 255.0 + 0.5), 255u);
        uint  sy = min(uint(uv.y * 255.0 + 0.5), 255u);
        float elev_m = tile_load_height_m(dem_bda, sx, sy);
        out_world_z  = (elev_m - elev_base_m) * vertical_scale;
        return true;
    }

    if (abs(world_xy.x) > 12000.0 || abs(world_xy.y) > 12000.0) {
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

    if (hdr.terrain_dir_bda == 0ull) {
        return false;
    }

    if (dir_is_v2(hdr.terrain_dir_bda)) {
        float half_ext = dir_world_half_extent(hdr.terrain_dir_bda);
        if (abs(world_xy.x) > half_ext || abs(world_xy.y) > half_ext) {
            return false;
        }
        GpuMathQuadNodeGpu chosen = load_quad_node(hdr.terrain_dir_bda, 0u);
        if (max_lod > 0) {
            uint leaf_cnt = min(((uint *)hdr.terrain_dir_bda)[20u], k_epack_v2_cut_cap);
            for (uint li = 0u; li < leaf_cnt; ++li) {
                uint nidx = load_active_leaf_index(hdr.terrain_dir_bda, li);
                GpuMathQuadNodeGpu cand = load_quad_node(hdr.terrain_dir_bda, nidx);
                if (world_xy.x >= cand.world_min_x && world_xy.x <= cand.world_max_x &&
                    world_xy.y >= cand.world_min_y && world_xy.y <= cand.world_max_y) {
                    chosen = cand;
                    break;
                }
            }
        }
        uint64_t dem_rel = chosen.rel_ssd_payload;
        uint64_t rgb_rel = chosen.rel_gpu_payload;
        if (dem_rel == 0ull || rgb_rel == 0ull) {
            GpuMathQuadNodeGpu root = load_quad_node(hdr.terrain_dir_bda, 0u);
            if (dem_rel == 0ull) dem_rel = root.rel_ssd_payload;
            if (rgb_rel == 0ull) rgb_rel = root.rel_gpu_payload;
        }
        if (dem_rel == 0ull || rgb_rel == 0ull) {
            return false;
        }
        uint64_t dem_bda = relptr2d_resolve_gpu(hdr.terrain_dir_bda, dem_rel);
        uint64_t rgb_bda = relptr2d_resolve_gpu(hdr.terrain_dir_bda, rgb_rel);
        float    dem_span = 1.0, rgb_span = 1.0;
        float2   dem_uv   = relptr2d_uv_and_span(hdr.terrain_dir_bda, dem_rel, world_xy, dem_span);
        float2   rgb_uv   = relptr2d_uv_and_span(hdr.terrain_dir_bda, rgb_rel, world_xy, rgb_span);
        float2   dh_duv   = float2(0.0);
        float    elev_m   = tile_sample_bilinear_height_grad_m(dem_bda, dem_uv, dh_duv);
        float4   img      = tile_sample_bilinear_imagery(rgb_bda, rgb_uv);
        float    dzdx     = (dh_duv.x / dem_span) * hdr.vertical_scale;
        float    dzdy     = (dh_duv.y / dem_span) * hdr.vertical_scale;
        out_world_z   = (elev_m - hdr.elev_base_m) * hdr.vertical_scale;
        out_world_nrm = normalize(float3(-dzdx, -dzdy, 1.0));
        out_albedo    = srgb_to_linear(img.rgb);
        return true;
    }

    if (abs(world_xy.x) > 12000.0 || abs(world_xy.y) > 12000.0) {
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

// Fast direct + multi-bounce outgoing radiance for G-buffer / HRC (zero software shadow ray loops!)
float3 lighting_eval_direct_radiance_fast(
    LightingFrameHeader hdr,
    LightingShadowBlock sb,
    float3              world_pos,
    float3              world_nrm,
    float3              albedo_in,
    float3              emissive,
    float               sun_vis,
    bool                is_terrain)
{
    float3 alb_lin = is_terrain ? albedo_in : lighting_srgb_to_linear(albedo_in);
    float3 direct_lit = float3(0.0);
    if (is_terrain) {
        float3 sun_dir   = lighting_terrain_sun_dir();
        float3 fill_dir  = lighting_terrain_fill_dir();
        float  ndl       = max(dot(world_nrm, sun_dir), 0.0);
        float  fill      = max(dot(world_nrm, fill_dir), 0.0);
        float3 sun_light = float3(1.06, 0.99, 0.92) * (0.68 * ndl);
        float3 sky_fill  = float3(0.42, 0.48, 0.56) * (0.36 + 0.20 * world_nrm.z + 0.14 * fill);
        direct_lit = alb_lin * (sun_light + sky_fill);
    } else {
        float  ndl     = max(dot(world_nrm, sb.sun_dir), 0.0);
        float  ndfill  = max(dot(world_nrm, lighting_world_fill_dir()), 0.0) * 0.25;
        float  hemi    = clamp(world_nrm.z * 0.5 + 0.5, 0.0, 1.0);
        float3 sky_amb = lerp(sb.sky_ground, sb.sky_top, hemi) * 0.55;
        direct_lit = alb_lin * (sky_amb + sb.sun_color * (ndl * sun_vis * k_lighting_inv_pi) +
                                float3(0.30, 0.44, 0.68) * (ndfill * k_lighting_inv_pi));
    }

    // Multi-bounce feedback from previous frame's diffuse GI irradiance
    if (hdr.history_valid != 0u && hdr.gi_prev_bda != 0ull) {
        float  prev_vz = 0.0;
        float2 puv     = lighting_world_to_prev_gi_uv(hdr, sb.prev_persp, world_pos, prev_vz);
        if (all(puv >= float2(0.0)) && all(puv <= float2(1.0))) {
            uint px = min(uint(puv.x * float(hdr.gi_width)),  hdr.gi_width  - 1u);
            uint py = min(uint(puv.y * float(hdr.gi_height)), hdr.gi_height - 1u);
            LightingGiTexel prev_tex = ((LightingGiTexel *)hdr.gi_prev_bda)[py * hdr.gi_width + px];
            if (prev_tex.view_z > -1.0e19 && abs(prev_tex.view_z - prev_vz) * max(hdr.prev_zoom, 0.02) < 18.0) {
                float3 prev_irr = lighting_unpack_rgb9e5_gpu(prev_tex.irr_rgb9e5);
                direct_lit += min(alb_lin, float3(0.85)) * min(prev_irr, float3(5.0)) * 0.45;
            }
        }
    }

    return direct_lit + emissive * 3.40;
}

float3 lighting_eval_sky_env(LightingFrameHeader hdr, float3 world_dir) {
    return lighting_eval_sky_env_hdr(hdr, world_dir);
}
