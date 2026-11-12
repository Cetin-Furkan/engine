#include "frame/material.h"
#include "lighting/lighting.h"
#include <stdio.h>

// Unified look table backed by the Core Engine LightingEnvBlock BDA ring
// (offset 0..2047 = LightingMatSlot[64], binary-identical to shaders/material.slang).

static_assert((uint32_t)k_mat_slots == (uint32_t)K_LIGHTING_MAT_SLOTS, "material slot count");
static_assert(k_mat_solid0 + 6u <= k_mat_face0, "solid slots");
static_assert(k_mat_face0 + k_mat_faces <= k_mat_mesh, "face slots");
static_assert(k_mat_mesh < k_mat_cube_more, "mesh slot");
static_assert(k_mat_cube_more < k_mat_none, "further cube looks");

static const float k_emit_r = 0.62f;
static const float k_emit_g = 0.38f;
static const float k_emit_b = 0.14f;
static bool        s_logged = false;

void material_shutdown(VulkanContext *vk) {
    (void)vk;
    s_logged = false;
}

int material_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    return 0;
}

VkDeviceAddress material_address(void) {
    return lighting_env_bda();
}

void material_set(uint32_t slot, bool glow) {
    if (slot >= k_mat_slots) {
        return;
    }
    if (glow && !s_logged) {
        printf("[Material] %u records. G opens glow on the selection, or the cube.\n", k_mat_slots);
        fflush(stdout);
        s_logged = true;
    }
    if (glow) {
        lighting_material_set(slot, k_emit_r, k_emit_g, k_emit_b, 1.0f);
    } else {
        lighting_material_set(slot, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}
