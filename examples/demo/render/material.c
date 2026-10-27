#include "frame/material.h"
#include "vulkan/memory.h"
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

static VkBuffer        s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory  s_memory = VK_NULL_HANDLE;
static VkDeviceAddress s_addr = 0;
static MatSlot        *s_map = nullptr;
static bool            s_logged = false;

static const float k_emit_r = 0.62f;
static const float k_emit_g = 0.38f;
static const float k_emit_b = 0.14f;

static void material_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Material Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

void material_shutdown(VulkanContext *vk) {
    s_map = nullptr;
    s_addr = 0;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_buffer = VK_NULL_HANDLE;
        s_memory = VK_NULL_HANDLE;
        return;
    }
    if (s_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, s_memory, nullptr);
        s_memory = VK_NULL_HANDLE;
    }
    if (s_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, s_buffer, nullptr);
        s_buffer = VK_NULL_HANDLE;
    }
}

int material_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_buffer != VK_NULL_HANDLE) {
        return 0;
    }
    VkDeviceSize bytes = (VkDeviceSize)k_mat_slots * sizeof(MatSlot);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = bytes,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &s_buffer);
    if (res != VK_SUCCESS) {
        s_buffer = VK_NULL_HANDLE;
        material_fail("vkCreateBuffer", res);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetBufferMemoryRequirements(vk->device, s_buffer, &reqs);
    uint32_t type_index = 0;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, host, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        fprintf(stderr, "[Material Error] No host-visible memory for the look table\n");
        material_shutdown(vk);
        return -1;
    }
    VkMemoryAllocateFlagsInfo alloc_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &alloc_flags,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &s_memory);
    if (res != VK_SUCCESS) {
        material_fail("vkAllocateMemory", res);
        material_shutdown(vk);
        return -1;
    }
    res = vkBindBufferMemory(vk->device, s_buffer, s_memory, 0);
    if (res != VK_SUCCESS) {
        material_fail("vkBindBufferMemory", res);
        material_shutdown(vk);
        return -1;
    }
    void *mapped = nullptr;
    res = vkMapMemory(vk->device, s_memory, 0, reqs.size, 0, &mapped);
    if (res != VK_SUCCESS || mapped == nullptr) {
        material_fail("vkMapMemory", res);
        material_shutdown(vk);
        return -1;
    }
    s_map = mapped;
    memset(s_map, 0, (size_t)bytes);
    VkBufferDeviceAddressInfo address_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = s_buffer,
    };
    s_addr = vkGetBufferDeviceAddress(vk->device, &address_info);
    if (s_addr == 0) {
        fprintf(stderr, "[Material Error] Look table device address is null\n");
        material_shutdown(vk);
        return -1;
    }
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
