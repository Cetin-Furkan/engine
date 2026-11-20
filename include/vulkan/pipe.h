#ifndef ENGINE_VULKAN_PIPE_H
#define ENGINE_VULKAN_PIPE_H

#include "vulkan/types.h"
#include <stddef.h>

// Vulkan 1.4 maintenance5 + Extended Dynamic State pipeline bank.
// SPIR-V is chained directly as VkShaderModuleCreateInfo (zero VkShaderModule).
// Viewport, scissor, cull mode, front face, depth test/write/compare (and
// optional depth bias) are bound dynamically per draw via Vulkan 1.4 core
// dynamic state, collapsing PSO permutations to shader + format + blend + MSAA.

enum : uint32_t {
    VULKAN_GFX_SPEC_BYTES = 32u,
    VULKAN_GFX_SPEC_MAPS  = 4u,
};

typedef struct VulkanGfxPipe {
    const void                  *vert;
    size_t                       vert_bytes;
    const void                  *frag;
    size_t                       frag_bytes;
    VkPipelineLayout             layout;
    VkFormat                     color_format;
    VkFormat                     depth_format;
    VkSampleCountFlagBits        samples;
    VkCullModeFlags              cull;
    VkBool32                     depth_test;
    VkBool32                     depth_write;
    VkCompareOp                  depth_compare;
    VkBool32                     blend;
    bool                         custom_depth_compare;
    bool                         depth_bias_enable;
    const VkSpecializationInfo  *frag_spec;
} VulkanGfxPipe;

// One armed graphics situation. Created lazily on first bind and cached in the
// shared device pipeline bank + disk VkPipelineCache.
typedef struct VulkanGfxLive {
    VulkanGfxPipe                info;
    VkFormat                     format;
    VkPipeline                   pipeline;
    VkSpecializationInfo         owned_spec;
    VkSpecializationMapEntry     spec_maps[VULKAN_GFX_SPEC_MAPS];
    uint8_t                      spec_data[VULKAN_GFX_SPEC_BYTES];
    bool                         has_spec;
    bool                         armed;
    bool                         failed;
} VulkanGfxLive;

[[nodiscard]] VkPipeline vulkan_compute_pipeline(VulkanContext *vk, const void *spv,
                                                 size_t bytes, VkPipelineLayout layout);

void vulkan_gfx_arm(VulkanGfxLive *live, const VulkanGfxPipe *info, VkFormat format,
                    const VkSpecializationInfo *spec);

// Lazily creates/reuses the graphics PSO, binds it, and emits Vulkan 1.4
// Extended Dynamic State (cull, front face, depth test/write/compare).
[[nodiscard]] bool vulkan_gfx_bind(VulkanContext *vk, VkCommandBuffer cmd, VulkanGfxLive *live);

void vulkan_gfx_disarm(VulkanContext *vk, VulkanGfxLive *live);

// Drops one bank reference. The pipeline is destroyed at zero.
void vulkan_pipe_release(VulkanContext *vk, VkPipeline pipeline);

void vulkan_pipe_cache_open(VulkanContext *vk);
void vulkan_pipe_cache_close(VulkanContext *vk);

#endif /* ENGINE_VULKAN_PIPE_H */
