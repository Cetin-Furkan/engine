#ifndef ENGINE_CHROME_GPU_H
#define ENGINE_CHROME_GPU_H

#include "vulkan/types.h"
#include <stdint.h>

// The bar, its controls, and a close glyph are filled by a fragment shader
// over each layer rectangle. Hit tests stay on the CPU.

[[nodiscard]] int chrome_gpu_init(VulkanContext *vk);
void chrome_gpu_shutdown(VulkanContext *vk);

// Records one quad per visible layer into an open color-only rendering pass.
// format is the view format, B8G8R8A8_SRGB or B8G8R8A8_UNORM.
void chrome_gpu_draw(VulkanContext *vk, VkCommandBuffer cmd,
                     uint32_t width, uint32_t height, VkFormat format);

#endif /* ENGINE_CHROME_GPU_H */
