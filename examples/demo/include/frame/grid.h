#ifndef ENGINE_FRAME_GRID_H
#define ENGINE_FRAME_GRID_H

#include "vulkan/types.h"
#include <stdint.h>

// One n^3 lattice in the device-address arena. Cells are fp16. n is a push
// constant and a multiple of the 8x8 tile. The cube picture uses 128, so the
// ball stays the size it had. A live frame steps the field, then it holds.
// The vertex shader samples the copy this frame writes. The mesh is not
// involved. Each fillet's own heat still wins where that fillet is hot.

[[nodiscard]] int grid_init(VulkanContext *vk);
void grid_shutdown(VulkanContext *vk);
void grid_record(VkCommandBuffer cmd, uint32_t dt_ms);
void grid_record_ok(void);
void grid_record_abort(void);
void grid_hold(void);
// A new heat pulse may step the field again after it has held.
void grid_wake(void);
// Edge length of the cube picture. Zero until the lattice exists.
[[nodiscard]] uint32_t grid_n(void);
// The copy the vertex shader must read this frame. Zero until it exists.
[[nodiscard]] uint64_t grid_field_addr(void);

#endif /* ENGINE_FRAME_GRID_H */
