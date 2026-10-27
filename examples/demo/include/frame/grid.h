#ifndef ENGINE_FRAME_GRID_H
#define ENGINE_FRAME_GRID_H

#include "vulkan/types.h"
#include <stdint.h>

// 128^3 diffusion in one device-address arena. A live frame steps it, then
// the field holds and the frame callback can drop. The cube vertex shader
// reads one sample per vertex. Heat still wins where the fillet is hot.

[[nodiscard]] int grid_init(VulkanContext *vk);
void grid_shutdown(VulkanContext *vk);
void grid_record(VkCommandBuffer cmd, uint32_t dt_ms);
void grid_record_ok(void);
void grid_record_abort(void);
void grid_hold(void);
// A new heat pulse may step the field again after it has held.
void grid_wake(void);
[[nodiscard]] uint64_t grid_tint_addr(void);

#endif /* ENGINE_FRAME_GRID_H */
