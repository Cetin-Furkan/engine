#ifndef ENGINE_VULKAN_PIPE_H
#define ENGINE_VULKAN_PIPE_H

#include "vulkan/types.h"
#include <stddef.h>

// Vulkan 1.4 maintenance5. SPIR-V is chained as VkShaderModuleCreateInfo.
// There is no VkShaderModule. The device pipeline cache is shared.

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
    const VkSpecializationInfo  *frag_spec;
} VulkanGfxPipe;

// Creates one graphics pipeline per color format. Same SPIR-V, same state.
// specs is null, or one entry per format (each entry may be null).
// out[i] matches formats[i]. On failure every out[i] is null.
[[nodiscard]] int vulkan_gfx_pipelines(VulkanContext *vk, const VulkanGfxPipe *info,
                                       const VkFormat *formats,
                                       const VkSpecializationInfo *const *specs,
                                       uint32_t count, VkPipeline *out);

[[nodiscard]] VkPipeline vulkan_compute_pipeline(VulkanContext *vk, const void *spv,
                                                 size_t bytes, VkPipelineLayout layout);

void vulkan_pipe_cache_open(VulkanContext *vk);
void vulkan_pipe_cache_close(VulkanContext *vk);

#endif /* ENGINE_VULKAN_PIPE_H */
