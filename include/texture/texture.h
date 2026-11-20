#ifndef ENGINE_TEXTURE_H
#define ENGINE_TEXTURE_H

#include "vulkan/types.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Engine v0.3.3: Vulkan 1.4 Bindless PBR Texture Heap & BDA Material Table
// - Single persistent UPDATE_AFTER_BIND + PARTIALLY_BOUND bindless descriptor
//   set (Set 1: Texture2D[1024] @ binding 0 + SamplerState[18] @ binding 1)
// - Lazy GPU allocation: 0 VRAM and 0 descriptor pools in World 0 / minimal
// - Auto-detected PNG (libpng16), JPEG (libjpeg), and WebP (libwebp) decoding
// - Full GPU mipmap pyramid generation via vkCmdBlitImage2 + anisotropic filter
// - Timeline-guarded zero-stall staging & retirement (SyncVal + GPU-AV clean)
// - 128-byte BDA PbrMaterialGpu table supporting full glTF 2.0 & Blender BSDF
// ============================================================================

enum : uint32_t {
    K_TEX_MAX_IMAGES    = 1024u,
    K_TEX_SAMPLER_COUNT = 18u,   // 3 wrapS * 3 wrapT * 2 filter modes
    K_TEX_MAX_MATERIALS = 512u,
    K_TEX_WHITE_SRGB    = 0u,    // Built-in 1x1 white sRGB fallback
    K_TEX_WHITE_LINEAR  = 1u,    // Built-in 1x1 white linear fallback
    K_TEX_FLAT_NORMAL   = 2u,    // Built-in 1x1 (128,128,255,255) flat tangent normal
    K_TEX_FIRST_USER    = 3u,
    K_TEX_INVALID       = 0xFFFFFFFFu,
};

typedef enum TextureWrap : uint8_t {
    TEX_WRAP_REPEAT = 0u,
    TEX_WRAP_CLAMP  = 1u,
    TEX_WRAP_MIRROR = 2u,
} TextureWrap;

typedef enum TextureFilter : uint8_t {
    TEX_FILTER_LINEAR  = 0u,
    TEX_FILTER_NEAREST = 1u,
} TextureFilter;

typedef enum PbrMaterialFlags : uint32_t {
    PBR_MAT_FLAG_NONE         = 0u,
    PBR_MAT_FLAG_DOUBLE_SIDED = 1u << 0,
    PBR_MAT_FLAG_ALPHA_MASK   = 1u << 1,
    PBR_MAT_FLAG_ALPHA_BLEND  = 1u << 2,
    PBR_MAT_FLAG_UNLIT        = 1u << 3,
    PBR_MAT_FLAG_HAS_TANGENTS = 1u << 4,
    PBR_MAT_FLAG_HAS_NORMAL   = 1u << 5,
    PBR_MAT_FLAG_HAS_OCC      = 1u << 6,
    PBR_MAT_FLAG_HAS_BASE_TEX = 1u << 7,
    PBR_MAT_FLAG_HAS_MR_TEX   = 1u << 8,
    PBR_MAT_FLAG_HAS_EMI_TEX  = 1u << 9,
} PbrMaterialFlags;

// 128-byte BDA PBR material record (matches shaders/pbr_bindless.slang byte-for-byte).
typedef struct PbrMaterialGpu {
    alignas(16) float base_color_factor[4]; // Linear RGBA (glTF 2.0 spec)
    float    emissive_factor[3];            // Linear RGB * KHR_materials_emissive_strength
    float    metallic_factor;               // [0..1]
    float    roughness_factor;              // [0..1]
    float    normal_scale;                  // Normal map XY scale (default 1.0)
    float    occlusion_strength;            // AO map strength (default 1.0)
    float    alpha_cutoff;                  // Alpha mask threshold (default 0.5)
    float    ior;                           // Index of refraction (default 1.5, KHR_materials_ior)
    float    uv_cos;                        // cos(KHR_texture_transform.rotation)
    float    uv_sin;                        // sin(KHR_texture_transform.rotation)
    uint32_t flags;                         // PbrMaterialFlags bitmask
    float    uv_offset[2];                  // KHR_texture_transform.offset
    float    uv_scale[2];                   // KHR_texture_transform.scale (default 1, 1)
    uint32_t tex_base;                      // Bindless Texture2D slot (sRGB)
    uint32_t samp_base;                     // Bindless SamplerState slot [0..17]
    uint32_t tex_mr;                        // Bindless Texture2D slot (linear: G=rough, B=metal)
    uint32_t samp_mr;
    uint32_t tex_normal;                    // Bindless Texture2D slot (linear tangent-space)
    uint32_t samp_normal;
    uint32_t tex_occ;                       // Bindless Texture2D slot (linear: R=occlusion)
    uint32_t samp_occ;
    uint32_t tex_emissive;                  // Bindless Texture2D slot (sRGB)
    uint32_t samp_emissive;
    uint32_t pad0;
    uint32_t pad1;
} PbrMaterialGpu;

static_assert(sizeof(PbrMaterialGpu) == 128u, "PbrMaterialGpu must be 128 bytes");

typedef struct TextureBenchStats {
    uint32_t texture_count;
    uint32_t material_count;
    uint64_t vram_bytes;
    uint32_t mip_levels_total;
    bool     bindless_active;
} TextureBenchStats;

static inline uint32_t texture_sampler_index(TextureWrap wrap_s, TextureWrap wrap_t,
                                             TextureFilter filter) {
    uint32_t ws = (uint32_t)wrap_s;
    uint32_t wt = (uint32_t)wrap_t;
    uint32_t fl = (uint32_t)filter;
    if (ws > 2u) ws = 0u;
    if (wt > 2u) wt = 0u;
    if (fl > 1u) fl = 0u;
    return ((ws * 3u + wt) * 2u) + fl;
}

static inline PbrMaterialGpu texture_pbr_material_default(void) {
    return (PbrMaterialGpu){
        .base_color_factor  = { 1.0f, 1.0f, 1.0f, 1.0f },
        .emissive_factor    = { 0.0f, 0.0f, 0.0f },
        .metallic_factor    = 0.0f,
        .roughness_factor   = 0.5f,
        .normal_scale       = 1.0f,
        .occlusion_strength = 1.0f,
        .alpha_cutoff       = 0.5f,
        .ior                = 1.5f,
        .uv_cos             = 1.0f,
        .uv_sin             = 0.0f,
        .flags              = PBR_MAT_FLAG_NONE,
        .uv_offset          = { 0.0f, 0.0f },
        .uv_scale           = { 1.0f, 1.0f },
        .tex_base           = K_TEX_WHITE_SRGB,
        .samp_base          = 0u,
        .tex_mr             = K_TEX_WHITE_LINEAR,
        .samp_mr            = 0u,
        .tex_normal         = K_TEX_FLAT_NORMAL,
        .samp_normal        = 0u,
        .tex_occ            = K_TEX_WHITE_LINEAR,
        .samp_occ           = 0u,
        .tex_emissive       = K_TEX_WHITE_SRGB,
        .samp_emissive      = 0u,
    };
}

// ---------------------------------------------------------------------------
// CPU Image Decoders (PNG / JPEG / WebP auto-detected by header magic)
// Caller frees *out_rgba with free().
// ---------------------------------------------------------------------------

[[nodiscard]] int texture_decode_memory(const uint8_t *bytes, size_t len,
                                        uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h);
[[nodiscard]] int texture_decode_file(const char *path,
                                      uint8_t **out_rgba, uint32_t *out_w, uint32_t *out_h);

// ---------------------------------------------------------------------------
// Vulkan 1.4 Bindless Texture & BDA Material Runtime
// ---------------------------------------------------------------------------

[[nodiscard]] int texture_init(VulkanContext *vk);
void texture_shutdown(VulkanContext *vk);
void texture_claim_frame(VkSemaphore acquire, uint64_t signal_point);

[[nodiscard]] bool texture_ensure_ready(void);
[[nodiscard]] VkDescriptorSetLayout texture_bindless_set_layout(void);
[[nodiscard]] VkDescriptorSet texture_bindless_set(void);
[[nodiscard]] uint64_t texture_materials_bda(void);

[[nodiscard]] uint32_t texture_create_rgba8(const uint8_t *rgba, uint32_t width, uint32_t height,
                                            bool srgb);
[[nodiscard]] uint32_t texture_create_from_memory(const uint8_t *encoded, size_t len, bool srgb);
[[nodiscard]] uint32_t texture_create_from_file(const char *path, bool srgb);
[[nodiscard]] uint32_t texture_register_material(const PbrMaterialGpu *mat);

void texture_reset_scene_assets(void);
void texture_flush_uploads(VkCommandBuffer cmd);
void texture_get_stats(TextureBenchStats *out_stats);

#endif /* ENGINE_TEXTURE_H */
