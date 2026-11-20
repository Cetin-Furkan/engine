#include "post/post.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include "vulkan/pipe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_down_spv[] = {
#embed "post_bloom_down.comp.spv"
};
alignas(uint32_t) static const unsigned char k_exp_spv[] = {
#embed "post_exposure.comp.spv"
};
alignas(uint32_t) static const unsigned char k_up_spv[] = {
#embed "post_bloom_up.comp.spv"
};
alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "plane.vert.spv"
};
alignas(uint32_t) static const unsigned char k_comp_spv[] = {
#embed "post_composite.frag.spv"
};

static_assert(sizeof k_down_spv % 4u == 0u, "post down spv");
static_assert(sizeof k_exp_spv  % 4u == 0u, "post exp spv");
static_assert(sizeof k_up_spv   % 4u == 0u, "post up spv");
static_assert(sizeof k_vert_spv % 4u == 0u, "post vert spv");
static_assert(sizeof k_comp_spv % 4u == 0u, "post comp spv");

enum : uint32_t {
    k_down_mips      = 6u,
    k_up_mips        = 5u,
    k_retire_cap     = 8u,
    k_hist_bytes     = 256u * sizeof(uint32_t),
    k_exp_buf_bytes  = 2048u, // [0..1023] histogram, [1024..1087] PostExposureState
};

typedef struct PostDownPush {
    uint64_t hist_bda;
    uint64_t exp_bda;
    uint32_t src_w, src_h;
    uint32_t m0_w, m0_h;
    uint32_t m1_w, m1_h;
    uint32_t m2_w, m2_h;
    uint32_t m3_w, m3_h;
    uint32_t m4_w, m4_h;
    uint32_t m5_w, m5_h;
    float    threshold;
    float    knee;
    uint32_t mode;
    uint32_t flags;
} PostDownPush;

static_assert(sizeof(PostDownPush) == 88u, "PostDownPush must match shader");

typedef struct PostExposurePush {
    uint64_t hist_bda;
    uint64_t exp_bda;
    float    dt_s;
    float    key_value;
    float    min_scale;
    float    max_scale;
    float    speed_up;
    float    speed_down;
    uint32_t snap;
    uint32_t frame_index;
} PostExposurePush;

static_assert(sizeof(PostExposurePush) == 48u, "PostExposurePush must match shader");

typedef struct PostUpPush {
    uint32_t dst_w;
    uint32_t dst_h;
    float    radius_u;
    float    radius_v;
    float    scatter;
    uint32_t pad0;
} PostUpPush;

static_assert(sizeof(PostUpPush) == 24u, "PostUpPush must match shader");

typedef struct PostCompPush {
    uint64_t exp_bda;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t frame_index;
    float    bloom_strength;
    float    manual_exposure;
    float    vignette_strength;
    float    pad0;
} PostCompPush;

static_assert(sizeof(PostCompPush) == 40u, "PostCompPush must match shader");

typedef struct RetiredBloom {
    VkImage        down_img;
    VkDeviceMemory down_mem;
    VkImageView    down_views[k_down_mips];
    VkImage        up_img;
    VkDeviceMemory up_mem;
    VkImageView    up_views[k_up_mips];
    uint64_t       retire_point;
    bool           used;
} RetiredBloom;

typedef struct PostExecCtx {
    VkImageView hdr_view;
    VkImageView swap_view;
    VkFormat    swap_format;
    uint32_t    width;
    uint32_t    height;
    bool        hdr_active;
    bool        run_bloom;
    bool        run_exp;
    bool        swap_unified;
    float       dt_s;
} PostExecCtx;

static VulkanContext        *s_vk             = nullptr;
static VkFormat              s_hdr_format     = VK_FORMAT_R16G16B16A16_SFLOAT;
static bool                  s_bloom_on       = true;
static bool                  s_auto_exp_on    = true;
static bool                  s_exp_snap       = true;
static uint32_t              s_frame_index    = 0u;
static bool                  s_logged         = false;

static VkSampler             s_linear_samp    = VK_NULL_HANDLE;
static VkDescriptorSetLayout s_down_dsl       = VK_NULL_HANDLE;
static VkDescriptorSetLayout s_up_dsl         = VK_NULL_HANDLE;
static VkDescriptorSetLayout s_comp_dsl       = VK_NULL_HANDLE;
static VkPipelineLayout      s_down_layout    = VK_NULL_HANDLE;
static VkPipelineLayout      s_exp_layout     = VK_NULL_HANDLE;
static VkPipelineLayout      s_up_layout      = VK_NULL_HANDLE;
static VkPipelineLayout      s_comp_layout    = VK_NULL_HANDLE;
static VkPipeline            s_down_pipe      = VK_NULL_HANDLE;
static VkPipeline            s_exp_pipe       = VK_NULL_HANDLE;
static VkPipeline            s_up_pipe        = VK_NULL_HANDLE;
static VulkanGfxLive         s_comp_gfx_srgb;
static VulkanGfxLive         s_comp_gfx_unorm;
static bool                  s_pipes_ready    = false;
static bool                  s_pipes_failed   = false;

static DaBuffer              s_exp_buf        = {0};
static bool                  s_exp_ready      = false;

static VkImage               s_down_img       = VK_NULL_HANDLE;
static VkDeviceMemory        s_down_mem       = VK_NULL_HANDLE;
static VkImageView           s_down_views[k_down_mips];
static VkImageLayout         s_down_layout_st = VK_IMAGE_LAYOUT_UNDEFINED;

static VkImage               s_up_img         = VK_NULL_HANDLE;
static VkDeviceMemory        s_up_mem         = VK_NULL_HANDLE;
static VkImageView           s_up_views[k_up_mips];
static VkImageLayout         s_up_layout_st   = VK_IMAGE_LAYOUT_UNDEFINED;

static uint32_t              s_bloom_src_w    = 0u;
static uint32_t              s_bloom_src_h    = 0u;
static uint32_t              s_mip_w[k_down_mips];
static uint32_t              s_mip_h[k_down_mips];
static uint64_t              s_bloom_vram     = 0ull;

static VkSemaphore           s_acquire_sem    = VK_NULL_HANDLE;
static uint64_t              s_cur_point      = 0ull;
static uint64_t              s_last_use_point = 0ull;
static RetiredBloom          s_retired[k_retire_cap];
static PostExecCtx           s_exec_ctx       = {0};

static bool env_flag(const char *name, bool fallback) {
    const char *v = getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return fallback;
    }
    if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0 || strcmp(v, "false") == 0 ||
        strcmp(v, "fixed") == 0) {
        return false;
    }
    if (strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0 ||
        strcmp(v, "auto") == 0) {
        return true;
    }
    return fallback;
}

static bool format_supports_hdr_compute(const VulkanContext *vk, VkFormat fmt) {
    if (vk == nullptr || vk->physical_device == VK_NULL_HANDLE) {
        return false;
    }
    VkFormatProperties3 fp3 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
    };
    VkFormatProperties2 fp2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &fp3,
    };
    vkGetPhysicalDeviceFormatProperties2(vk->physical_device, fmt, &fp2);
    const VkFormatFeatureFlags2 need =
        VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    return (fp3.optimalTilingFeatures & need) == need;
}

static void destroy_retired_entry(VulkanContext *vk, RetiredBloom *r) {
    if (r == nullptr || !r->used) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        for (uint32_t i = 0u; i < k_down_mips; ++i) {
            if (r->down_views[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(vk->device, r->down_views[i], nullptr);
            }
        }
        for (uint32_t i = 0u; i < k_up_mips; ++i) {
            if (r->up_views[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(vk->device, r->up_views[i], nullptr);
            }
        }
        if (r->down_img != VK_NULL_HANDLE) vkDestroyImage(vk->device, r->down_img, nullptr);
        if (r->down_mem != VK_NULL_HANDLE) vkFreeMemory(vk->device, r->down_mem, nullptr);
        if (r->up_img   != VK_NULL_HANDLE) vkDestroyImage(vk->device, r->up_img, nullptr);
        if (r->up_mem   != VK_NULL_HANDLE) vkFreeMemory(vk->device, r->up_mem, nullptr);
    }
    *r = (RetiredBloom){0};
}

static void reap_retired_bloom(VulkanContext *vk, bool force) {
    uint64_t done = UINT64_MAX;
    if (!force && vk != nullptr && vk->device != VK_NULL_HANDLE && s_acquire_sem != VK_NULL_HANDLE) {
        if (vkGetSemaphoreCounterValue(vk->device, s_acquire_sem, &done) != VK_SUCCESS) {
            done = 0ull;
        }
    }
    for (uint32_t i = 0u; i < k_retire_cap; ++i) {
        if (!s_retired[i].used) {
            continue;
        }
        if (force || done >= s_retired[i].retire_point) {
            destroy_retired_entry(vk, &s_retired[i]);
        }
    }
}

static void retire_active_bloom(VulkanContext *vk) {
    if (s_down_img == VK_NULL_HANDLE && s_up_img == VK_NULL_HANDLE) {
        return;
    }
    reap_retired_bloom(vk, false);
    RetiredBloom snap = {
        .down_img     = s_down_img,
        .down_mem     = s_down_mem,
        .up_img       = s_up_img,
        .up_mem       = s_up_mem,
        .retire_point = s_last_use_point,
        .used         = true,
    };
    memcpy(snap.down_views, s_down_views, sizeof s_down_views);
    memcpy(snap.up_views,   s_up_views,   sizeof s_up_views);

    s_down_img       = VK_NULL_HANDLE;
    s_down_mem       = VK_NULL_HANDLE;
    s_up_img         = VK_NULL_HANDLE;
    s_up_mem         = VK_NULL_HANDLE;
    memset(s_down_views, 0, sizeof s_down_views);
    memset(s_up_views,   0, sizeof s_up_views);
    s_down_layout_st = VK_IMAGE_LAYOUT_UNDEFINED;
    s_up_layout_st   = VK_IMAGE_LAYOUT_UNDEFINED;
    s_bloom_src_w    = 0u;
    s_bloom_src_h    = 0u;
    s_bloom_vram     = 0ull;

    for (uint32_t i = 0u; i < k_retire_cap; ++i) {
        if (!s_retired[i].used) {
            s_retired[i] = snap;
            return;
        }
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(vk->device);
    }
    destroy_retired_entry(vk, &snap);
}

static bool create_mip_image(VulkanContext *vk,
                             uint32_t w, uint32_t h, uint32_t mips,
                             VkImage *out_img, VkDeviceMemory *out_mem,
                             VkImageView *out_views, uint64_t *out_bytes) {
    VkImageCreateInfo ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = s_hdr_format,
        .extent        = { .width = w, .height = h, .depth = 1u },
        .mipLevels     = mips,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(vk->device, &ci, nullptr, out_img) != VK_SUCCESS) {
        return false;
    }
    VkImageMemoryRequirementsInfo2 req_i = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = *out_img,
    };
    VkMemoryRequirements2 req2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    vkGetImageMemoryRequirements2(vk->device, &req_i, &req2);

    uint32_t type_idx = 0u;
    if (vulkan_find_memory_type(vk, req2.memoryRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_idx) != VULKAN_SUCCESS) {
        vkDestroyImage(vk->device, *out_img, nullptr);
        *out_img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req2.memoryRequirements.size,
        .memoryTypeIndex = type_idx,
    };
    if (vkAllocateMemory(vk->device, &ai, nullptr, out_mem) != VK_SUCCESS) {
        vkDestroyImage(vk->device, *out_img, nullptr);
        *out_img = VK_NULL_HANDLE;
        return false;
    }
    VkBindImageMemoryInfo bi = {
        .sType  = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image  = *out_img,
        .memory = *out_mem,
    };
    if (vkBindImageMemory2(vk->device, 1u, &bi) != VK_SUCCESS) {
        vkDestroyImage(vk->device, *out_img, nullptr);
        vkFreeMemory(vk->device, *out_mem, nullptr);
        *out_img = VK_NULL_HANDLE;
        *out_mem = VK_NULL_HANDLE;
        return false;
    }

    for (uint32_t m = 0u; m < mips; ++m) {
        VkImageViewCreateInfo vi = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = *out_img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = s_hdr_format,
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = m,
                .levelCount     = 1u,
                .baseArrayLayer = 0u,
                .layerCount     = 1u,
            },
        };
        if (vkCreateImageView(vk->device, &vi, nullptr, &out_views[m]) != VK_SUCCESS) {
            for (uint32_t j = 0u; j < m; ++j) {
                vkDestroyImageView(vk->device, out_views[j], nullptr);
                out_views[j] = VK_NULL_HANDLE;
            }
            vkDestroyImage(vk->device, *out_img, nullptr);
            vkFreeMemory(vk->device, *out_mem, nullptr);
            *out_img = VK_NULL_HANDLE;
            *out_mem = VK_NULL_HANDLE;
            return false;
        }
    }
    if (out_bytes != nullptr) {
        *out_bytes = (uint64_t)req2.memoryRequirements.size;
    }
    return true;
}

static bool ensure_pipelines(void) {
    if (s_pipes_ready) {
        return true;
    }
    if (s_pipes_failed || s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }

    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = 0.0f,
    };
    if (vkCreateSampler(s_vk->device, &sci, nullptr, &s_linear_samp) != VK_SUCCESS) {
        s_pipes_failed = true;
        return false;
    }

    // 1. Downsample push-descriptor layout (bindings 0..7)
    VkDescriptorSetLayoutBinding down_b[8] = {
        { .binding = 0u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 4u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 5u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 6u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 7u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo down_dci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = 8u,
        .pBindings    = down_b,
    };
    // 2. Upsample push-descriptor layout (bindings 0..2)
    VkDescriptorSetLayoutBinding up_b[3] = {
        { .binding = 0u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo up_dci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = 3u,
        .pBindings    = up_b,
    };
    // 3. Composite push-descriptor layout (bindings 0..1)
    VkDescriptorSetLayoutBinding comp_b[2] = {
        { .binding = 0u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1u, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo comp_dci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = 2u,
        .pBindings    = comp_b,
    };

    if (vkCreateDescriptorSetLayout(s_vk->device, &down_dci, nullptr, &s_down_dsl) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(s_vk->device, &up_dci,   nullptr, &s_up_dsl)   != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(s_vk->device, &comp_dci, nullptr, &s_comp_dsl) != VK_SUCCESS) {
        s_pipes_failed = true;
        return false;
    }

    VkPushConstantRange p_down = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,  .size = sizeof(PostDownPush) };
    VkPushConstantRange p_exp  = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,  .size = sizeof(PostExposurePush) };
    VkPushConstantRange p_up   = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,  .size = sizeof(PostUpPush) };
    VkPushConstantRange p_comp = { .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .size = sizeof(PostCompPush) };

    VkPipelineLayoutCreateInfo pl_down = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1u, .pSetLayouts = &s_down_dsl, .pushConstantRangeCount = 1u, .pPushConstantRanges = &p_down };
    VkPipelineLayoutCreateInfo pl_exp  = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1u, .pPushConstantRanges = &p_exp };
    VkPipelineLayoutCreateInfo pl_up   = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1u, .pSetLayouts = &s_up_dsl,   .pushConstantRangeCount = 1u, .pPushConstantRanges = &p_up };
    VkPipelineLayoutCreateInfo pl_comp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1u, .pSetLayouts = &s_comp_dsl, .pushConstantRangeCount = 1u, .pPushConstantRanges = &p_comp };

    if (vkCreatePipelineLayout(s_vk->device, &pl_down, nullptr, &s_down_layout) != VK_SUCCESS ||
        vkCreatePipelineLayout(s_vk->device, &pl_exp,  nullptr, &s_exp_layout)  != VK_SUCCESS ||
        vkCreatePipelineLayout(s_vk->device, &pl_up,   nullptr, &s_up_layout)   != VK_SUCCESS ||
        vkCreatePipelineLayout(s_vk->device, &pl_comp, nullptr, &s_comp_layout) != VK_SUCCESS) {
        s_pipes_failed = true;
        return false;
    }

    s_down_pipe = vulkan_compute_pipeline(s_vk, k_down_spv, sizeof k_down_spv, s_down_layout);
    s_exp_pipe  = vulkan_compute_pipeline(s_vk, k_exp_spv,  sizeof k_exp_spv,  s_exp_layout);
    s_up_pipe   = vulkan_compute_pipeline(s_vk, k_up_spv,   sizeof k_up_spv,   s_up_layout);
    if (s_down_pipe == VK_NULL_HANDLE || s_exp_pipe == VK_NULL_HANDLE || s_up_pipe == VK_NULL_HANDLE) {
        s_pipes_failed = true;
        return false;
    }

    VulkanGfxPipe comp_gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_comp_spv,
        .frag_bytes    = sizeof k_comp_spv,
        .layout        = s_comp_layout,
        .depth_format  = VK_FORMAT_UNDEFINED,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_FALSE,
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_ALWAYS,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_comp_gfx_srgb,  &comp_gfx, VK_FORMAT_B8G8R8A8_SRGB,  nullptr);
    vulkan_gfx_arm(&s_comp_gfx_unorm, &comp_gfx, VK_FORMAT_B8G8R8A8_UNORM, nullptr);

    s_pipes_ready = true;
    return true;
}

static bool ensure_exposure_and_bloom(uint32_t width, uint32_t height, bool need_bloom) {
    if (!ensure_pipelines()) {
        return false;
    }
    if (!s_exp_ready) {
        if (da_buffer_create(s_vk, k_exp_buf_bytes,
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             true, &s_exp_buf) != 0) {
            return false;
        }
        if (s_exp_buf.map != nullptr) {
            memset(s_exp_buf.map, 0, k_exp_buf_bytes);
            PostExposureState *st = (PostExposureState *)((uint8_t *)s_exp_buf.map + k_hist_bytes);
            st->exposure_scale = 1.0f;
            st->avg_luma       = 0.18f;
            st->valid          = 0u;
        }
        s_exp_ready = true;
    }
    if (!need_bloom) {
        return true;
    }
    if (s_down_img != VK_NULL_HANDLE && s_bloom_src_w == width && s_bloom_src_h == height) {
        return true;
    }
    retire_active_bloom(s_vk);

    uint32_t mw = (width  + 1u) / 2u;
    uint32_t mh = (height + 1u) / 2u;
    if (mw < 1u) mw = 1u;
    if (mh < 1u) mh = 1u;
    for (uint32_t m = 0u; m < k_down_mips; ++m) {
        uint32_t tw = mw >> m;
        uint32_t th = mh >> m;
        s_mip_w[m] = tw > 0u ? tw : 1u;
        s_mip_h[m] = th > 0u ? th : 1u;
    }

    uint64_t down_bytes = 0ull;
    uint64_t up_bytes   = 0ull;
    if (!create_mip_image(s_vk, s_mip_w[0], s_mip_h[0], k_down_mips,
                          &s_down_img, &s_down_mem, s_down_views, &down_bytes) ||
        !create_mip_image(s_vk, s_mip_w[0], s_mip_h[0], k_up_mips,
                          &s_up_img, &s_up_mem, s_up_views, &up_bytes)) {
        retire_active_bloom(s_vk);
        return false;
    }
    s_bloom_src_w = width;
    s_bloom_src_h = height;
    s_bloom_vram  = down_bytes + up_bytes;
    if (!s_logged) {
        printf("[Post] Linear HDR (%s) + Fused SPD Bloom (%ux%u, 6 mips, %.2f MiB) + 256-Bin Log-Luma Auto-Exposure + AgX Punchy\n",
               s_hdr_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? "B10G11R11_UFLOAT 4B/px" : "R16G16B16A16_SFLOAT 8B/px",
               s_mip_w[0], s_mip_h[0], (double)s_bloom_vram / (1024.0 * 1024.0));
        fflush(stdout);
        s_logged = true;
    }
    return true;
}

static void img_mip_barrier(VkCommandBuffer cmd, VkImage img, uint32_t base_mip, uint32_t count,
                            VkPipelineStageFlags2 src_st, VkAccessFlags2 src_ac,
                            VkPipelineStageFlags2 dst_st, VkAccessFlags2 dst_ac,
                            VkImageLayout old_l, VkImageLayout new_l) {
    VkImageMemoryBarrier2 b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src_st,
        .srcAccessMask       = src_ac,
        .dstStageMask        = dst_st,
        .dstAccessMask       = dst_ac,
        .oldLayout           = old_l,
        .newLayout           = new_l,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = img,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = base_mip,
            .levelCount     = count,
            .baseArrayLayer = 0u,
            .layerCount     = 1u,
        },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1u,
        .pImageMemoryBarriers    = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void exec_bloom_down_hist(VkCommandBuffer cmd, void *user) {
    const PostExecCtx *ctx = (const PostExecCtx *)user;
    const bool use_unified = s_vk->device_info.supports_unified_image_layouts;
    const VkImageLayout hdr_rl = use_unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_down_pipe);

    VkDescriptorImageInfo imgs[8] = {
        { .sampler = s_linear_samp, .imageView = ctx->hdr_view, .imageLayout = hdr_rl },
        { .imageView = s_down_views[0], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = s_down_views[1], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = s_down_views[2], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = s_down_views[3], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = s_down_views[4], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .sampler = s_linear_samp, .imageView = s_down_views[4], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = s_down_views[5], .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkWriteDescriptorSet writes[8];
    for (uint32_t i = 0u; i < 8u; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding      = i,
            .descriptorCount = 1u,
            .descriptorType  = (i == 0u || i == 6u) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                    : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo      = &imgs[i],
        };
    }
    vulkan_cmd_push_descriptors(s_vk, cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_down_layout, 0u, 8u, writes);

    uint32_t flags = (ctx->run_exp ? 1u : 0u) |
                     (ctx->run_bloom ? 2u : 0u) |
                     (ctx->run_exp ? 4u : 0u);
    PostDownPush push = {
        .hist_bda  = s_exp_buf.address,
        .exp_bda   = s_exp_buf.address + k_hist_bytes,
        .src_w     = ctx->width,
        .src_h     = ctx->height,
        .m0_w      = s_mip_w[0], .m0_h = s_mip_h[0],
        .m1_w      = s_mip_w[1], .m1_h = s_mip_h[1],
        .m2_w      = s_mip_w[2], .m2_h = s_mip_h[2],
        .m3_w      = s_mip_w[3], .m3_h = s_mip_h[3],
        .m4_w      = s_mip_w[4], .m4_h = s_mip_h[4],
        .m5_w      = s_mip_w[5], .m5_h = s_mip_h[5],
        .threshold = 0.78f,
        .knee      = 0.62f,
        .mode      = 0u,
        .flags     = flags,
    };
    vulkan_cmd_push(cmd, s_down_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, (s_mip_w[0] + 15u) / 16u, (s_mip_h[0] + 15u) / 16u, 1u);

    if (ctx->run_bloom) {
        // Barrier Mip 4 write -> sampled read for Mode 1 coarse Mip 5 tail
        img_mip_barrier(cmd, s_down_img, 4u, 1u,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        push.mode = 1u;
        vulkan_cmd_push(cmd, s_down_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
        vkCmdDispatch(cmd, 1u, 1u, 1u);
    }
    s_last_use_point = s_cur_point;
}

static void exec_auto_exposure(VkCommandBuffer cmd, void *user) {
    const PostExecCtx *ctx = (const PostExecCtx *)user;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_exp_pipe);

    PostExposurePush push = {
        .hist_bda    = s_exp_buf.address,
        .exp_bda     = s_exp_buf.address + k_hist_bytes,
        .dt_s        = ctx->dt_s,
        .key_value   = 0.24f,
        .min_scale   = 0.45f,
        .max_scale   = 2.60f,
        .speed_up    = 3.4f,
        .speed_down  = 1.5f,
        .snap        = s_exp_snap ? 1u : 0u,
        .frame_index = s_frame_index,
    };
    s_exp_snap = false;
    vulkan_cmd_push(cmd, s_exp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, 1u, 1u, 1u);
}

static void exec_bloom_up(VkCommandBuffer cmd, void *user) {
    (void)user;
    const bool use_unified = s_vk->device_info.supports_unified_image_layouts;
    const VkImageLayout down_rl = use_unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_up_pipe);

    // Progressive Jimenez 3x3 tent upsample: mip 4 -> 3 -> 2 -> 1 -> 0
    for (int m = (int)k_up_mips - 1; m >= 0; --m) {
        VkImageView small_view   = (m == (int)k_up_mips - 1) ? s_down_views[5] : s_up_views[m + 1];
        VkImageLayout small_lay  = (m == (int)k_up_mips - 1) ? down_rl         : VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo imgs[3] = {
            { .sampler = s_linear_samp, .imageView = s_down_views[m], .imageLayout = down_rl },
            { .sampler = s_linear_samp, .imageView = small_view,      .imageLayout = small_lay },
            { .imageView = s_up_views[m],                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        };
        VkWriteDescriptorSet writes[3] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0u, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imgs[0] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1u, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imgs[1] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2u, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          .pImageInfo = &imgs[2] },
        };
        vulkan_cmd_push_descriptors(s_vk, cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_up_layout, 0u, 3u, writes);

        uint32_t sw = s_mip_w[m + 1];
        uint32_t sh = s_mip_h[m + 1];
        PostUpPush push = {
            .dst_w    = s_mip_w[m],
            .dst_h    = s_mip_h[m],
            .radius_u = 1.15f / (float)(sw > 0u ? sw : 1u),
            .radius_v = 1.15f / (float)(sh > 0u ? sh : 1u),
            .scatter  = 0.68f,
        };
        vulkan_cmd_push(cmd, s_up_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
        vkCmdDispatch(cmd, (s_mip_w[m] + 7u) / 8u, (s_mip_h[m] + 7u) / 8u, 1u);

        if (m > 0) {
            img_mip_barrier(cmd, s_up_img, (uint32_t)m, 1u,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
    }
    s_last_use_point = s_cur_point;
}

static void exec_post_composite(VkCommandBuffer cmd, void *user) {
    const PostExecCtx *ctx = (const PostExecCtx *)user;
    VulkanGfxLive *gfx = (ctx->swap_format == VK_FORMAT_B8G8R8A8_UNORM)
                       ? &s_comp_gfx_unorm : &s_comp_gfx_srgb;
    const bool use_unified = s_vk->device_info.supports_unified_image_layouts;
    const VkImageLayout rl = use_unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkRenderingAttachmentInfo color_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = ctx->swap_view,
        .imageLayout = ctx->swap_unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { .extent = { .width = ctx->width, .height = ctx->height } },
        .layerCount           = 1u,
        .colorAttachmentCount = 1u,
        .pColorAttachments    = &color_att,
    };
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { .width = (float)ctx->width, .height = (float)ctx->height, .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D   sc = { .extent = { .width = ctx->width, .height = ctx->height } };
    vkCmdSetViewport(cmd, 0u, 1u, &vp);
    vkCmdSetScissor(cmd, 0u, 1u, &sc);

    if (vulkan_gfx_bind(s_vk, cmd, gfx)) {
        VkImageView bloom_v = (ctx->run_bloom && s_up_views[0] != VK_NULL_HANDLE)
                            ? s_up_views[0] : ctx->hdr_view;
        VkDescriptorImageInfo imgs[2] = {
            { .sampler = s_linear_samp, .imageView = ctx->hdr_view, .imageLayout = rl },
            { .sampler = s_linear_samp, .imageView = bloom_v,       .imageLayout = rl },
        };
        VkWriteDescriptorSet writes[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0u, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imgs[0] },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1u, .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imgs[1] },
        };
        vulkan_cmd_push_descriptors(s_vk, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_comp_layout, 0u, 2u, writes);

        uint32_t flags = (ctx->hdr_active ? 1u : 0u) |
                         (ctx->run_bloom  ? 2u : 0u) |
                         (ctx->run_exp    ? 4u : 0u);
        PostCompPush push = {
            .exp_bda           = s_exp_ready ? (s_exp_buf.address + k_hist_bytes) : 0ull,
            .width             = ctx->width,
            .height            = ctx->height,
            .flags             = flags,
            .frame_index       = s_frame_index++,
            .bloom_strength    = 0.14f,
            .manual_exposure   = 1.00f,
            .vignette_strength = ctx->hdr_active ? 0.08f : 0.0f,
        };
        vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof push, &push);
        vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
    }
    vkCmdEndRendering(cmd);
}

int post_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk          = vk;
    s_bloom_on    = env_flag("ENGINE_BLOOM", true);
    s_auto_exp_on = env_flag("ENGINE_EXPOSURE", true);
    s_exp_snap    = true;
    s_frame_index = 0u;
    s_pipes_failed = false;

    if (vk->device_info.supports_storage_write_without_format &&
        format_supports_hdr_compute(vk, VK_FORMAT_B10G11R11_UFLOAT_PACK32)) {
        s_hdr_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    } else {
        s_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    return 0;
}

static void destroy_active_bloom_now(VulkanContext *vk) {
    if (s_down_img == VK_NULL_HANDLE && s_up_img == VK_NULL_HANDLE) {
        return;
    }
    RetiredBloom snap = {
        .down_img = s_down_img,
        .down_mem = s_down_mem,
        .up_img   = s_up_img,
        .up_mem   = s_up_mem,
        .used     = true,
    };
    memcpy(snap.down_views, s_down_views, sizeof s_down_views);
    memcpy(snap.up_views,   s_up_views,   sizeof s_up_views);
    destroy_retired_entry(vk, &snap);

    s_down_img       = VK_NULL_HANDLE;
    s_down_mem       = VK_NULL_HANDLE;
    s_up_img         = VK_NULL_HANDLE;
    s_up_mem         = VK_NULL_HANDLE;
    memset(s_down_views, 0, sizeof s_down_views);
    memset(s_up_views,   0, sizeof s_up_views);
    s_down_layout_st = VK_IMAGE_LAYOUT_UNDEFINED;
    s_up_layout_st   = VK_IMAGE_LAYOUT_UNDEFINED;
    s_bloom_src_w    = 0u;
    s_bloom_src_h    = 0u;
    s_bloom_vram     = 0ull;
}

void post_shutdown(VulkanContext *vk) {
    destroy_active_bloom_now(vk);
    reap_retired_bloom(vk, true);
    da_buffer_destroy(vk, &s_exp_buf);
    s_exp_ready = false;

    vulkan_gfx_disarm(vk, &s_comp_gfx_srgb);
    vulkan_gfx_disarm(vk, &s_comp_gfx_unorm);

    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (s_down_pipe   != VK_NULL_HANDLE) vulkan_pipe_release(vk, s_down_pipe);
        if (s_exp_pipe    != VK_NULL_HANDLE) vulkan_pipe_release(vk, s_exp_pipe);
        if (s_up_pipe     != VK_NULL_HANDLE) vulkan_pipe_release(vk, s_up_pipe);
        if (s_down_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk->device, s_down_layout, nullptr);
        if (s_exp_layout  != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk->device, s_exp_layout, nullptr);
        if (s_up_layout   != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk->device, s_up_layout, nullptr);
        if (s_comp_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(vk->device, s_comp_layout, nullptr);
        if (s_down_dsl    != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(vk->device, s_down_dsl, nullptr);
        if (s_up_dsl      != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(vk->device, s_up_dsl, nullptr);
        if (s_comp_dsl    != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(vk->device, s_comp_dsl, nullptr);
        if (s_linear_samp != VK_NULL_HANDLE) vkDestroySampler(vk->device, s_linear_samp, nullptr);
    }
    s_down_pipe = s_exp_pipe = s_up_pipe = VK_NULL_HANDLE;
    s_down_layout = s_exp_layout = s_up_layout = s_comp_layout = VK_NULL_HANDLE;
    s_down_dsl = s_up_dsl = s_comp_dsl = VK_NULL_HANDLE;
    s_linear_samp = VK_NULL_HANDLE;
    s_pipes_ready = false;
    s_pipes_failed = false;
    s_acquire_sem = VK_NULL_HANDLE;
    s_cur_point = 0ull;
    s_last_use_point = 0ull;
    s_logged = false;
    s_vk = nullptr;
}

void post_reset_exposure(void) {
    s_exp_snap = true;
}

void post_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    s_acquire_sem = acquire;
    s_cur_point   = signal_point;
    if (s_vk != nullptr) {
        reap_retired_bloom(s_vk, false);
    }
}

VkFormat post_hdr_format(void) {
    return s_hdr_format;
}

void post_add_graph_passes(RgGraph *g,
                           RgResHandle hdr_res,
                           VkImageView hdr_view,
                           RgResHandle swap_res,
                           VkImageView swap_view,
                           VkFormat swap_format,
                           uint32_t width,
                           uint32_t height,
                           bool hdr_active,
                           bool allow_exposure_bloom,
                           float dt_s) {
    if (g == nullptr || s_vk == nullptr || hdr_view == VK_NULL_HANDLE || swap_view == VK_NULL_HANDLE ||
        width == 0u || height == 0u) {
        return;
    }
    if (!ensure_pipelines()) {
        return;
    }

    const bool want_bloom = hdr_active && allow_exposure_bloom && s_bloom_on;
    const bool want_exp   = hdr_active && allow_exposure_bloom && s_auto_exp_on;
    const bool need_chain = want_bloom || want_exp;
    bool chain_ok = false;
    if (need_chain) {
        chain_ok = ensure_exposure_and_bloom(width, height, true);
    }

    const bool run_bloom = want_bloom && chain_ok;
    const bool run_exp   = want_exp && chain_ok;

    s_exec_ctx = (PostExecCtx){
        .hdr_view     = hdr_view,
        .swap_view    = swap_view,
        .swap_format  = swap_format,
        .width        = width,
        .height       = height,
        .hdr_active   = hdr_active,
        .run_bloom    = run_bloom,
        .run_exp      = run_exp,
        .swap_unified = (swap_res < g->res_count) ? g->resources[swap_res].unified_general : false,
        .dt_s         = dt_s,
    };

    const bool unified = s_vk->device_info.supports_unified_image_layouts;
    RgResHandle down_res = RG_INVALID_RES;
    RgResHandle up_res   = RG_INVALID_RES;
    RgResHandle exp_res  = RG_INVALID_RES;

    if (chain_ok) {
        down_res = rg_import_image(g, "BloomDown", s_down_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                   0u, k_down_mips, s_down_layout_st, &s_down_layout_st, unified);
        up_res   = rg_import_image(g, "BloomUp", s_up_img, VK_IMAGE_ASPECT_COLOR_BIT,
                                   0u, k_up_mips, s_up_layout_st, &s_up_layout_st, unified);
        exp_res  = rg_import_buffer(g, "ExposureBDA", VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);

        // Declare cross-frame WAR/WAW visibility for shared single-instance post targets
        if (s_down_layout_st != VK_IMAGE_LAYOUT_UNDEFINED) {
            rg_set_prior_access(g, down_res,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        }
        if (s_up_layout_st != VK_IMAGE_LAYOUT_UNDEFINED) {
            rg_set_prior_access(g, up_res,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        }
        if (s_frame_index > 0u) {
            rg_set_prior_access(g, exp_res,
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        }
    }

    if (run_bloom || run_exp) {
        RgAccess acc_down[] = {
            { hdr_res,  RG_USE_COMPUTE_SAMPLED },
            { down_res, RG_USE_COMPUTE_WRITE },
            { exp_res,  RG_USE_COMPUTE_RW },
        };
        rg_add_pass(g, run_bloom ? "BloomDown+Hist" : "LumaHistogram",
                    RG_PASS_POST, acc_down, 3u, exec_bloom_down_hist, &s_exec_ctx);
    }
    if (run_exp) {
        RgAccess acc_exp[] = { { exp_res, RG_USE_COMPUTE_RW } };
        rg_add_pass(g, "AutoExposure", RG_PASS_POST, acc_exp, 1u, exec_auto_exposure, &s_exec_ctx);
    }
    if (run_bloom) {
        RgAccess acc_up[] = {
            { down_res, RG_USE_COMPUTE_SAMPLED },
            { up_res,   RG_USE_COMPUTE_WRITE },
        };
        rg_add_pass(g, "BloomUp", RG_PASS_POST, acc_up, 2u, exec_bloom_up, &s_exec_ctx);
    }

    RgAccess acc_comp[4];
    uint32_t n_comp = 0u;
    acc_comp[n_comp++] = (RgAccess){ hdr_res,  RG_USE_FRAG_SAMPLED };
    if (run_bloom) {
        acc_comp[n_comp++] = (RgAccess){ up_res, RG_USE_FRAG_SAMPLED };
    }
    if (run_exp) {
        acc_comp[n_comp++] = (RgAccess){ exp_res, RG_USE_GRAPHICS_READ };
    }
    acc_comp[n_comp++] = (RgAccess){ swap_res, RG_USE_COLOR_WRITE };
    rg_add_pass(g, hdr_active ? "PostAgX" : "PostCopy",
                RG_PASS_POST, acc_comp, n_comp, exec_post_composite, &s_exec_ctx);
}

bool post_toggle_bloom(void) {
    s_bloom_on = !s_bloom_on;
    printf("[Post] Physically-Based Bloom: %s\n", s_bloom_on ? "ON" : "OFF");
    fflush(stdout);
    return true;
}

bool post_toggle_auto_exposure(void) {
    s_auto_exp_on = !s_auto_exp_on;
    s_exp_snap    = true;
    printf("[Post] 256-Bin Histogram Auto-Exposure: %s\n", s_auto_exp_on ? "ON (Snap)" : "OFF (Fixed 1.0)");
    fflush(stdout);
    return true;
}

void post_get_stats(PostBenchStats *out) {
    if (out == nullptr) {
        return;
    }
    float scl = 1.0f;
    float ev  = 0.0f;
    if (s_exp_ready && s_exp_buf.map != nullptr) {
        const PostExposureState *st = (const PostExposureState *)((const uint8_t *)s_exp_buf.map + k_hist_bytes);
        if (st->valid != 0u) {
            scl = st->exposure_scale;
            ev  = st->ev100;
        }
    }
    *out = (PostBenchStats){
        .bloom_enabled         = s_bloom_on,
        .auto_exposure_enabled = s_auto_exp_on,
        .hdr_format            = s_hdr_format,
        .vram_bytes            = s_bloom_vram + (s_exp_ready ? k_exp_buf_bytes : 0ull),
        .last_exposure_scale   = scl,
        .last_ev100            = ev,
    };
}
