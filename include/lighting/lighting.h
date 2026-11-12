#ifndef ENGINE_LIGHTING_H
#define ENGINE_LIGHTING_H

#include "vulkan/types.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Engine v0.2 Milestone 4: First-Class Global Illumination & Visibility Kernel
// - 2026 SSILVB (Screen-Space Indirect Lighting with 32-Bit Visibility Bitmasks)
// - Split / Holographic Radiance Cascades (HRC, 4-Level Constant Space-Angle)
// - Dual Hardware Oracles (UMA Analytical+HiZ+DEM vs Discrete High-Density/RT)
// - Unified BDA Environment + Material Look Slab (Zero VkDescriptorSets)
// ============================================================================

enum : uint32_t {
    K_LIGHTING_MAGIC          = 0x4C474934u, // "LGI4"
    K_LIGHTING_MAT_SLOTS      = 64u,
    K_LIGHTING_MAX_PROXIES    = 128u,
    K_LIGHTING_MAX_CASCADES   = 4u,
    K_LIGHTING_HEADER_OFFSET  = 2048u,
    K_LIGHTING_PROXIES_OFFSET = 2304u,
    K_LIGHTING_ENV_SLICE_SIZE = 10496u,      // 2048 + 256 + 128*64 (41 * 256B)
};

enum : uint32_t {
    LIGHTING_PROXY_OBB       = 0u,
    LIGHTING_PROXY_ELLIPSOID = 1u,
};

enum : uint32_t {
    LIGHTING_ORACLE_ANALYTICAL_UMA = 0u,
    LIGHTING_ORACLE_DISCRETE_HIGH  = 1u,
};

// 32-byte material emission slot (binary-identical to shaders/material.slang).
typedef struct LightingMatSlot {
    float emit_r;
    float emit_g;
    float emit_b;
    float glow;
    float reserved[4];
} LightingMatSlot;

static_assert(sizeof(LightingMatSlot) == 32u, "LightingMatSlot must be 32 bytes");

// 64-byte cache-line-aligned 3D analytical occluder & emitter proxy for HRC + G-buffer.
typedef struct LightingProxy {
    alignas(64) float center[3];
    uint32_t kind;          // LIGHTING_PROXY_OBB or LIGHTING_PROXY_ELLIPSOID
    float    extents[3];    // Half-extents (OBB) or semi-axes (Ellipsoid)
    float    roughness;
    float    pitch_rad;
    float    yaw_rad;
    uint16_t alb_f16[3];    // Linear diffuse reflectance RGB in IEEE fp16
    uint16_t flags;
    uint16_t emi_f16[3];       // Outgoing emissive + thermal radiance RGB in IEEE fp16
    uint16_t bound_radius_f16; // Tight world-space circumsphere radius in IEEE fp16
    uint32_t trig_pitch;       // [15:0] sin(pitch) fp16, [31:16] cos(pitch) fp16
    uint32_t trig_yaw;         // [15:0] sin(yaw)   fp16, [31:16] cos(yaw)   fp16
} LightingProxy;

static_assert(sizeof(LightingProxy) == 64u, "LightingProxy must be 64 bytes");

// 16-byte GI G-buffer texel (128-bit cache-line-divisible load/store).
typedef struct LightingGbufTexel {
    alignas(16) float view_z;     // <= -1.0e20f indicates sky / empty background
    uint32_t oct_nrm_s16;         // View-space unit normal in octahedral snorm16x2
    uint32_t direct_rgb9e5;       // Direct + emissive radiance in shared-exponent RGB9E5
    uint32_t alb_flags;           // R8 | G8<<8 | B8<<16 | flags<<24
} LightingGbufTexel;

static_assert(sizeof(LightingGbufTexel) == 16u, "LightingGbufTexel must be 16 bytes");

// 16-byte temporally & spatially resolved GI texel sampled by surface shaders via BDA.
typedef struct LightingGiTexel {
    alignas(16) float view_z;     // <= -1.0e20f indicates sky / empty background
    uint32_t packed_nrm_bent;     // [15:0] oct_snorm8 view normal, [31:16] oct_snorm8 bent normal
    uint32_t irr_rgb9e5;          // Multi-bounce indirect diffuse irradiance (HRC + SSILVB)
    uint16_t ao_unorm16;          // 32-bit visibility bitmask ambient visibility [0..1] in unorm16
    uint16_t conf_unorm16;        // Temporal accumulation confidence [0..1] in unorm16
} LightingGiTexel;

static_assert(sizeof(LightingGiTexel) == 16u, "LightingGiTexel must be 16 bytes");

// 256-byte BDA frame header located at (env_bda + K_LIGHTING_HEADER_OFFSET).
typedef struct LightingFrameHeader {
    uint32_t magic;
    uint32_t gi_enabled;
    uint32_t oracle_mode;
    uint32_t frame_index;
    uint32_t full_width;
    uint32_t full_height;
    uint32_t gi_width;
    uint32_t gi_height;
    uint32_t hrc_w0;
    uint32_t hrc_h0;
    uint32_t proxy_count;
    uint32_t has_ground;
    float    ground_z;
    float    ground_r;
    float    ground_g;
    float    ground_b;
    uint64_t proxies_bda;
    uint64_t gbuf_bda;
    uint64_t hrc_bda;
    uint64_t gi_curr_bda;
    uint64_t gi_prev_bda;
    uint64_t terrain_dir_bda;
    uint64_t scene_hiz_bda;
    float    elev_base_m;
    float    vertical_scale;
    uint32_t history_valid;
    uint32_t ssilvb_slices;
    uint32_t ssilvb_steps;
    uint32_t hrc_cascades;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z, zoom;
    float    prev_qx, prev_qy, prev_qz, prev_qw;
    float    prev_cam_x, prev_cam_y, prev_cam_z, prev_zoom;
    float    gi_intensity;
    float    ao_strength;
    float    ssilvb_radius_px;
    float    thickness_world;
    uint32_t hrc_offset_rays[4];
    uint32_t reserved[4];
} LightingFrameHeader;

static_assert(offsetof(LightingFrameHeader, proxies_bda) == 64u, "proxies_bda offset");
static_assert(offsetof(LightingFrameHeader, qx) == 144u, "camera quat offset");
static_assert(offsetof(LightingFrameHeader, hrc_offset_rays) == 224u, "hrc offsets");
static_assert(sizeof(LightingFrameHeader) == 256u, "LightingFrameHeader must be 256 bytes");

typedef struct LightingCameraParams {
    float qx, qy, qz, qw;
    float cam_x, cam_y, cam_z;
    float zoom;
} LightingCameraParams;

typedef struct LightingSsilvbSample {
    float delta_view[3]; // Neighbor view-space offset (P_sample - P_center)
    float normal_view[3];// Neighbor view-space unit normal
    float radiance[3];   // Neighbor outgoing direct+emissive radiance
} LightingSsilvbSample;

typedef struct LightingSsilvbSliceResult {
    uint32_t vis_mask;       // Final 32-bit angular sector occlusion bitmask
    float    visibility;     // Unoccluded hemisphere fraction [0..1]
    float    bent_tilt;      // Unoccluded direction bias in [-1..+1] along slice tangent
    float    indirect_rgb[3];// Single-bounce local diffuse irradiance accumulated along slice
} LightingSsilvbSliceResult;

typedef struct LightingBenchStats {
    bool     gi_enabled;
    uint32_t oracle_mode;
    uint32_t gi_width;
    uint32_t gi_height;
    uint32_t hrc_probes_c0;
    uint32_t hrc_total_rays;
    uint32_t ssilvb_slices;
    uint32_t ssilvb_steps;
    uint32_t active_proxies;
    uint64_t bda_bytes;
} LightingBenchStats;

// ----------------------------------------------------------------------------
// Pure C23 Mathematical, Bitmask & Codec Kernels (CPU + Standalone Testable)
// ----------------------------------------------------------------------------

uint16_t lighting_f32_to_f16(float v);
float    lighting_f16_to_f32(uint16_t h);

uint32_t lighting_pack_rgb9e5(float r, float g, float b);
void     lighting_unpack_rgb9e5(uint32_t packed, float out_rgb[3]);

uint32_t lighting_pack_oct_snorm16(float nx, float ny, float nz);
void     lighting_unpack_oct_snorm16(uint32_t packed, float out_nrm[3]);

uint16_t lighting_pack_oct_snorm8(float nx, float ny, float nz);
void     lighting_unpack_oct_snorm8(uint16_t packed, float out_nrm[3]);

// 2026 SSILVB 32-Bit Visibility Bitmask sector encoder for angular span [alpha_min, alpha_max] in [0, 1].
uint32_t lighting_ssilvb_sector_mask(float alpha_min, float alpha_max);

// Evaluates one horizon slice along tangent (tx, ty, 0) using 32-bit visibility bitmasks.
LightingSsilvbSliceResult lighting_ssilvb_eval_slice(const float center_nrm[3],
                                                     float slice_cos,
                                                     float slice_sin,
                                                     float thickness,
                                                     const LightingSsilvbSample *samples,
                                                     uint32_t sample_count);

// Exact Split / Holographic Radiance Cascade interval merge operator:
// L_c(x, w) = L_near + beta_near * 0.5 * (L_far_a + L_far_b).
void lighting_hrc_merge_ray(const float near_rad[3], float near_beta,
                            const float far_rad_a[3], const float far_rad_b[3],
                            float out_rad[3]);

// Jimenez 2016 / 2026 albedo-preserving multi-bounce ambient occlusion curve.
void lighting_gtao_multibounce(float visibility, const float albedo[3], float out_ao_rgb[3]);

// Analytical world-space ray interval [t_min, t_max] intersection against a 3D LightingProxy.
bool lighting_ray_proxy_interval(const float ro[3], const float rd[3],
                                 float t_min, float t_max,
                                 const LightingProxy *proxy,
                                 float *out_t, float out_nrm[3]);

// Helper to populate a 64-byte LightingProxy with IEEE fp16 albedo/emissive fields.
void lighting_proxy_fill(LightingProxy *out, uint32_t kind,
                         const float center[3], const float extents[3],
                         float pitch_rad, float yaw_rad, float roughness,
                         const float albedo[3], const float emissive[3]);

[[nodiscard]] bool lighting_self_check(void);

// ----------------------------------------------------------------------------
// Vulkan 1.4 BDA GPU Lighting Runtime (SSILVB + HRC + Unified Material Block)
// ----------------------------------------------------------------------------

[[nodiscard]] int lighting_init(VulkanContext *vk);
void lighting_shutdown(VulkanContext *vk);
void lighting_reset(void);
void lighting_invalidate_history(void);

void lighting_material_set(uint32_t slot, float emit_r, float emit_g, float emit_b, float glow);
void lighting_material_get_emissive(uint32_t slot, float out_emit[3]);
VkDeviceAddress lighting_env_bda(void);

void lighting_begin_frame(uint32_t width, uint32_t height, const LightingCameraParams *cam);
void lighting_set_ground_plane(bool enabled, float z_height, const float base_albedo[3]);
void lighting_set_terrain_field(uint64_t terrain_dir_bda, float elev_base_m, float vertical_scale);
void lighting_set_scene_hiz(uint64_t scene_hiz_bda);
void lighting_submit_proxy(const LightingProxy *proxy);

void lighting_prepare(VkCommandBuffer cmd, uint32_t width, uint32_t height);

bool lighting_gi_active(void);
bool lighting_toggle_gi(void);
void lighting_get_stats(LightingBenchStats *out_stats);

#endif /* ENGINE_LIGHTING_H */
