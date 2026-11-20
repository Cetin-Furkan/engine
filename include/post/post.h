#ifndef ENGINE_POST_H
#define ENGINE_POST_H

#include "graph/graph.h"
#include "vulkan/types.h"
#include <stdint.h>

// ============================================================================
// Engine v0.3.1: Unified Linear HDR Post-Processing Subsystem
// - Primary Format: VK_FORMAT_B10G11R11_UFLOAT_PACK32 (4 B/px, 50% bandwidth)
//   with automatic fallback to VK_FORMAT_R16G16B16A16_SFLOAT
// - Fused Karis-Average SPD Subgroup Bloom Downsample + 256-Bin Center-Weighted
//   Log-Luminance Histogram (zero extra HDR read pass)
// - Percentile-Trimmed (10%..95%) GPU BDA Temporal Auto-Exposure (zero stall)
// - Energy-Conserving Jimenez 3x3 Tent Progressive Upsample
// - Unified Troy Sobotka AgX ("Punchy") Tone Mapping + Triangular-PDF IGN Dither
// ============================================================================

typedef struct PostExposureState {
    alignas(16) float ev100;
    float    exposure_scale;
    float    avg_luma;
    float    target_ev100;
    uint32_t valid;
    uint32_t frame_index;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t reserved[8];
} PostExposureState;

static_assert(sizeof(PostExposureState) == 64u, "PostExposureState must match 64-byte shader struct");

typedef struct PostBenchStats {
    bool     bloom_enabled;
    bool     auto_exposure_enabled;
    VkFormat hdr_format;
    uint64_t vram_bytes;
    float    last_exposure_scale;
    float    last_ev100;
} PostBenchStats;

[[nodiscard]] int post_init(VulkanContext *vk);
void post_shutdown(VulkanContext *vk);
void post_reset_exposure(void);

// Timeline-guarded per-frame deferred reclamation of resized bloom targets.
void post_claim_frame(VkSemaphore acquire, uint64_t signal_point);

// Selected linear HDR render target format (B10G11R11_UFLOAT or R16G16B16A16_SFLOAT).
[[nodiscard]] VkFormat post_hdr_format(void);

// Registers Post-Processing passes (BloomDown+Hist, Exposure, BloomUp, PostAgX/Copy) into RgGraph.
void post_add_graph_passes(RgGraph *g,
                           RgResHandle hdr_res,
                           VkImageView hdr_view,
                           RgResHandle swap_res,
                           VkImageView swap_view,
                           VkFormat swap_format,
                           uint32_t width,
                           uint32_t height,
                           bool hdr_active,
                           bool allow_exposure_bloom,
                           float dt_s);

bool post_toggle_bloom(void);
bool post_toggle_auto_exposure(void);
void post_get_stats(PostBenchStats *out);

#endif /* ENGINE_POST_H */
