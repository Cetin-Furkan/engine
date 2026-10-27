#include "frame/material.h"
#include "vulkan/bda.h"
#include <stdio.h>
#include <string.h>

// Two vec4s. The shader reads e[slot * 2]. The second stays zero until a
// later look needs it. 64 slots is 2 KiB, one host-visible allocation.

typedef struct MatSlot {
    float emit_r;
    float emit_g;
    float emit_b;
    float glow;
    float reserved_r;
    float reserved_g;
    float reserved_b;
    float reserved_a;
} MatSlot;

static_assert(sizeof(MatSlot) == 32u, "material slot");
static_assert(k_mat_solid0 + 6u <= k_mat_face0, "solid slots");
static_assert(k_mat_face0 + k_mat_faces <= k_mat_mesh, "face slots");
static_assert(k_mat_mesh < k_mat_cube_more, "mesh slot");
static_assert(k_mat_cube_more < k_mat_none, "further cube looks");

static DaBuffer        s_buf;
static VkDeviceAddress s_addr = 0;
static MatSlot        *s_map = nullptr;
static bool            s_logged = false;

static const float k_emit_r = 0.62f;
static const float k_emit_g = 0.38f;
static const float k_emit_b = 0.14f;

void material_shutdown(VulkanContext *vk) {
    s_map = nullptr;
    s_addr = 0;
    da_buffer_destroy(vk, &s_buf);
}

int material_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_buf.buffer != VK_NULL_HANDLE) {
        return 0;
    }
    VkDeviceSize bytes = (VkDeviceSize)k_mat_slots * sizeof(MatSlot);
    if (da_buffer_create(vk, bytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, &s_buf) != 0) {
        return -1;
    }
    s_map = s_buf.map;
    s_addr = s_buf.address;
    if (s_map == nullptr || s_addr == 0) {
        fprintf(stderr, "[Material Error] Look table device address is null\n");
        material_shutdown(vk);
        return -1;
    }
    memset(s_map, 0, (size_t)bytes);
    if (!s_logged) {
        printf("[Material] %u records. G opens glow on the selection, or the cube.\n", k_mat_slots);
        fflush(stdout);
        s_logged = true;
    }
    return 0;
}

VkDeviceAddress material_address(void) {
    return s_addr;
}

void material_set(uint32_t slot, bool glow) {
    if (s_map == nullptr || slot >= k_mat_slots) {
        return;
    }
    MatSlot *rec = &s_map[slot];
    if (glow) {
        rec->emit_r = k_emit_r;
        rec->emit_g = k_emit_g;
        rec->emit_b = k_emit_b;
        rec->glow = 1.0f;
    } else {
        rec->emit_r = 0.0f;
        rec->emit_g = 0.0f;
        rec->emit_b = 0.0f;
        rec->glow = 0.0f;
    }
}
