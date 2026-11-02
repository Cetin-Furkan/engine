#ifndef ENGINE_CHROME_GPU_H
#define ENGINE_CHROME_GPU_H

#include "vulkan/types.h"
#include <stdint.h>

// The bar, its controls, and a close glyph are filled by a fragment shader
// over each layer rectangle. Hit tests stay on the CPU.

[[nodiscard]] int chrome_gpu_init(VulkanContext *vk);
void chrome_gpu_shutdown(VulkanContext *vk);

// The flat pipelines exist at init. The scene pair matches the 3D pass
// (sample count and depth format, depth test off) and is built once the
// pass format is known. Chrome init runs before that pass exists.
[[nodiscard]] int chrome_gpu_bind_pass(VulkanContext *vk, VkFormat depth_format,
                                       VkSampleCountFlagBits samples);

// Records one quad per visible layer into the open rendering pass.
// window_w/window_h are the layout the spec was measured in. origin is the
// top-left of this framebuffer inside that window, and fb_w/fb_h are the
// framebuffer. A layer that misses the framebuffer is skipped. format is
// the view format. depth_format and samples select the scene pipelines;
// zero selects the flat 1-sample pair.
void chrome_gpu_draw(VulkanContext *vk, VkCommandBuffer cmd,
                     uint32_t window_w, uint32_t window_h,
                     int32_t origin_x, int32_t origin_y,
                     uint32_t fb_w, uint32_t fb_h, VkFormat format,
                     VkFormat depth_format, VkSampleCountFlagBits samples);

#endif /* ENGINE_CHROME_GPU_H */
