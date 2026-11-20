#include "scene_gpu_int.h"
#include "lighting/lighting.h"
#include "texture/texture.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SceneGpuState g_scene;

alignas(uint32_t) static const unsigned char k_cull_comp_spv[] = {
#embed "scene_cull.comp.spv"
};
alignas(uint32_t) static const unsigned char k_hiz_comp_spv[] = {
#embed "scene_hiz.comp.spv"
};
alignas(uint32_t) static const unsigned char k_visbuf_vert_spv[] = {
#embed "scene_visbuf.vert.spv"
};
alignas(uint32_t) static const unsigned char k_visbuf_frag_spv[] = {
#embed "scene_visbuf.frag.spv"
};
alignas(uint32_t) static const unsigned char k_resolve_vert_spv[] = {
#embed "scene_visbuf_resolve.vert.spv"
};
alignas(uint32_t) static const unsigned char k_resolve_frag_spv[] = {
#embed "scene_visbuf_resolve.frag.spv"
};
alignas(uint32_t) static const unsigned char k_meshlet_vert_spv[] = {
#embed "scene_meshlet.vert.spv"
};
alignas(uint32_t) static const unsigned char k_meshlet_frag_spv[] = {
#embed "scene_meshlet.frag.spv"
};
alignas(uint32_t) static const unsigned char k_shadow_vert_spv[] = {
#embed "scene_shadow.vert.spv"
};

static_assert(sizeof k_cull_comp_spv % 4u == 0u, "scene_cull.comp.spv align");
static_assert(sizeof k_hiz_comp_spv % 4u == 0u, "scene_hiz.comp.spv align");
static_assert(sizeof k_visbuf_vert_spv % 4u == 0u, "scene_visbuf.vert.spv align");
static_assert(sizeof k_visbuf_frag_spv % 4u == 0u, "scene_visbuf.frag.spv align");
static_assert(sizeof k_resolve_vert_spv % 4u == 0u, "scene_visbuf_resolve.vert.spv align");
static_assert(sizeof k_resolve_frag_spv % 4u == 0u, "scene_visbuf_resolve.frag.spv align");
static_assert(sizeof k_meshlet_vert_spv % 4u == 0u, "scene_meshlet.vert.spv align");
static_assert(sizeof k_meshlet_frag_spv % 4u == 0u, "scene_meshlet.frag.spv align");
static_assert(sizeof k_shadow_vert_spv % 4u == 0u, "scene_shadow.vert.spv align");

static bool env_contains_ci(const char *hay, const char *needle) {
    if (hay == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    const size_t nlen = strlen(needle);
    for (size_t i = 0u; hay[i] != '\0'; ++i) {
        size_t j = 0u;
        while (j < nlen && hay[i + j] != '\0' &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

int scene_init(VulkanContext *vk, VkFormat color_format,
               VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || depth_format == VK_FORMAT_UNDEFINED) {
        return -1;
    }
    if (!scene_self_check()) {
        fprintf(stderr, "[Scene Error] Startup mathematical self-check failed\n");
        return -1;
    }

    g_scene.vk             = vk;
    g_scene.pass_color_fmt = color_format != VK_FORMAT_UNDEFINED ? color_format : VK_FORMAT_B8G8R8A8_SRGB;
    g_scene.pass_depth_fmt = depth_format;
    g_scene.pass_samples   = samples == 0 ? VK_SAMPLE_COUNT_1_BIT : samples;
    g_scene.can_hiz_push   = vk->device_info.supports_push_descriptors &&
                             vk->dispatch.vkCmdPushDescriptorSet != nullptr;

    g_scene.visbuf_enabled = (g_scene.pass_samples == VK_SAMPLE_COUNT_1_BIT) && g_scene.can_hiz_push;
    const char *env_vb = getenv("ENGINE_VISBUF");
    if (env_vb != nullptr && env_vb[0] != '\0' && !env_contains_ci(env_vb, "auto")) {
        if (strcmp(env_vb, "1") == 0 || env_contains_ci(env_vb, "on") || env_contains_ci(env_vb, "true")) {
            g_scene.visbuf_enabled = g_scene.can_hiz_push;
        } else if (strcmp(env_vb, "0") == 0 || env_contains_ci(env_vb, "off") || env_contains_ci(env_vb, "false")) {
            g_scene.visbuf_enabled = false;
        }
    }

    g_scene.hiz_policy = SCENE_HIZ_AUTO;
    const char *env_hiz = getenv("ENGINE_HIZ");
    if (env_hiz != nullptr && env_hiz[0] != '\0' && !env_contains_ci(env_hiz, "auto")) {
        if (strcmp(env_hiz, "1") == 0 || env_contains_ci(env_hiz, "on") || env_contains_ci(env_hiz, "true")) {
            g_scene.hiz_policy = SCENE_HIZ_ON;
        } else if (strcmp(env_hiz, "0") == 0 || env_contains_ci(env_hiz, "off") || env_contains_ci(env_hiz, "false")) {
            g_scene.hiz_policy = SCENE_HIZ_OFF;
        }
    }

    g_scene.ready               = true;
    g_scene.gpu_ready           = false;
    g_scene.hiz_header_dirty    = true;
    g_scene.used_hiz_this_frame = false;
    return 0;
}

bool scene_ensure_gpu_ready(void) {
    if (g_scene.gpu_ready) {
        return true;
    }
    if (!g_scene.ready || g_scene.vk == nullptr || g_scene.vk->device == VK_NULL_HANDLE) {
        return false;
    }
    VulkanContext *vk = g_scene.vk;

    if (da_buffer_create(vk, K_SCENE_SLAB_TOTAL_BYTES,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         false, &g_scene.geom_slab) != 0) {
        return false;
    }

    if (da_buffer_create(vk, (VkDeviceSize)K_SCENE_CULL_RING_SLOTS * sizeof(SceneCullSlot),
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                         true, &g_scene.cull_buf) != 0 || g_scene.cull_buf.map == nullptr) {
        return false;
    }
    memset(g_scene.cull_buf.map, 0, (size_t)K_SCENE_CULL_RING_SLOTS * sizeof(SceneCullSlot));

    if (g_scene.can_hiz_push) {
        VkDescriptorSetLayoutBinding tex_binding = {
            .binding         = 0u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1u,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        VkDescriptorSetLayoutCreateInfo dsl_info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
            .bindingCount = 1u,
            .pBindings    = &tex_binding,
        };
        if (vkCreateDescriptorSetLayout(vk->device, &dsl_info, nullptr, &g_scene.tex_set_layout) != VK_SUCCESS) {
            g_scene.tex_set_layout = VK_NULL_HANDLE;
            g_scene.can_hiz_push   = false;
            g_scene.visbuf_enabled = false;
        }
    }

    // 1. Two-Phase Meshlet Cull Compute Pipeline
    VkPushConstantRange cull_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(SceneCullPush),
    };
    VkPipelineLayoutCreateInfo cull_li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &cull_range,
    };
    if (vkCreatePipelineLayout(vk->device, &cull_li, nullptr, &g_scene.cull_layout) != VK_SUCCESS) {
        return false;
    }
    g_scene.cull_pipe = vulkan_compute_pipeline(vk, k_cull_comp_spv, sizeof k_cull_comp_spv, g_scene.cull_layout);
    if (g_scene.cull_pipe == VK_NULL_HANDLE) {
        return false;
    }

    // 2. Subgroup Hi-Z Pyramid Compute Pipeline
    if (g_scene.can_hiz_push) {
        VkPushConstantRange hiz_range = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0u,
            .size       = sizeof(SceneHizPush),
        };
        VkPipelineLayoutCreateInfo hiz_li = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1u,
            .pSetLayouts            = &g_scene.tex_set_layout,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &hiz_range,
        };
        if (vkCreatePipelineLayout(vk->device, &hiz_li, nullptr, &g_scene.hiz_layout) != VK_SUCCESS) {
            return false;
        }
        g_scene.hiz_pipe = vulkan_compute_pipeline(vk, k_hiz_comp_spv, sizeof k_hiz_comp_spv, g_scene.hiz_layout);
        if (g_scene.hiz_pipe == VK_NULL_HANDLE) {
            return false;
        }
    }

    // 3. Draw Pipeline Layouts (VisBuf + Bindless@1, Meshlet + Shadow@0 + Bindless@1, Shadow Map)
    (void)texture_ensure_ready();
    VkDescriptorSetLayout bindless_dsl = texture_bindless_set_layout();

    VkPushConstantRange draw_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0u,
        .size       = sizeof(SceneDrawPush),
    };
    VkDescriptorSetLayout draw_sets[2] = { g_scene.tex_set_layout, bindless_dsl };
    uint32_t draw_set_count = (g_scene.tex_set_layout != VK_NULL_HANDLE && bindless_dsl != VK_NULL_HANDLE) ? 2u : 0u;
    VkPipelineLayoutCreateInfo draw_li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = draw_set_count,
        .pSetLayouts            = draw_set_count > 0u ? draw_sets : nullptr,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &draw_range,
    };
    if (vkCreatePipelineLayout(vk->device, &draw_li, nullptr, &g_scene.draw_layout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetLayout shadow_dsl = lighting_shadow_set_layout();
    VkDescriptorSetLayout ml_sets[2] = { shadow_dsl, bindless_dsl };
    uint32_t ml_set_count = 0u;
    if (shadow_dsl != VK_NULL_HANDLE) {
        ml_set_count = (bindless_dsl != VK_NULL_HANDLE) ? 2u : 1u;
    }
    VkPipelineLayoutCreateInfo ml_li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = ml_set_count,
        .pSetLayouts            = ml_set_count > 0u ? ml_sets : nullptr,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &draw_range,
    };
    if (vkCreatePipelineLayout(vk->device, &ml_li, nullptr, &g_scene.meshlet_layout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange sh_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0u,
        .size       = sizeof(SceneShadowPush),
    };
    VkPipelineLayoutCreateInfo sh_li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &sh_range,
    };
    if (vkCreatePipelineLayout(vk->device, &sh_li, nullptr, &g_scene.shadow_layout) != VK_SUCCESS) {
        return false;
    }

    VulkanGfxPipe shadow_cfg = {
        .vert              = k_shadow_vert_spv,
        .vert_bytes        = sizeof k_shadow_vert_spv,
        .frag              = nullptr,
        .frag_bytes        = 0u,
        .layout            = g_scene.shadow_layout,
        .depth_format      = VK_FORMAT_D32_SFLOAT,
        .samples           = VK_SAMPLE_COUNT_1_BIT,
        .cull              = VK_CULL_MODE_NONE,
        .depth_test        = VK_TRUE,
        .depth_write       = VK_TRUE,
        .depth_compare     = VK_COMPARE_OP_LESS_OR_EQUAL,
        .blend             = VK_FALSE,
        .depth_bias_enable = true,
    };
    vulkan_gfx_arm(&g_scene.shadow_gfx, &shadow_cfg, VK_FORMAT_UNDEFINED, nullptr);

    VulkanGfxPipe depth_only_cfg = {
        .vert          = k_visbuf_vert_spv,
        .vert_bytes    = sizeof k_visbuf_vert_spv,
        .frag          = nullptr,
        .frag_bytes    = 0u,
        .layout        = g_scene.draw_layout,
        .depth_format  = VK_FORMAT_D32_SFLOAT,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&g_scene.depth_only_gfx, &depth_only_cfg, VK_FORMAT_UNDEFINED, nullptr);

    VulkanGfxPipe visbuf_cfg = {
        .vert          = k_visbuf_vert_spv,
        .vert_bytes    = sizeof k_visbuf_vert_spv,
        .frag          = k_visbuf_frag_spv,
        .frag_bytes    = sizeof k_visbuf_frag_spv,
        .layout        = g_scene.draw_layout,
        .depth_format  = VK_FORMAT_D32_SFLOAT,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&g_scene.visbuf_gfx, &visbuf_cfg, VK_FORMAT_R32_UINT, nullptr);

    VulkanGfxPipe meshlet_cfg = {
        .vert          = k_meshlet_vert_spv,
        .vert_bytes    = sizeof k_meshlet_vert_spv,
        .frag          = k_meshlet_frag_spv,
        .frag_bytes    = sizeof k_meshlet_frag_spv,
        .layout        = g_scene.meshlet_layout,
        .depth_format  = g_scene.pass_depth_fmt,
        .samples       = g_scene.pass_samples,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&g_scene.meshlet_gfx, &meshlet_cfg, g_scene.pass_color_fmt, nullptr);

    VulkanGfxPipe blend_cfg = meshlet_cfg;
    blend_cfg.cull        = VK_CULL_MODE_NONE;
    blend_cfg.depth_write = VK_FALSE;
    blend_cfg.blend       = VK_TRUE;
    vulkan_gfx_arm(&g_scene.meshlet_blend_gfx, &blend_cfg, g_scene.pass_color_fmt, nullptr);

    // 4. Fullscreen R32_UINT Visibility Buffer Analytical Resolve Pipeline
    if (g_scene.can_hiz_push) {
        VkDescriptorSetLayoutBinding res_binds[2] = {
            {
                .binding         = 0u,
                .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount = 1u,
                .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
            {
                .binding         = 1u,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1u,
                .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        };
        VkDescriptorSetLayoutCreateInfo res_dsl_ci = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
            .bindingCount = 2u,
            .pBindings    = res_binds,
        };
        if (vkCreateDescriptorSetLayout(vk->device, &res_dsl_ci, nullptr, &g_scene.resolve_set_layout) != VK_SUCCESS) {
            return false;
        }
        VkDescriptorSetLayout res_sets[2] = { g_scene.resolve_set_layout, bindless_dsl };
        uint32_t res_set_count = bindless_dsl != VK_NULL_HANDLE ? 2u : 1u;
        VkPipelineLayoutCreateInfo res_li = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = res_set_count,
            .pSetLayouts            = res_sets,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &draw_range,
        };
        if (vkCreatePipelineLayout(vk->device, &res_li, nullptr, &g_scene.resolve_layout) != VK_SUCCESS) {
            return false;
        }
        VulkanGfxPipe resolve_cfg = {
            .vert          = k_resolve_vert_spv,
            .vert_bytes    = sizeof k_resolve_vert_spv,
            .frag          = k_resolve_frag_spv,
            .frag_bytes    = sizeof k_resolve_frag_spv,
            .layout        = g_scene.resolve_layout,
            .depth_format  = g_scene.pass_depth_fmt,
            .samples       = g_scene.pass_samples,
            .cull          = VK_CULL_MODE_NONE,
            .depth_test    = VK_TRUE,
            .depth_write   = VK_TRUE,
            .depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL,
            .blend         = VK_FALSE,
        };
        vulkan_gfx_arm(&g_scene.resolve_gfx, &resolve_cfg, g_scene.pass_color_fmt, nullptr);
    }

    g_scene.gpu_ready = true;
    const char *hiz_mode_str = !g_scene.can_hiz_push ? "Frustum+Cone"
        : (g_scene.hiz_policy == SCENE_HIZ_ON ? "Two-Phase (Forced)"
        : (g_scene.hiz_policy == SCENE_HIZ_OFF ? "Frustum+Cone (ENGINE_HIZ=0)" : "Adaptive Two-Phase"));
    printf("[Scene] Unified 64v/124t quantized PBR meshlet kernel armed (32B/vert, Subgroup Hi-Z=%s, VisBuf=%s, Y toggles).\n",
           hiz_mode_str,
           g_scene.visbuf_enabled ? "ON (R32_UINT)" : "OFF (Hardware MSAA Indirect)");
    fflush(stdout);
    return true;
}

void scene_shutdown(VulkanContext *vk) {
    VulkanContext *dev_vk = vk != nullptr ? vk : g_scene.vk;
    vulkan_gfx_disarm(dev_vk, &g_scene.depth_only_gfx);
    vulkan_gfx_disarm(dev_vk, &g_scene.visbuf_gfx);
    vulkan_gfx_disarm(dev_vk, &g_scene.resolve_gfx);
    vulkan_gfx_disarm(dev_vk, &g_scene.meshlet_gfx);
    vulkan_gfx_disarm(dev_vk, &g_scene.meshlet_blend_gfx);
    vulkan_gfx_disarm(dev_vk, &g_scene.shadow_gfx);

    if (dev_vk != nullptr && dev_vk->device != VK_NULL_HANDLE) {
        if (g_scene.cull_pipe != VK_NULL_HANDLE) vulkan_pipe_release(dev_vk, g_scene.cull_pipe);
        if (g_scene.hiz_pipe != VK_NULL_HANDLE) vulkan_pipe_release(dev_vk, g_scene.hiz_pipe);
        if (g_scene.shadow_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev_vk->device, g_scene.shadow_layout, nullptr);
        if (g_scene.meshlet_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev_vk->device, g_scene.meshlet_layout, nullptr);
        if (g_scene.resolve_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev_vk->device, g_scene.resolve_layout, nullptr);
        if (g_scene.draw_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev_vk->device, g_scene.draw_layout, nullptr);
        if (g_scene.hiz_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev_vk->device, g_scene.hiz_layout, nullptr);
        if (g_scene.cull_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev_vk->device, g_scene.cull_layout, nullptr);
        if (g_scene.resolve_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev_vk->device, g_scene.resolve_set_layout, nullptr);
        if (g_scene.tex_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev_vk->device, g_scene.tex_set_layout, nullptr);
    }

    scene_uploads_reap(dev_vk, true);
    scene_targets_destroy(dev_vk);
    da_buffer_destroy(dev_vk, &g_scene.cull_buf);
    da_buffer_destroy(dev_vk, &g_scene.geom_slab);

    g_scene = (SceneGpuState){0};
}
