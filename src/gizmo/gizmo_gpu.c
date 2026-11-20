#include "gizmo/gizmo.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/pipe.h"
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_gizmo_vert_spv[] = {
#embed "gizmo.vert.spv"
};
alignas(uint32_t) static const unsigned char k_gizmo_frag_spv[] = {
#embed "gizmo.frag.spv"
};

static_assert(sizeof k_gizmo_vert_spv % 4u == 0u, "gizmo.vert.spv align");
static_assert(sizeof k_gizmo_frag_spv % 4u == 0u, "gizmo.frag.spv align");

enum : uint32_t {
    k_gizmo_max_verts = 16384u,
};

typedef struct GizmoPush {
    uint64_t verts_bda;
    uint32_t width;
    uint32_t height;
    uint8_t  view_bytes[48];
} GizmoPush;

static_assert(offsetof(GizmoPush, view_bytes) == 16u, "GizmoPush view offset");
static_assert(sizeof(GizmoPush) == 64u, "GizmoPush must be 64 bytes");

static VulkanContext   *s_vk        = nullptr;
static VkPipelineLayout s_layout    = VK_NULL_HANDLE;
static VulkanGfxLive    s_gfx_srgb  = {0};
static VulkanGfxLive    s_gfx_unorm = {0};
static bool             s_ready     = false;

static bool ensure_ready(void) {
    if (s_ready) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    VkPushConstantRange pr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0u,
        .size       = sizeof(GizmoPush),
    };
    VkPipelineLayoutCreateInfo li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &pr,
    };
    if (vkCreatePipelineLayout(s_vk->device, &li, nullptr, &s_layout) != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        return false;
    }
    VulkanGfxPipe pipe = {
        .vert          = k_gizmo_vert_spv,
        .vert_bytes    = sizeof k_gizmo_vert_spv,
        .frag          = k_gizmo_frag_spv,
        .frag_bytes    = sizeof k_gizmo_frag_spv,
        .layout        = s_layout,
        .depth_format  = VK_FORMAT_UNDEFINED,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_FALSE,
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_ALWAYS,
        .blend         = VK_TRUE,
    };
    vulkan_gfx_arm(&s_gfx_srgb,  &pipe, VK_FORMAT_B8G8R8A8_SRGB,  nullptr);
    vulkan_gfx_arm(&s_gfx_unorm, &pipe, VK_FORMAT_B8G8R8A8_UNORM, nullptr);
    s_ready = true;
    return true;
}

int gizmo_gpu_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk    = vk;
    s_ready = false;
    return 0;
}

void gizmo_gpu_shutdown(VulkanContext *vk) {
    VulkanContext *dev_vk = vk != nullptr ? vk : s_vk;
    vulkan_gfx_disarm(dev_vk, &s_gfx_srgb);
    vulkan_gfx_disarm(dev_vk, &s_gfx_unorm);
    if (dev_vk != nullptr && dev_vk->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev_vk->device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
    s_ready  = false;
    s_vk     = nullptr;
}

void gizmo_gpu_draw(VkCommandBuffer cmd,
                    uint32_t width,
                    uint32_t height,
                    VkFormat color_format,
                    const void *view_push_ptr,
                    size_t view_push_bytes,
                    const GizmoState *st,
                    const EcsWorld *w,
                    EcsEntity selected,
                    const GizmoCamera *cam) {
    if (cmd == VK_NULL_HANDLE || s_vk == nullptr || width == 0u || height == 0u ||
        view_push_ptr == nullptr || view_push_bytes != 48u || st == nullptr ||
        w == nullptr || cam == nullptr) {
        return;
    }
    if (!st->debug_draw && (st->mode == GIZMO_MODE_NONE || !ecs_alive(w, selected))) {
        return;
    }
    if (!ensure_ready()) {
        return;
    }

    static GizmoVertex s_scratch[k_gizmo_max_verts];
    uint32_t vcount = gizmo_build_vertices(st, w, selected, cam, s_scratch, k_gizmo_max_verts);
    if (vcount < 3u) {
        return;
    }

    BdaSlice slice = bda_alloc_transient(s_vk, (VkDeviceSize)vcount * sizeof(GizmoVertex), 16u);
    if (slice.cpu == nullptr || slice.gpu == 0ull) {
        return;
    }
    memcpy(slice.cpu, s_scratch, (size_t)vcount * sizeof(GizmoVertex));

    VulkanGfxLive *gfx = (color_format == VK_FORMAT_B8G8R8A8_UNORM) ? &s_gfx_unorm : &s_gfx_srgb;
    if (!vulkan_gfx_bind(s_vk, cmd, gfx)) {
        return;
    }

    GizmoPush push = {
        .verts_bda = slice.gpu,
        .width     = width,
        .height    = height,
    };
    memcpy(push.view_bytes, view_push_ptr, 48u);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof push, &push);
    vkCmdDraw(cmd, vcount, 1u, 0u, 0u);
}
