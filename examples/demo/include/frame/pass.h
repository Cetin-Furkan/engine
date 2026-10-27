#ifndef ENGINE_FRAME_PASS_H
#define ENGINE_FRAME_PASS_H

#include "vulkan/types.h"
#include <stdint.h>

// Shared depth and MSAA targets for the 3D pass. Kinds draw into these
// images. They do not own them. Format and sample count are chosen once.
// Every graphics pipeline in the pass uses that pair, so dynamic rendering
// attachments stay compatible.

typedef struct PassDepth {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} PassDepth;

// Empty when the device only has one sample. The draw then goes straight
// to the dma-buf.
typedef struct PassCoverage {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} PassCoverage;

[[nodiscard]] int pass_init(VulkanContext *vk);
void pass_shutdown(VulkanContext *vk);

[[nodiscard]] int pass_depth_create(VulkanContext *vk, PassDepth *depth,
                                    uint32_t width, uint32_t height);
void pass_depth_destroy(VulkanContext *vk, PassDepth *depth);

[[nodiscard]] int pass_coverage_create(VulkanContext *vk, PassCoverage *coverage,
                                       uint32_t width, uint32_t height);
void pass_coverage_destroy(VulkanContext *vk, PassCoverage *coverage);

// Depth starts undefined. The barrier makes it a depth attachment before
// the render clears it.
void pass_prepare_draw(VkCommandBuffer cmd, VkImage depth);

VkFormat pass_depth_format(void);
VkSampleCountFlagBits pass_raster_samples(void);

#endif /* ENGINE_FRAME_PASS_H */
