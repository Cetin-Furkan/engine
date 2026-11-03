#ifndef ENGINE_INPUT_PAGE_H
#define ENGINE_INPUT_PAGE_H

#include "vulkan/types.h"
#include <stdint.h>

// One host-coherent cache line the seat writes, and a second line a scene
// dispatch fills with the sequence it actually loaded. The host reads that
// word after the device is idle. A later sample does not need its own submit.

[[nodiscard]] int input_gpu_init(VulkanContext *vk);
void input_gpu_shutdown(VulkanContext *vk);
void input_page_record(VkCommandBuffer cmd);
void input_page_harvest(void);
[[nodiscard]] uint32_t input_page_seen(void);

#endif /* ENGINE_INPUT_PAGE_H */
