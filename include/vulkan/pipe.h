#ifndef ENGINE_VULKAN_PIPE_H
#define ENGINE_VULKAN_PIPE_H

#include "vulkan/types.h"
#include <stddef.h>

// Vulkan 1.4 maintenance5. SPIR-V is chained as VkShaderModuleCreateInfo.
// There is no VkShaderModule. Pipelines are created the first time a
// situation is drawn, then reused from a bank. The device pipeline cache
// is shared, so a second process start does not compile the same key again.
// Viewport and scissor stay dynamic. Other state is part of the key, because
// the draws do not set it on the command buffer.

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
    const VkSpecializationInfo  *frag_spec;
} VulkanGfxPipe;

// One graphics situation. Armed at init, created on the first draw of this
// format, depth, and specialization. SPIR-V bytes must outlive the armed
// record; specialization constants and map entries are deep-copied inline.
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

// Creates one graphics pipeline per color format. Same SPIR-V, same state.
// specs is null, or one entry per format (each entry may be null).
// out[i] matches formats[i]. A matching bank entry is reused and its
// reference count goes up. On failure every out[i] is null.
[[nodiscard]] int vulkan_gfx_pipelines(VulkanContext *vk, const VulkanGfxPipe *info,
                                       const VkFormat *formats,
                                       const VkSpecializationInfo *const *specs,
                                       uint32_t count, VkPipeline *out);

[[nodiscard]] VkPipeline vulkan_compute_pipeline(VulkanContext *vk, const void *spv,
                                                 size_t bytes, VkPipelineLayout layout);

void vulkan_gfx_arm(VulkanGfxLive *live, const VulkanGfxPipe *info, VkFormat format,
                    const VkSpecializationInfo *spec);
// Creates the pipeline on the first call. Later calls return the same one.
[[nodiscard]] VkPipeline vulkan_gfx_use(VulkanContext *vk, VulkanGfxLive *live);
void vulkan_gfx_disarm(VulkanContext *vk, VulkanGfxLive *live);

// Drops one bank reference. The pipeline is destroyed at zero.
// A null handle is ignored.
void vulkan_pipe_release(VulkanContext *vk, VkPipeline pipeline);

void vulkan_pipe_cache_open(VulkanContext *vk);
void vulkan_pipe_cache_close(VulkanContext *vk);

#endif /* ENGINE_VULKAN_PIPE_H */
