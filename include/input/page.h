#ifndef ENGINE_INPUT_PAGE_H
#define ENGINE_INPUT_PAGE_H

#include "vulkan/types.h"
#include <stdint.h>

// Lock-free 4-slot cache-line-aligned host-coherent BDA input snapshot ring.
// Any GPU shader can read live pointer/keyboard state directly via BDA without
// injecting per-frame compute dispatches or HOST<->COMPUTE pipeline stalls.

[[nodiscard]] int input_gpu_init(VulkanContext *vk);
void input_gpu_shutdown(VulkanContext *vk);
void input_page_harvest(void);
[[nodiscard]] uint32_t input_page_seen(void);

#endif /* ENGINE_INPUT_PAGE_H */
