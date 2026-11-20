#include "lighting/lighting.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include "vulkan/pipe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

alignas(uint32_t) static const unsigned char k_gbuf_comp_spv[] = {
#embed "lighting_gbuf.comp.spv"
};
alignas(uint32_t) static const unsigned char k_hrc_comp_spv[] = {
#embed "lighting_hrc.comp.spv"
};
alignas(uint32_t) static const unsigned char k_ssilvb_comp_spv[] = {
#embed "lighting_ssilvb.comp.spv"
};

static_assert(sizeof k_gbuf_comp_spv % 4u == 0u, "lighting_gbuf.comp.spv align");
static_assert(sizeof k_hrc_comp_spv % 4u == 0u, "lighting_hrc.comp.spv align");
static_assert(sizeof k_ssilvb_comp_spv % 4u == 0u, "lighting_ssilvb.comp.spv align");

enum : uint32_t {
    k_env_ring_slots   = 4u,
    k_retire_ring_cap  = 4u,
    k_shadow_dim_uma   = 1024u,
    k_shadow_dim_disc  = 2048u,
};

typedef struct LightingComputePush {
    uint64_t env_bda;
    uint32_t cascade_idx;
    uint32_t flags;
} LightingComputePush;

static_assert(sizeof(LightingComputePush) == 16u, "LightingComputePush must be 16 bytes");

typedef struct RetiredGiSlab {
    DaBuffer buf;
    uint64_t retire_point;
    bool     active;
} RetiredGiSlab;

static VulkanContext        *s_vk             = nullptr;
static VkDescriptorSetLayout s_shadow_dsl     = VK_NULL_HANDLE;
static VkPipelineLayout      s_gbuf_layout    = VK_NULL_HANDLE;
static VkPipelineLayout      s_comp_layout    = VK_NULL_HANDLE;
static VkPipeline            s_pipe_gbuf      = VK_NULL_HANDLE;
static VkPipeline            s_pipe_hrc       = VK_NULL_HANDLE;
static VkPipeline            s_pipe_ssilvb    = VK_NULL_HANDLE;

static VkImage               s_shadow_img     = VK_NULL_HANDLE;
static VkDeviceMemory        s_shadow_mem     = VK_NULL_HANDLE;
static VkImageView           s_shadow_view    = VK_NULL_HANDLE;
static VkSampler             s_shadow_sampler = VK_NULL_HANDLE;
static VkImageLayout         s_shadow_layout  = VK_IMAGE_LAYOUT_UNDEFINED;
static uint32_t              s_shadow_dim     = k_shadow_dim_uma;
static bool                  s_shadow_ready   = false;
static bool                  s_shadow_rendered = false;
static LightingShadowBlock   s_shadow_cpu     = {0};

static DaBuffer              s_env_buf        = {0};
static DaBuffer              s_gi_slab        = {0};
static RetiredGiSlab         s_retired_gi[k_retire_ring_cap];
static VkSemaphore           s_acquire_sem    = VK_NULL_HANDLE;
static uint64_t              s_cur_point      = 0u;
static uint64_t              s_gi_last_point  = 0u;

static uint32_t              s_env_slot       = 0u;
static bool                  s_env_ready      = false;

static LightingMatSlot       s_mat_cpu[K_LIGHTING_MAT_SLOTS];
static LightingProxy         s_proxies[K_LIGHTING_MAX_PROXIES];
static uint32_t              s_proxy_count    = 0u;

static bool                  s_gi_user_on     = true;
static bool                  s_gi_frame_live  = false;
static uint32_t              s_oracle_mode    = LIGHTING_ORACLE_ANALYTICAL_UMA;
static uint32_t              s_frame_index    = 0u;
static uint32_t              s_ping_index     = 0u;
static bool                  s_history_valid  = false;
static bool                  s_have_prev_cam  = false;
static bool                  s_logged_init    = false;
static bool                  s_logged_gi      = false;

static uint32_t              s_full_w         = 0u;
static uint32_t              s_full_h         = 0u;
static uint32_t              s_gi_w           = 0u;
static uint32_t              s_gi_h           = 0u;
static uint32_t              s_hrc_w0         = 0u;
static uint32_t              s_hrc_h0         = 0u;
static uint32_t              s_hrc_offsets[4] = {0};
static uint32_t              s_hrc_total_rays = 0u;
static uint32_t              s_ssilvb_slices  = 2u;
static uint32_t              s_ssilvb_steps   = 5u;

static VkDeviceSize          s_off_gbuf       = 0u;
static VkDeviceSize          s_off_hrc        = 0u;
static VkDeviceSize          s_off_gi[2]      = {0u, 0u};

static LightingCameraParams  s_curr_cam       = { .qw = 1.0f, .zoom = 1.0f };
static LightingCameraParams  s_prev_cam       = { .qw = 1.0f, .zoom = 1.0f };

static bool                  s_has_ground     = false;
static float                 s_ground_z       = 0.0f;
static float                 s_ground_alb[3]  = { 0.14f, 0.16f, 0.20f };

static uint64_t              s_terrain_bda    = 0ull;
static float                 s_elev_base_m    = 0.0f;
static float                 s_vert_scale     = 1.0f;
static uint64_t              s_scene_hiz_bda  = 0ull;
static LightingSceneGeom     s_scene_geom     = {0};
static float                 s_scene_bounds[K_LIGHTING_MAX_SCENE_INST][4];

static VkDeviceSize align_up_size(VkDeviceSize v, VkDeviceSize a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static void parse_gi_env(void) {
    const char *env = getenv("ENGINE_GI");
    if (env == nullptr || env[0] == '\0') {
        s_gi_user_on = true;
        return;
    }
    if (strcasecmp(env, "0") == 0 || strcasecmp(env, "off") == 0 ||
        strcasecmp(env, "false") == 0 || strcasecmp(env, "no") == 0) {
        s_gi_user_on = false;
    } else {
        s_gi_user_on = true;
    }
}

static void select_profile_params(const VulkanContext *vk) {
    if (vk != nullptr && vk->device_info.execution_profile != VULKAN_PROFILE_UMA_IGPU) {
        s_oracle_mode   = LIGHTING_ORACLE_DISCRETE_HIGH;
        s_ssilvb_slices = 4u;
        s_ssilvb_steps  = 8u;
        s_shadow_dim    = k_shadow_dim_disc;
    } else {
        s_oracle_mode   = LIGHTING_ORACLE_ANALYTICAL_UMA;
        s_ssilvb_slices = 2u;
        s_ssilvb_steps  = 5u;
        s_shadow_dim    = k_shadow_dim_uma;
    }
}

static void reap_retired_gi_slabs(VulkanContext *vk, bool force_all) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    uint64_t done = 0u;
    bool have_done = false;
    if (!force_all && s_acquire_sem != VK_NULL_HANDLE &&
        vkGetSemaphoreCounterValue(vk->device, s_acquire_sem, &done) == VK_SUCCESS) {
        have_done = true;
    }
    for (uint32_t i = 0u; i < k_retire_ring_cap; ++i) {
        if (!s_retired_gi[i].active) {
            continue;
        }
        if (force_all || s_retired_gi[i].retire_point == 0u ||
            (have_done && done >= s_retired_gi[i].retire_point)) {
            da_buffer_destroy(vk, &s_retired_gi[i].buf);
            s_retired_gi[i] = (RetiredGiSlab){0};
        }
    }
}

static void retire_gi_slab(VulkanContext *vk, DaBuffer *buf, uint64_t last_point) {
    if (vk == nullptr || buf == nullptr || buf->buffer == VK_NULL_HANDLE) {
        return;
    }
    reap_retired_gi_slabs(vk, false);
    for (uint32_t i = 0u; i < k_retire_ring_cap; ++i) {
        if (!s_retired_gi[i].active) {
            s_retired_gi[i] = (RetiredGiSlab){
                .buf          = *buf,
                .retire_point = last_point,
                .active       = true,
            };
            *buf = (DaBuffer){0};
            return;
        }
    }
    // Ring full: wait only on oldest retirement point if timeline is available
    if (s_acquire_sem != VK_NULL_HANDLE && s_retired_gi[0].retire_point > 0u) {
        VkSemaphoreWaitInfo wi = {
            .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1u,
            .pSemaphores    = &s_acquire_sem,
            .pValues        = &s_retired_gi[0].retire_point,
        };
        (void)vkWaitSemaphores(vk->device, &wi, 3000000000ull);
    }
    da_buffer_destroy(vk, &s_retired_gi[0].buf);
    s_retired_gi[0] = (RetiredGiSlab){
        .buf          = *buf,
        .retire_point = last_point,
        .active       = true,
    };
    *buf = (DaBuffer){0};
}

static bool ensure_shadow_resources(void) {
    if (s_shadow_ready && s_shadow_dsl != VK_NULL_HANDLE && s_shadow_view != VK_NULL_HANDLE &&
        s_shadow_sampler != VK_NULL_HANDLE) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }

    if (s_shadow_dsl == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding bind = {
            .binding         = 1u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1u,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
        };
        VkDescriptorSetLayoutCreateInfo dsl_ci = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
            .bindingCount = 1u,
            .pBindings    = &bind,
        };
        if (vkCreateDescriptorSetLayout(s_vk->device, &dsl_ci, nullptr, &s_shadow_dsl) != VK_SUCCESS) {
            s_shadow_dsl = VK_NULL_HANDLE;
            return false;
        }
    }

    if (s_shadow_img == VK_NULL_HANDLE) {
        VkImageCreateInfo img_ci = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = VK_FORMAT_D32_SFLOAT,
            .extent        = { .width = s_shadow_dim, .height = s_shadow_dim, .depth = 1u },
            .mipLevels     = 1u,
            .arrayLayers   = 1u,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .tiling        = VK_IMAGE_TILING_OPTIMAL,
            .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(s_vk->device, &img_ci, nullptr, &s_shadow_img) != VK_SUCCESS) {
            s_shadow_img = VK_NULL_HANDLE;
            return false;
        }

        VkImageMemoryRequirementsInfo2 req_in = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = s_shadow_img,
        };
        VkMemoryRequirements2 req2 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        };
        vkGetImageMemoryRequirements2(s_vk->device, &req_in, &req2);

        uint32_t mem_type = 0u;
        if (vulkan_find_memory_type(s_vk, req2.memoryRequirements.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_type) != VULKAN_SUCCESS) {
            vkDestroyImage(s_vk->device, s_shadow_img, nullptr);
            s_shadow_img = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryPriorityAllocateInfoEXT prio = {0};
        VkMemoryAllocateInfo alloc_ci = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext           = vulkan_chain_memory_priority(s_vk, &prio, 1.0f, nullptr),
            .allocationSize  = req2.memoryRequirements.size,
            .memoryTypeIndex = mem_type,
        };
        if (vkAllocateMemory(s_vk->device, &alloc_ci, nullptr, &s_shadow_mem) != VK_SUCCESS) {
            vkDestroyImage(s_vk->device, s_shadow_img, nullptr);
            s_shadow_img = VK_NULL_HANDLE;
            s_shadow_mem = VK_NULL_HANDLE;
            return false;
        }

        VkBindImageMemoryInfo bind_ci = {
            .sType        = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .image        = s_shadow_img,
            .memory       = s_shadow_mem,
            .memoryOffset = 0u,
        };
        if (vkBindImageMemory2(s_vk->device, 1u, &bind_ci) != VK_SUCCESS) {
            vkFreeMemory(s_vk->device, s_shadow_mem, nullptr);
            vkDestroyImage(s_vk->device, s_shadow_img, nullptr);
            s_shadow_img = VK_NULL_HANDLE;
            s_shadow_mem = VK_NULL_HANDLE;
            return false;
        }

        VkImageViewCreateInfo view_ci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = s_shadow_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel   = 0u,
                .levelCount     = 1u,
                .baseArrayLayer = 0u,
                .layerCount     = 1u,
            },
        };
        if (vkCreateImageView(s_vk->device, &view_ci, nullptr, &s_shadow_view) != VK_SUCCESS) {
            vkFreeMemory(s_vk->device, s_shadow_mem, nullptr);
            vkDestroyImage(s_vk->device, s_shadow_img, nullptr);
            s_shadow_img  = VK_NULL_HANDLE;
            s_shadow_mem  = VK_NULL_HANDLE;
            s_shadow_view = VK_NULL_HANDLE;
            return false;
        }
        s_shadow_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (s_shadow_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samp_ci = {
            .sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter     = VK_FILTER_LINEAR,
            .minFilter     = VK_FILTER_LINEAR,
            .mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .compareEnable = VK_TRUE,
            .compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL,
            .minLod        = 0.0f,
            .maxLod        = 1.0f,
            .borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        };
        if (vkCreateSampler(s_vk->device, &samp_ci, nullptr, &s_shadow_sampler) != VK_SUCCESS) {
            s_shadow_sampler = VK_NULL_HANDLE;
            return false;
        }
    }

    s_shadow_ready = true;
    return true;
}

static VkImageLayout shadow_read_layout(void) {
    return (s_vk != nullptr && s_vk->device_info.supports_unified_image_layouts)
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static VkImageLayout shadow_attach_layout(void) {
    return (s_vk != nullptr && s_vk->device_info.supports_unified_image_layouts)
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
}

static void transition_shadow_image(VkCommandBuffer cmd,
                                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                                    VkImageLayout new_layout) {
    if (cmd == VK_NULL_HANDLE || s_shadow_img == VK_NULL_HANDLE) {
        return;
    }
    VkImageMemoryBarrier2 bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src_stage,
        .srcAccessMask       = src_access,
        .dstStageMask        = dst_stage,
        .dstAccessMask       = dst_access,
        .oldLayout           = s_shadow_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = s_shadow_img,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0u,
            .levelCount     = 1u,
            .baseArrayLayer = 0u,
            .layerCount     = 1u,
        },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1u,
        .pImageMemoryBarriers    = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    s_shadow_layout = new_layout;
}

static bool ensure_env_buffer(void) {
    if (s_env_ready && s_env_buf.buffer != VK_NULL_HANDLE && s_env_buf.map != nullptr) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    const VkDeviceSize total = (VkDeviceSize)k_env_ring_slots * K_LIGHTING_ENV_ALLOC_STRIDE;
    const VkBufferUsageFlags env_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(s_vk, total, env_usage, true, &s_env_buf) != 0 ||
        s_env_buf.map == nullptr || s_env_buf.address == 0ull) {
        return false;
    }
    memset(s_env_buf.map, 0, (size_t)total);
    uint8_t *base = (uint8_t *)s_env_buf.map;
    for (uint32_t i = 0u; i < k_env_ring_slots; ++i) {
        memcpy(base + (size_t)i * K_LIGHTING_ENV_ALLOC_STRIDE, s_mat_cpu, sizeof s_mat_cpu);
    }
    s_env_ready = true;
    return true;
}

static bool ensure_compute_pipelines(void) {
    if (s_pipe_gbuf != VK_NULL_HANDLE &&
        s_pipe_hrc != VK_NULL_HANDLE &&
        s_pipe_ssilvb != VK_NULL_HANDLE) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE || !ensure_shadow_resources()) {
        return false;
    }
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(LightingComputePush),
    };
    if (s_gbuf_layout == VK_NULL_HANDLE) {
        VkPipelineLayoutCreateInfo info = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1u,
            .pSetLayouts            = &s_shadow_dsl,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &range,
        };
        if (vkCreatePipelineLayout(s_vk->device, &info, nullptr, &s_gbuf_layout) != VK_SUCCESS) {
            s_gbuf_layout = VK_NULL_HANDLE;
            return false;
        }
    }
    if (s_comp_layout == VK_NULL_HANDLE) {
        VkPipelineLayoutCreateInfo info = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &range,
        };
        if (vkCreatePipelineLayout(s_vk->device, &info, nullptr, &s_comp_layout) != VK_SUCCESS) {
            s_comp_layout = VK_NULL_HANDLE;
            return false;
        }
    }

    if (s_pipe_gbuf == VK_NULL_HANDLE) {
        s_pipe_gbuf = vulkan_compute_pipeline(s_vk, k_gbuf_comp_spv, sizeof k_gbuf_comp_spv, s_gbuf_layout);
    }
    if (s_pipe_hrc == VK_NULL_HANDLE) {
        s_pipe_hrc = vulkan_compute_pipeline(s_vk, k_hrc_comp_spv, sizeof k_hrc_comp_spv, s_comp_layout);
    }
    if (s_pipe_ssilvb == VK_NULL_HANDLE) {
        s_pipe_ssilvb = vulkan_compute_pipeline(s_vk, k_ssilvb_comp_spv, sizeof k_ssilvb_comp_spv, s_comp_layout);
    }

    return s_pipe_gbuf != VK_NULL_HANDLE &&
           s_pipe_hrc != VK_NULL_HANDLE &&
           s_pipe_ssilvb != VK_NULL_HANDLE;
}

static bool ensure_gi_slab(uint32_t width, uint32_t height) {
    if (s_vk == nullptr || width == 0u || height == 0u) {
        return false;
    }

    uint32_t target_gi_w  = 0u;
    uint32_t target_gi_h  = 0u;
    uint32_t target_hrc_w = 0u;
    uint32_t target_hrc_h = 0u;
    uint32_t hrc_offsets[K_LIGHTING_MAX_CASCADES] = {0};
    const uint32_t ray_off = lighting_compute_hrc_rays(width, height, s_oracle_mode,
                                                       &target_gi_w, &target_gi_h,
                                                       &target_hrc_w, &target_hrc_h,
                                                       hrc_offsets);

    if (s_gi_slab.buffer != VK_NULL_HANDLE &&
        s_full_w == width && s_full_h == height &&
        s_gi_w == target_gi_w && s_gi_h == target_gi_h &&
        s_hrc_w0 == target_hrc_w && s_hrc_h0 == target_hrc_h) {
        return true;
    }

    for (uint32_t c = 0u; c < K_LIGHTING_MAX_CASCADES; ++c) {
        s_hrc_offsets[c] = hrc_offsets[c];
    }

    const VkDeviceSize gbuf_bytes = (VkDeviceSize)target_gi_w * target_gi_h * sizeof(LightingGbufTexel);
    const VkDeviceSize hrc_bytes  = (VkDeviceSize)ray_off * 8u;
    const VkDeviceSize gi_bytes   = (VkDeviceSize)target_gi_w * target_gi_h * sizeof(LightingGiTexel);

    VkDeviceSize cursor = 0u;
    s_off_gbuf  = cursor;
    cursor      = align_up_size(cursor + gbuf_bytes, 256u);
    s_off_hrc   = cursor;
    cursor      = align_up_size(cursor + hrc_bytes,  256u);
    s_off_gi[0] = cursor;
    cursor      = align_up_size(cursor + gi_bytes,   256u);
    s_off_gi[1] = cursor;
    cursor      = align_up_size(cursor + gi_bytes,   256u);

    if (s_gi_slab.buffer != VK_NULL_HANDLE) {
        retire_gi_slab(s_vk, &s_gi_slab, s_gi_last_point);
    }

    const VkBufferUsageFlags gi_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(s_vk, cursor, gi_usage, false, &s_gi_slab) != 0 ||
        s_gi_slab.address == 0ull) {
        da_buffer_destroy(s_vk, &s_gi_slab);
        return false;
    }

    s_full_w         = width;
    s_full_h         = height;
    s_gi_w           = target_gi_w;
    s_gi_h           = target_gi_h;
    s_hrc_w0         = target_hrc_w;
    s_hrc_h0         = target_hrc_h;
    s_hrc_total_rays = ray_off;
    s_history_valid  = false;
    s_ping_index     = 0u;

    if (!s_logged_gi) {
        printf("[Lighting] 2026 SSILVB (%ux%u, %u slices x %u steps, 10x10 halo denoise) + "
               "4-Level Split HRC (%ux%u C0 probes, %u rays) + %ux%u D32 Shadow Map "
               "(oracle=%s, %.2f MiB BDA)\n",
               s_gi_w, s_gi_h, s_ssilvb_slices, s_ssilvb_steps,
               s_hrc_w0, s_hrc_h0, s_hrc_total_rays,
               s_shadow_dim, s_shadow_dim,
               s_oracle_mode == LIGHTING_ORACLE_DISCRETE_HIGH ? "Discrete-High" : "UMA-Analytical+DEM",
               (double)cursor / (1024.0 * 1024.0));
        fflush(stdout);
        s_logged_gi = true;
    }
    return true;
}

static void cmd_compute_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dst_stage,
                                VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = dst_stage,
        .dstAccessMask = dst_access,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void populate_default_studio_light(LightingShadowBlock *sb) {
    // Warm sunlit key light (~47 deg elevation, angled 3/4 from camera)
    const float sx = 0.38f, sy = 0.56f, sz = 0.735f;
    const float inv_l = 1.0f / sqrtf(sx * sx + sy * sy + sz * sz);
    sb->sun_dir[0]     = sx * inv_l;
    sb->sun_dir[1]     = sy * inv_l;
    sb->sun_dir[2]     = sz * inv_l;
    sb->inv_shadow_res = 1.0f / (float)s_shadow_dim;

    sb->sun_color[0]   = 2.95f;
    sb->sun_color[1]   = 2.72f;
    sb->sun_color[2]   = 2.38f;
    sb->persp          = s_curr_cam.persp;

    sb->sky_top[0]     = 0.34f;
    sb->sky_top[1]     = 0.52f;
    sb->sky_top[2]     = 0.84f;
    sb->prev_persp     = s_prev_cam.persp;

    sb->sky_horizon[0] = 0.62f;
    sb->sky_horizon[1] = 0.70f;
    sb->sky_horizon[2] = 0.80f;
    sb->normal_bias    = 2.0f;

    sb->sky_ground[0]  = 0.16f;
    sb->sky_ground[1]  = 0.19f;
    sb->sky_ground[2]  = 0.25f;
    sb->shadow_enabled = 0u;
}

static void v3_cross_f(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void v3_norm_f(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1.0e-7f) {
        float inv = 1.0f / l;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

static float v3_dot_f(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

int lighting_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (!lighting_self_check()) {
        fprintf(stderr, "[Lighting Error] CPU mathematical & 32-bit bitmask self-check failed!\n");
        return -1;
    }
    s_vk = vk;
    memset(s_mat_cpu, 0, sizeof s_mat_cpu);
    memset(s_retired_gi, 0, sizeof s_retired_gi);
    parse_gi_env();
    select_profile_params(vk);
    (void)ensure_shadow_resources();
    lighting_reset();

    if (!s_logged_init) {
        printf("[Lighting] Core PBR+AgX+Shadow+GI kernel armed (GI=%s, Shadow=%ux%u D32_SFLOAT, RayQuery=%s)\n",
               s_gi_user_on ? "on" : "off",
               s_shadow_dim, s_shadow_dim,
               vk->device_info.supports_ray_query ? "supported" : "analytical");
        fflush(stdout);
        s_logged_init = true;
    }
    return 0;
}

void lighting_shutdown(VulkanContext *vk) {
    VulkanContext *ctx = vk != nullptr ? vk : s_vk;
    if (ctx != nullptr && ctx->device != VK_NULL_HANDLE) {
        reap_retired_gi_slabs(ctx, true);
        vulkan_pipe_release(ctx, s_pipe_gbuf);
        vulkan_pipe_release(ctx, s_pipe_hrc);
        vulkan_pipe_release(ctx, s_pipe_ssilvb);
        s_pipe_gbuf   = VK_NULL_HANDLE;
        s_pipe_hrc    = VK_NULL_HANDLE;
        s_pipe_ssilvb = VK_NULL_HANDLE;

        if (s_gbuf_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_gbuf_layout, nullptr);
            s_gbuf_layout = VK_NULL_HANDLE;
        }
        if (s_comp_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_comp_layout, nullptr);
            s_comp_layout = VK_NULL_HANDLE;
        }
        if (s_shadow_sampler != VK_NULL_HANDLE) {
            vkDestroySampler(ctx->device, s_shadow_sampler, nullptr);
            s_shadow_sampler = VK_NULL_HANDLE;
        }
        if (s_shadow_view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device, s_shadow_view, nullptr);
            s_shadow_view = VK_NULL_HANDLE;
        }
        if (s_shadow_img != VK_NULL_HANDLE) {
            vkDestroyImage(ctx->device, s_shadow_img, nullptr);
            s_shadow_img = VK_NULL_HANDLE;
        }
        if (s_shadow_mem != VK_NULL_HANDLE) {
            vkFreeMemory(ctx->device, s_shadow_mem, nullptr);
            s_shadow_mem = VK_NULL_HANDLE;
        }
        if (s_shadow_dsl != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx->device, s_shadow_dsl, nullptr);
            s_shadow_dsl = VK_NULL_HANDLE;
        }
        da_buffer_destroy(ctx, &s_gi_slab);
        da_buffer_destroy(ctx, &s_env_buf);
    }
    s_shadow_layout   = VK_IMAGE_LAYOUT_UNDEFINED;
    s_shadow_ready    = false;
    s_shadow_rendered = false;
    s_env_ready       = false;
    s_gi_frame_live   = false;
    s_history_valid   = false;
    s_acquire_sem     = VK_NULL_HANDLE;
    s_cur_point       = 0u;
    s_gi_last_point   = 0u;
    s_vk              = nullptr;
}

void lighting_invalidate_history(void) {
    s_history_valid = false;
    s_have_prev_cam = false;
}

void lighting_reset(void) {
    memset(s_mat_cpu, 0, sizeof s_mat_cpu);
    if (s_env_ready && s_env_buf.map != nullptr) {
        memset(s_env_buf.map, 0, (size_t)k_env_ring_slots * K_LIGHTING_ENV_ALLOC_STRIDE);
    }
    s_proxy_count     = 0u;
    s_has_ground      = false;
    s_terrain_bda     = 0ull;
    s_scene_hiz_bda   = 0ull;
    s_gi_frame_live   = false;
    s_shadow_rendered = false;
    lighting_invalidate_history();
}

void lighting_material_set(uint32_t slot, float emit_r, float emit_g, float emit_b, float glow) {
    if (slot >= K_LIGHTING_MAT_SLOTS) {
        return;
    }
    s_mat_cpu[slot] = (LightingMatSlot){
        .emit_r = emit_r,
        .emit_g = emit_g,
        .emit_b = emit_b,
        .glow   = glow,
    };
    const bool non_zero = (glow > 0.0f) && (emit_r > 0.0f || emit_g > 0.0f || emit_b > 0.0f);
    if (!s_env_ready && non_zero) {
        (void)ensure_env_buffer();
    }
    if (s_env_ready && s_env_buf.map != nullptr) {
        uint8_t *base = (uint8_t *)s_env_buf.map;
        for (uint32_t i = 0u; i < k_env_ring_slots; ++i) {
            uint8_t *slice = base + (size_t)i * K_LIGHTING_ENV_ALLOC_STRIDE;
            memcpy(slice + (size_t)slot * sizeof(LightingMatSlot),
                   &s_mat_cpu[slot], sizeof(LightingMatSlot));
        }
    }
}

void lighting_material_get_emissive(uint32_t slot, float out_emit[3]) {
    if (out_emit == nullptr) {
        return;
    }
    if (slot >= K_LIGHTING_MAT_SLOTS) {
        out_emit[0] = 0.0f;
        out_emit[1] = 0.0f;
        out_emit[2] = 0.0f;
        return;
    }
    float g = s_mat_cpu[slot].glow;
    out_emit[0] = s_mat_cpu[slot].emit_r * g;
    out_emit[1] = s_mat_cpu[slot].emit_g * g;
    out_emit[2] = s_mat_cpu[slot].emit_b * g;
}

VkDeviceAddress lighting_env_bda(void) {
    if (!s_env_ready || s_env_buf.address == 0ull) {
        return 0ull;
    }
    return s_env_buf.address + (VkDeviceAddress)s_env_slot * K_LIGHTING_ENV_ALLOC_STRIDE;
}

void lighting_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    s_acquire_sem = acquire;
    s_cur_point   = signal_point;
    if (s_vk != nullptr) {
        reap_retired_gi_slabs(s_vk, false);
    }
}

void lighting_begin_frame(uint32_t width, uint32_t height, const LightingCameraParams *cam) {
    s_proxy_count     = 0u;
    s_has_ground      = false;
    s_terrain_bda     = 0ull;
    s_scene_hiz_bda   = 0ull;
    s_scene_geom      = (LightingSceneGeom){0};
    s_gi_frame_live   = false;
    s_shadow_rendered = false;

    if (width == 0u || height == 0u || cam == nullptr) {
        return;
    }
    if (!ensure_env_buffer()) {
        return;
    }

    s_env_slot = (s_env_slot + 1u) % k_env_ring_slots;
    if (s_have_prev_cam) {
        s_prev_cam = s_curr_cam;
    } else {
        s_prev_cam      = *cam;
        s_have_prev_cam = true;
    }
    s_curr_cam = *cam;
    populate_default_studio_light(&s_shadow_cpu);
}

void lighting_set_ground_plane(bool enabled, float z_height, const float base_albedo[3]) {
    s_has_ground = enabled;
    s_ground_z   = z_height;
    if (base_albedo != nullptr) {
        s_ground_alb[0] = base_albedo[0];
        s_ground_alb[1] = base_albedo[1];
        s_ground_alb[2] = base_albedo[2];
    }
}

void lighting_set_terrain_field(uint64_t terrain_dir_bda, float elev_base_m, float vertical_scale) {
    s_terrain_bda = terrain_dir_bda;
    s_elev_base_m = elev_base_m;
    s_vert_scale  = vertical_scale;
}

void lighting_set_scene_hiz(uint64_t scene_hiz_bda) {
    s_scene_hiz_bda = scene_hiz_bda;
}

void lighting_set_scene_geom(uint64_t meshlets_bda,
                             uint64_t vertices_bda,
                             uint64_t triangles_bda,
                             uint64_t instances_bda,
                             uint32_t inst_count,
                             const float (*inst_bounds)[4]) {
    if (meshlets_bda == 0ull || vertices_bda == 0ull || triangles_bda == 0ull ||
        instances_bda == 0ull || inst_count == 0u || inst_bounds == nullptr) {
        s_scene_geom = (LightingSceneGeom){0};
        return;
    }
    uint32_t clamped = inst_count <= K_LIGHTING_MAX_SCENE_INST ? inst_count : K_LIGHTING_MAX_SCENE_INST;
    s_scene_geom = (LightingSceneGeom){
        .meshlets_bda  = meshlets_bda,
        .vertices_bda  = vertices_bda,
        .triangles_bda = triangles_bda,
        .instances_bda = instances_bda,
        .bounds_bda    = 0ull,
        .inst_count    = clamped,
        .pad0          = 0u,
        .pad1          = 0ull,
    };
    memcpy(s_scene_bounds, inst_bounds, (size_t)clamped * sizeof(s_scene_bounds[0]));
}

void lighting_submit_proxy(const LightingProxy *proxy) {
    if (proxy == nullptr || s_proxy_count >= K_LIGHTING_MAX_PROXIES) {
        return;
    }
    LightingProxy p = *proxy;
    if (p.bound_radius_f16 == 0u) {
        float ex = fmaxf(p.extents[0], 1.0e-4f);
        float ey = fmaxf(p.extents[1], 1.0e-4f);
        float ez = fmaxf(p.extents[2], 1.0e-4f);
        float br = (p.kind == LIGHTING_PROXY_OBB)
            ? (sqrtf(ex * ex + ey * ey + ez * ez) * 1.015f)
            : (fmaxf(ex, fmaxf(ey, ez)) * 1.015f);
        uint16_t sp = lighting_f32_to_f16(sinf(p.pitch_rad));
        uint16_t cp = lighting_f32_to_f16(cosf(p.pitch_rad));
        uint16_t sy = lighting_f32_to_f16(sinf(p.yaw_rad));
        uint16_t cy = lighting_f32_to_f16(cosf(p.yaw_rad));
        p.bound_radius_f16 = lighting_f32_to_f16(br);
        p.trig_pitch       = (uint32_t)sp | ((uint32_t)cp << 16u);
        p.trig_yaw         = (uint32_t)sy | ((uint32_t)cy << 16u);
    }
    s_proxies[s_proxy_count++] = p;
}

VkDescriptorSetLayout lighting_shadow_set_layout(void) {
    (void)ensure_shadow_resources();
    return s_shadow_dsl;
}

void lighting_shadow_get_descriptor(VkImageView *out_view, VkSampler *out_sampler,
                                    VkImageLayout *out_layout) {
    (void)ensure_shadow_resources();
    if (out_view != nullptr)    *out_view    = s_shadow_view;
    if (out_sampler != nullptr) *out_sampler = s_shadow_sampler;
    if (out_layout != nullptr)  *out_layout  = shadow_read_layout();
}

void lighting_bind_shadow(VkCommandBuffer cmd, VkPipelineBindPoint bind_point,
                          VkPipelineLayout layout) {
    if (cmd == VK_NULL_HANDLE || layout == VK_NULL_HANDLE || s_vk == nullptr ||
        !ensure_shadow_resources()) {
        return;
    }
    VkDescriptorImageInfo img_info = {
        .sampler     = s_shadow_sampler,
        .imageView   = s_shadow_view,
        .imageLayout = shadow_read_layout(),
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = VK_NULL_HANDLE,
        .dstBinding      = 1u,
        .dstArrayElement = 0u,
        .descriptorCount = 1u,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &img_info,
    };
    vulkan_cmd_push_descriptors(s_vk, cmd, bind_point, layout, 0u, 1u, &write);
}

bool lighting_shadow_begin(VkCommandBuffer cmd, float out_light_clip_vp[16]) {
    s_shadow_rendered = false;
    if (cmd == VK_NULL_HANDLE || out_light_clip_vp == nullptr || s_vk == nullptr ||
        s_terrain_bda != 0ull || (s_proxy_count == 0u && s_scene_geom.inst_count == 0u) ||
        !ensure_shadow_resources()) {
        return false;
    }

    populate_default_studio_light(&s_shadow_cpu);
    const float fwd[3] = {
        -s_shadow_cpu.sun_dir[0],
        -s_shadow_cpu.sun_dir[1],
        -s_shadow_cpu.sun_dir[2],
    };
    const float z_up[3] = { 0.0f, 0.0f, 1.0f };
    float right[3];
    float up[3];
    v3_cross_f(fwd, z_up, right);
    v3_norm_f(right);
    v3_cross_f(right, fwd, up);
    v3_norm_f(up);

    // Compute tight light-space bounds across all 3D proxies & scene instances
    float lx_min =  1.0e30f, lx_max = -1.0e30f;
    float ly_min =  1.0e30f, ly_max = -1.0e30f;
    float lz_min =  1.0e30f, lz_max = -1.0e30f;

    for (uint32_t i = 0u; i < s_proxy_count; ++i) {
        const float *c = s_proxies[i].center;
        float r = lighting_f16_to_f32(s_proxies[i].bound_radius_f16);
        if (r < 1.0f) {
            r = 120.0f;
        }
        float lx = v3_dot_f(c, right);
        float ly = v3_dot_f(c, up);
        float lz = v3_dot_f(c, fwd);
        if (lx - r < lx_min) lx_min = lx - r;
        if (lx + r > lx_max) lx_max = lx + r;
        if (ly - r < ly_min) ly_min = ly - r;
        if (ly + r > ly_max) ly_max = ly + r;
        if (lz - r < lz_min) lz_min = lz - r;
        if (lz + r > lz_max) lz_max = lz + r;
    }
    for (uint32_t i = 0u; i < s_scene_geom.inst_count; ++i) {
        const float *b = s_scene_bounds[i];
        float r = fmaxf(b[3], 1.0f);
        float lx = v3_dot_f(b, right);
        float ly = v3_dot_f(b, up);
        float lz = v3_dot_f(b, fwd);
        if (lx - r < lx_min) lx_min = lx - r;
        if (lx + r > lx_max) lx_max = lx + r;
        if (ly - r < ly_min) ly_min = ly - r;
        if (ly + r > ly_max) ly_max = ly + r;
        if (lz - r < lz_min) lz_min = lz - r;
        if (lz + r > lz_max) lz_max = lz + r;
    }

    // Pad slightly so cast shadows on z = 0 floor stay fully inside frustum
    float cx = 0.5f * (lx_min + lx_max);
    float cy = 0.5f * (ly_min + ly_max);
    float half_x = 0.5f * (lx_max - lx_min) + 220.0f;
    float half_y = 0.5f * (ly_max - ly_min) + 220.0f;
    float half_xy = fmaxf(half_x, half_y);
    if (half_xy < 480.0f)  half_xy = 480.0f;
    if (half_xy > 2200.0f) half_xy = 2200.0f;

    // Texel-snap light-space center to eliminate shadow edge shimmer during motion
    float texel_world = (2.0f * half_xy) / (float)s_shadow_dim;
    cx = floorf(cx / texel_world + 0.5f) * texel_world;
    cy = floorf(cy / texel_world + 0.5f) * texel_world;

    float z_min = lz_min - 800.0f;
    float z_max = lz_max + 900.0f;
    float inv_xy = 1.0f / half_xy;
    float inv_z  = 1.0f / fmaxf(z_max - z_min, 1.0f);

    // Row-major clip matrix:
    //   ndc.x =  dot(P, right) * inv_xy - cx * inv_xy
    //   ndc.y = -dot(P, up)    * inv_xy + cy * inv_xy
    //   ndc.z =  dot(P, fwd)   * inv_z  - z_min * inv_z
    //   ndc.w =  1.0
    out_light_clip_vp[0]  =  right[0] * inv_xy;
    out_light_clip_vp[1]  =  right[1] * inv_xy;
    out_light_clip_vp[2]  =  right[2] * inv_xy;
    out_light_clip_vp[3]  = -cx * inv_xy;

    out_light_clip_vp[4]  = -up[0] * inv_xy;
    out_light_clip_vp[5]  = -up[1] * inv_xy;
    out_light_clip_vp[6]  = -up[2] * inv_xy;
    out_light_clip_vp[7]  =  cy * inv_xy;

    out_light_clip_vp[8]  =  fwd[0] * inv_z;
    out_light_clip_vp[9]  =  fwd[1] * inv_z;
    out_light_clip_vp[10] =  fwd[2] * inv_z;
    out_light_clip_vp[11] = -z_min * inv_z;

    out_light_clip_vp[12] = 0.0f;
    out_light_clip_vp[13] = 0.0f;
    out_light_clip_vp[14] = 0.0f;
    out_light_clip_vp[15] = 1.0f;

    // Pre-biased UV+Depth shadow sampling matrix: u = 0.5*ndc.x + 0.5, v = 0.5*ndc.y + 0.5, d = ndc.z
    float *uv_m = s_shadow_cpu.light_vp;
    uv_m[0]  = 0.5f * out_light_clip_vp[0];
    uv_m[1]  = 0.5f * out_light_clip_vp[1];
    uv_m[2]  = 0.5f * out_light_clip_vp[2];
    uv_m[3]  = 0.5f * out_light_clip_vp[3] + 0.5f;

    uv_m[4]  = 0.5f * out_light_clip_vp[4];
    uv_m[5]  = 0.5f * out_light_clip_vp[5];
    uv_m[6]  = 0.5f * out_light_clip_vp[6];
    uv_m[7]  = 0.5f * out_light_clip_vp[7] + 0.5f;

    uv_m[8]  = out_light_clip_vp[8];
    uv_m[9]  = out_light_clip_vp[9];
    uv_m[10] = out_light_clip_vp[10];
    uv_m[11] = out_light_clip_vp[11];

    uv_m[12] = 0.0f;
    uv_m[13] = 0.0f;
    uv_m[14] = 0.0f;
    uv_m[15] = 1.0f;

    s_shadow_cpu.normal_bias    = texel_world * 1.45f;
    s_shadow_cpu.shadow_enabled = 1u;

    transition_shadow_image(
        cmd,
        s_shadow_layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_2_NONE
            : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
        s_shadow_layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_ACCESS_2_NONE
            : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        shadow_attach_layout());

    VkRenderingAttachmentInfo depth_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = s_shadow_view,
        .imageLayout = shadow_attach_layout(),
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .depthStencil = { .depth = 1.0f, .stencil = 0u } },
    };
    VkRenderingInfo ri = {
        .sType            = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea       = { .offset = { 0, 0 }, .extent = { s_shadow_dim, s_shadow_dim } },
        .layerCount       = 1u,
        .pDepthAttachment = &depth_att,
    };
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float)s_shadow_dim,
        .height   = (float)s_shadow_dim,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = { .offset = { 0, 0 }, .extent = { s_shadow_dim, s_shadow_dim } };
    vkCmdSetViewport(cmd, 0u, 1u, &vp);
    vkCmdSetScissor(cmd, 0u, 1u, &sc);
    vkCmdSetDepthBias(cmd, 1.35f, 0.0f, 1.85f);

    s_shadow_rendered = true;
    return true;
}

void lighting_shadow_end(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || !s_shadow_rendered) {
        return;
    }
    vkCmdEndRendering(cmd);
    transition_shadow_image(
        cmd,
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        shadow_read_layout());
}

void lighting_prepare(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (cmd == VK_NULL_HANDLE) {
        return;
    }

    // Ensure shadow image is always in a valid sampled layout even on 2D or shadow-free frames
    if (ensure_shadow_resources() && s_shadow_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        transition_shadow_image(
            cmd,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            shadow_read_layout());
    }

    if (!s_env_ready || s_env_buf.map == nullptr || width == 0u || height == 0u) {
        return;
    }

    uint8_t       *slice     = (uint8_t *)s_env_buf.map + (size_t)s_env_slot * K_LIGHTING_ENV_ALLOC_STRIDE;
    VkDeviceAddress slice_bda = lighting_env_bda();

    // Always synchronize material emission look slots into the active ring slice
    memcpy(slice, s_mat_cpu, sizeof s_mat_cpu);

    const bool has_3d_content = (s_proxy_count > 0u) || (s_scene_geom.inst_count > 0u) || (s_terrain_bda != 0ull);
    const bool run_gi         = s_gi_user_on && has_3d_content &&
                                ensure_compute_pipelines() && ensure_gi_slab(width, height);

    const uint32_t curr_ping = s_ping_index & 1u;
    const uint32_t prev_ping = (curr_ping ^ 1u);

    float px_world  = 1.0f / fmaxf(s_curr_cam.zoom, 1.0e-4f);
    float thick_w   = (s_terrain_bda != 0ull) ? fmaxf(px_world * 14.0f, 18.0f)
                                              : fmaxf(px_world * 20.0f, 32.0f);
    float gi_boost  = (s_terrain_bda != 0ull) ? 0.92f : 1.15f;
    float ao_str    = (s_terrain_bda != 0ull) ? 0.85f : 0.90f;
    float radius_px = (s_oracle_mode == LIGHTING_ORACLE_DISCRETE_HIGH) ? 48.0f : 38.0f;

    uint32_t scene_flags = 0u;
    if (s_shadow_rendered) {
        scene_flags |= K_LIGHTING_FLAG_HAS_SHADOW;
    }
    if (run_gi) {
        scene_flags |= K_LIGHTING_FLAG_FILT_IN_GBUF;
    }

    LightingFrameHeader hdr = {
        .magic            = K_LIGHTING_MAGIC,
        .gi_enabled       = run_gi ? 1u : 0u,
        .oracle_mode      = s_oracle_mode,
        .frame_index      = s_frame_index,
        .full_width       = width,
        .full_height      = height,
        .gi_width         = run_gi ? s_gi_w : 0u,
        .gi_height        = run_gi ? s_gi_h : 0u,
        .hrc_w0           = run_gi ? s_hrc_w0 : 0u,
        .hrc_h0           = run_gi ? s_hrc_h0 : 0u,
        .proxy_count      = s_proxy_count,
        .has_ground       = s_has_ground ? 1u : 0u,
        .ground_z         = s_ground_z,
        .ground_r         = s_ground_alb[0],
        .ground_g         = s_ground_alb[1],
        .ground_b         = s_ground_alb[2],
        .proxies_bda      = s_proxy_count > 0u ? (slice_bda + K_LIGHTING_PROXIES_OFFSET) : 0ull,
        .gbuf_bda         = run_gi ? (s_gi_slab.address + s_off_gbuf) : 0ull,
        .hrc_bda          = run_gi ? (s_gi_slab.address + s_off_hrc)  : 0ull,
        .gi_curr_bda      = run_gi ? (s_gi_slab.address + s_off_gi[curr_ping]) : 0ull,
        .gi_prev_bda      = (run_gi && s_history_valid) ? (s_gi_slab.address + s_off_gi[prev_ping]) : 0ull,
        .terrain_dir_bda  = s_terrain_bda,
        .scene_hiz_bda    = s_scene_hiz_bda,
        .elev_base_m      = s_elev_base_m,
        .vertical_scale   = s_vert_scale,
        .history_valid    = (run_gi && s_history_valid) ? 1u : 0u,
        .ssilvb_slices    = s_ssilvb_slices,
        .ssilvb_steps     = s_ssilvb_steps,
        .hrc_cascades     = K_LIGHTING_MAX_CASCADES,
        .qx               = s_curr_cam.qx,
        .qy               = s_curr_cam.qy,
        .qz               = s_curr_cam.qz,
        .qw               = s_curr_cam.qw,
        .cam_x            = s_curr_cam.cam_x,
        .cam_y            = s_curr_cam.cam_y,
        .cam_z            = s_curr_cam.cam_z,
        .zoom             = s_curr_cam.zoom,
        .prev_qx          = s_prev_cam.qx,
        .prev_qy          = s_prev_cam.qy,
        .prev_qz          = s_prev_cam.qz,
        .prev_qw          = s_prev_cam.qw,
        .prev_cam_x       = s_prev_cam.cam_x,
        .prev_cam_y       = s_prev_cam.cam_y,
        .prev_cam_z       = s_prev_cam.cam_z,
        .prev_zoom        = s_prev_cam.zoom,
        .gi_intensity     = gi_boost,
        .ao_strength      = ao_str,
        .ssilvb_radius_px = radius_px,
        .thickness_world  = thick_w,
        .hrc_offset_rays  = {
            s_hrc_offsets[0], s_hrc_offsets[1], s_hrc_offsets[2], s_hrc_offsets[3],
        },
        .scene_geom_bda   = (s_scene_geom.inst_count > 0u) ? (slice_bda + K_LIGHTING_SCENE_OFFSET) : 0ull,
        .scene_inst_count = s_scene_geom.inst_count,
        .scene_flags      = scene_flags,
    };

    s_shadow_cpu.persp          = s_curr_cam.persp;
    s_shadow_cpu.prev_persp     = s_prev_cam.persp;
    s_shadow_cpu.shadow_enabled = s_shadow_rendered ? 1u : 0u;

    memcpy(slice + K_LIGHTING_HEADER_OFFSET, &hdr, sizeof hdr);
    memcpy(slice + K_LIGHTING_SHADOW_OFFSET, &s_shadow_cpu, sizeof s_shadow_cpu);
    if (s_proxy_count > 0u) {
        memcpy(slice + K_LIGHTING_PROXIES_OFFSET,
               s_proxies, (size_t)s_proxy_count * sizeof(LightingProxy));
    }
    if (s_scene_geom.inst_count > 0u) {
        LightingSceneGeom sg = s_scene_geom;
        sg.bounds_bda = slice_bda + K_LIGHTING_BOUNDS_OFFSET;
        memcpy(slice + K_LIGHTING_SCENE_OFFSET, &sg, sizeof sg);
        memcpy(slice + K_LIGHTING_BOUNDS_OFFSET, s_scene_bounds,
               (size_t)s_scene_geom.inst_count * sizeof(s_scene_bounds[0]));
    }

    if (!run_gi) {
        s_gi_frame_live = false;
        s_history_valid = false;
        return;
    }

    // 1. Primary GI G-Buffer + Direct/Multi-Bounce Radiance Compute Pass (with hardware shadow map)
    LightingComputePush push = {
        .env_bda     = slice_bda,
        .cascade_idx = 0u,
        .flags       = 0u,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_gbuf);
    lighting_bind_shadow(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gbuf_layout);
    vulkan_cmd_push(cmd, s_gbuf_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, (s_gi_w + 7u) / 8u, (s_gi_h + 7u) / 8u, 1u);

    cmd_compute_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 2. 4-Level Split / Holographic Radiance Cascades (coarse c=3 -> fine c=0)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_hrc);
    for (int c = (int)K_LIGHTING_MAX_CASCADES - 1; c >= 0; --c) {
        uint32_t uc    = (uint32_t)c;
        uint32_t cw    = s_hrc_w0 >> uc;
        uint32_t ch    = s_hrc_h0 >> uc;
        if (cw == 0u) cw = 1u;
        if (ch == 0u) ch = 1u;
        uint32_t m_cnt = 4u << uc;
        uint32_t tx    = cw * m_cnt;

        push.cascade_idx = uc;
        vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
        vkCmdDispatch(cmd, (tx + 7u) / 8u, (ch + 7u) / 8u, 1u);

        cmd_compute_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // 3a. 2026 SSILVB (32-Bit Visibility Bitmask) + HRC C0 Fusion + Temporal Accumulation -> gi_curr_bda
    push.cascade_idx = 0u;
    push.flags       = 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_ssilvb);
    vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, (s_gi_w + 7u) / 8u, (s_gi_h + 7u) / 8u, 1u);

    cmd_compute_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 3b. Seamless 10x10-halo SLM cross-bilateral spatial denoise: gi_curr_bda -> gbuf_bda (0 extra VRAM, 0 tile seams)
    push.flags = 1u;
    vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, (s_gi_w + 7u) / 8u, (s_gi_h + 7u) / 8u, 1u);

    // Publish filtered LightingGiTexel grid to all downstream surface fragment shaders
    cmd_compute_barrier(cmd,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    s_gi_frame_live = true;
    s_history_valid = true;
    s_gi_last_point = s_cur_point;
    s_ping_index    = prev_ping;
    s_frame_index  += 1u;
}

bool lighting_toggle_gi(void) {
    s_gi_user_on = !s_gi_user_on;
    if (!s_gi_user_on) {
        s_history_valid = false;
        s_gi_frame_live = false;
    }
    printf("[Lighting] Global Illumination (SSILVB + HRC): %s\n",
           s_gi_user_on ? "ON" : "OFF");
    fflush(stdout);
    return true;
}

void lighting_get_stats(LightingBenchStats *out_stats) {
    if (out_stats == nullptr) {
        return;
    }
    uint64_t bda_total = 0ull;
    if (s_env_ready) {
        bda_total += (uint64_t)s_env_buf.bytes;
    }
    if (s_gi_slab.buffer != VK_NULL_HANDLE) {
        bda_total += (uint64_t)s_gi_slab.bytes;
    }
    *out_stats = (LightingBenchStats){
        .gi_enabled     = s_gi_user_on && s_gi_frame_live,
        .oracle_mode    = s_oracle_mode,
        .gi_width       = s_gi_frame_live ? s_gi_w : 0u,
        .gi_height      = s_gi_frame_live ? s_gi_h : 0u,
        .hrc_probes_c0  = s_gi_frame_live ? (s_hrc_w0 * s_hrc_h0) : 0u,
        .hrc_total_rays = s_gi_frame_live ? s_hrc_total_rays : 0u,
        .ssilvb_slices  = s_ssilvb_slices,
        .ssilvb_steps   = s_ssilvb_steps,
        .active_proxies = s_proxy_count,
        .bda_bytes      = bda_total,
    };
}
