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
// to the 1x linear HDR target (or dma-buf if HDR is unused).
typedef struct PassCoverage {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} PassCoverage;

// 1x Linear HDR scene target (B10G11R11_UFLOAT_PACK32 or R16G16B16A16_SFLOAT).
// Receives 1x draws directly or hardware MSAA resolve from PassCoverage,
// then feeds Bloom + Histogram + Auto-Exposure + AgX Post-Composite.
typedef struct PassHdr {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} PassHdr;

[[nodiscard]] int pass_init(VulkanContext *vk);
void pass_shutdown(VulkanContext *vk);

[[nodiscard]] int pass_depth_create(VulkanContext *vk, PassDepth *depth,
                                    uint32_t width, uint32_t height);
void pass_depth_destroy(VulkanContext *vk, PassDepth *depth);

[[nodiscard]] int pass_coverage_create(VulkanContext *vk, PassCoverage *coverage,
                                       uint32_t width, uint32_t height);
void pass_coverage_destroy(VulkanContext *vk, PassCoverage *coverage);

[[nodiscard]] int pass_hdr_create(VulkanContext *vk, PassHdr *hdr,
                                  uint32_t width, uint32_t height);
void pass_hdr_destroy(VulkanContext *vk, PassHdr *hdr);

VkFormat pass_depth_format(void);
VkFormat pass_hdr_format(void);
VkSampleCountFlagBits pass_raster_samples(void);
void pass_pool_stats(uint64_t *out_depth_bytes, uint64_t *out_cover_bytes);

#endif /* ENGINE_FRAME_PASS_H */
