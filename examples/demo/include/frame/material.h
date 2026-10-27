#ifndef ENGINE_FRAME_MATERIAL_H
#define ENGINE_FRAME_MATERIAL_H

#include "vulkan/types.h"
#include <stdint.h>

// One table every mesh reads by device address. A slot is a look, not a
// shader. G writes the slot. The next draw uses it. Slot 0 is the first
// cube. Slots 40..62 are further cubes. 1..6 are the six solids, 7..38
// are the faces, 39 is the OBJ mesh. Slot 63 stays closed for corner
// marks that have no object of their own.

enum {
    k_mat_slots     = 64u,
    k_mat_cube      = 0u,
    k_mat_solid0    = 1u,
    k_mat_face0     = 7u,
    k_mat_faces     = 32u,
    k_mat_mesh      = 39u,
    k_mat_cube_more = 40u,
    k_mat_none      = 63u,
};

[[nodiscard]] int material_init(VulkanContext *vk);
void material_shutdown(VulkanContext *vk);

VkDeviceAddress material_address(void);

// glow writes an emission the fragment adds after the sun. A closed slot
// is zero, so the same shader draws both.
void material_set(uint32_t slot, bool glow);

#endif /* ENGINE_FRAME_MATERIAL_H */
