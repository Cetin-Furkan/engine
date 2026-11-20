#include "texture/texture.h"
#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum : uint32_t {
    k_tex_pending_cap = 256u,
    k_tex_retire_cap  = 256u,
};

typedef struct TextureSlot {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint32_t       width;
    uint32_t       height;
    uint32_t       mip_levels;
    uint64_t       vram_bytes;
    uint64_t       retire_point; // Non-zero while previously bound resource is still retiring
    bool           srgb;
    bool           in_use;
} TextureSlot;

typedef struct TexturePendingUpload {
    uint32_t slot;
    DaBuffer stage;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint64_t signal_point;
    bool     recorded;
    bool     in_use;
} TexturePendingUpload;

typedef struct TextureRetiredImg {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint64_t       retire_point;
    bool           active;
} TextureRetiredImg;

static VulkanContext        *s_vk             = nullptr;
static bool                  s_ready          = false;
static bool                  s_gpu_ready      = false;
static bool                  s_srgb_blit_ok   = false;
static bool                  s_unorm_blit_ok  = false;

static VkDescriptorSetLayout s_bindless_dsl   = VK_NULL_HANDLE;
static VkDescriptorPool      s_bindless_pool  = VK_NULL_HANDLE;
static VkDescriptorSet       s_bindless_set   = VK_NULL_HANDLE;
static VkSampler             s_samplers[K_TEX_SAMPLER_COUNT];

static DaBuffer              s_mat_buf        = {0};
static uint32_t              s_mat_cursor     = 1u; // Slot 0 is default white dielectric

static TextureSlot           s_slots[K_TEX_MAX_IMAGES];
static uint32_t              s_slot_cursor    = K_TEX_FIRST_USER;
static uint32_t              s_active_count   = 0u;
static uint64_t              s_active_vram    = 0u;
static uint32_t              s_active_mips    = 0u;

static TexturePendingUpload  s_pending[k_tex_pending_cap];
static TextureRetiredImg     s_retired[k_tex_retire_cap];
static VkSemaphore           s_timeline_sem   = VK_NULL_HANDLE;
static uint64_t              s_cur_signal_pt  = 0u;

static bool point_retired(uint64_t pt) {
    if (pt == 0u) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE || s_timeline_sem == VK_NULL_HANDLE) {
        return false;
    }
    uint64_t done = 0u;
    return vkGetSemaphoreCounterValue(s_vk->device, s_timeline_sem, &done) == VK_SUCCESS &&
           done >= pt;
}

static void destroy_img_handles(VulkanContext *vk, VkImage img, VkDeviceMemory mem, VkImageView view) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return;
    }
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, view, nullptr);
    }
    if (img != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, img, nullptr);
    }
    if (mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, mem, nullptr);
    }
}

static void reap_retired(VulkanContext *vk, bool force_all) {
    for (uint32_t i = 0u; i < k_tex_retire_cap; ++i) {
        if (!s_retired[i].active) {
            continue;
        }
        if (force_all || point_retired(s_retired[i].retire_point)) {
            destroy_img_handles(vk, s_retired[i].image, s_retired[i].memory, s_retired[i].view);
            s_retired[i] = (TextureRetiredImg){0};
        }
    }
    for (uint32_t i = 0u; i < k_tex_pending_cap; ++i) {
        TexturePendingUpload *up = &s_pending[i];
        if (!up->in_use) {
            continue;
        }
        if (force_all || (up->recorded && up->signal_point != 0u && point_retired(up->signal_point))) {
            da_buffer_destroy(vk, &up->stage);
            *up = (TexturePendingUpload){0};
        }
    }
}

static void push_retired(VkImage img, VkDeviceMemory mem, VkImageView view, uint64_t pt) {
    if (img == VK_NULL_HANDLE && mem == VK_NULL_HANDLE && view == VK_NULL_HANDLE) {
        return;
    }
    reap_retired(s_vk, false);
    for (uint32_t i = 0u; i < k_tex_retire_cap; ++i) {
        if (!s_retired[i].active) {
            s_retired[i] = (TextureRetiredImg){
                .image        = img,
                .memory       = mem,
                .view         = view,
                .retire_point = pt,
                .active       = true,
            };
            return;
        }
    }
    if (s_timeline_sem != VK_NULL_HANDLE && s_retired[0].retire_point > 0u) {
        VkSemaphoreWaitInfo wi = {
            .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1u,
            .pSemaphores    = &s_timeline_sem,
            .pValues        = &s_retired[0].retire_point,
        };
        (void)vkWaitSemaphores(s_vk->device, &wi, 3000000000ull);
    }
    destroy_img_handles(s_vk, s_retired[0].image, s_retired[0].memory, s_retired[0].view);
    s_retired[0] = (TextureRetiredImg){
        .image        = img,
        .memory       = mem,
        .view         = view,
        .retire_point = pt,
        .active       = true,
    };
}

static VkSamplerAddressMode wrap_to_vk(TextureWrap w) {
    switch (w) {
    case TEX_WRAP_CLAMP:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TEX_WRAP_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case TEX_WRAP_REPEAT:
    default:              return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static uint32_t compute_mip_levels(uint32_t w, uint32_t h, bool can_blit) {
    if (!can_blit || (w <= 1u && h <= 1u)) {
        return 1u;
    }
    uint32_t m = w > h ? w : h;
    uint32_t levels = 1u;
    while (m > 1u) {
        m >>= 1u;
        levels++;
    }
    return levels;
}

static VkImageLayout final_sample_layout(void) {
    return (s_vk != nullptr && s_vk->device_info.supports_unified_image_layouts)
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void write_bindless_image(uint32_t slot, VkImageView view) {
    if (s_vk == nullptr || s_bindless_set == VK_NULL_HANDLE || view == VK_NULL_HANDLE) {
        return;
    }
    VkDescriptorImageInfo info = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = view,
        .imageLayout = final_sample_layout(),
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = s_bindless_set,
        .dstBinding      = 0u,
        .dstArrayElement = slot,
        .descriptorCount = 1u,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo      = &info,
    };
    vkUpdateDescriptorSets(s_vk->device, 1u, &write, 0u, nullptr);
}

static bool probe_blit_support(VulkanContext *vk, VkFormat fmt) {
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
    vkGetPhysicalDeviceFormatProperties2(vk->physical_device, fmt, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (props.formatProperties.optimalTilingFeatures & need) == need;
}

static int upload_slot_rgba8(uint32_t slot, const uint8_t *rgba, uint32_t w, uint32_t h, bool srgb) {
    if (s_vk == nullptr || rgba == nullptr || w == 0u || h == 0u || slot >= K_TEX_MAX_IMAGES) {
        return -1;
    }
    reap_retired(s_vk, false);

    int pend_idx = -1;
    for (uint32_t i = 0u; i < k_tex_pending_cap; ++i) {
        if (!s_pending[i].in_use) {
            pend_idx = (int)i;
            break;
        }
    }
    if (pend_idx < 0) {
        return -1;
    }

    const VkFormat fmt      = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const bool     can_blit = srgb ? s_srgb_blit_ok : s_unorm_blit_ok;
    const uint32_t mips     = compute_mip_levels(w, h, can_blit);

    VkImageCreateInfo img_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = { .width = w, .height = h, .depth = 1u },
        .mipLevels     = mips,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT |
                         (mips > 1u ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u),
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(s_vk->device, &img_ci, nullptr, &image) != VK_SUCCESS) {
        return -1;
    }

    VkImageMemoryRequirementsInfo2 req_in = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    VkMemoryRequirements2 reqs = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    vkGetImageMemoryRequirements2(s_vk->device, &req_in, &reqs);

    uint32_t mem_type = 0u;
    if (vulkan_find_memory_type(s_vk, reqs.memoryRequirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_type) != VULKAN_SUCCESS) {
        vkDestroyImage(s_vk->device, image, nullptr);
        return -1;
    }

    VkMemoryPriorityAllocateInfoEXT prio = {0};
    VkMemoryAllocateInfo alloc_ci = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = vulkan_chain_memory_priority(s_vk, &prio, 0.75f, nullptr),
        .allocationSize  = reqs.memoryRequirements.size,
        .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(s_vk->device, &alloc_ci, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyImage(s_vk->device, image, nullptr);
        return -1;
    }
    VkBindImageMemoryInfo bind = {
        .sType        = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
        .image        = image,
        .memory       = mem,
        .memoryOffset = 0u,
    };
    if (vkBindImageMemory2(s_vk->device, 1u, &bind) != VK_SUCCESS) {
        vkDestroyImage(s_vk->device, image, nullptr);
        vkFreeMemory(s_vk->device, mem, nullptr);
        return -1;
    }

    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = fmt,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0u,
            .levelCount     = mips,
            .baseArrayLayer = 0u,
            .layerCount     = 1u,
        },
    };
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(s_vk->device, &view_ci, nullptr, &view) != VK_SUCCESS) {
        destroy_img_handles(s_vk, image, mem, VK_NULL_HANDLE);
        return -1;
    }

    VkDeviceSize byte_count = (VkDeviceSize)w * (VkDeviceSize)h * 4ull;
    DaBuffer stage = {0};
    if (da_buffer_create(s_vk, byte_count,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &stage) != 0 || stage.map == nullptr) {
        da_buffer_destroy(s_vk, &stage);
        destroy_img_handles(s_vk, image, mem, view);
        return -1;
    }
    memcpy(stage.map, rgba, (size_t)byte_count);

    s_slots[slot] = (TextureSlot){
        .image        = image,
        .memory       = mem,
        .view         = view,
        .width        = w,
        .height       = h,
        .mip_levels   = mips,
        .vram_bytes   = reqs.memoryRequirements.size,
        .retire_point = 0u,
        .srgb         = srgb,
        .in_use       = true,
    };
    s_active_count++;
    s_active_vram += reqs.memoryRequirements.size;
    s_active_mips += mips;

    s_pending[pend_idx] = (TexturePendingUpload){
        .slot         = slot,
        .stage        = stage,
        .width        = w,
        .height       = h,
        .mip_levels   = mips,
        .signal_point = 0u,
        .recorded     = false,
        .in_use       = true,
    };

    write_bindless_image(slot, view);
    return 0;
}

int texture_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk        = vk;
    s_ready     = true;
    s_gpu_ready = false;
    return 0;
}

bool texture_ensure_ready(void) {
    if (s_gpu_ready) {
        return true;
    }
    if (!s_ready || s_vk == nullptr || s_vk->device == VK_NULL_HANDLE ||
        !s_vk->device_info.supports_bindless_textures) {
        return false;
    }
    VulkanContext *vk = s_vk;

    s_srgb_blit_ok  = probe_blit_support(vk, VK_FORMAT_R8G8B8A8_SRGB);
    s_unorm_blit_ok = probe_blit_support(vk, VK_FORMAT_R8G8B8A8_UNORM);

    // 1. Create bindless descriptor set layout (Set 1: 1024 sampled images + 18 samplers)
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding         = 0u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = K_TEX_MAX_IMAGES,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding         = 1u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = K_TEX_SAMPLER_COUNT,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorBindingFlags bind_flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount  = 2u,
        .pBindingFlags = bind_flags,
    };
    VkDescriptorSetLayoutCreateInfo dsl_ci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext        = &flags_ci,
        .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 2u,
        .pBindings    = bindings,
    };
    if (vkCreateDescriptorSetLayout(vk->device, &dsl_ci, nullptr, &s_bindless_dsl) != VK_SUCCESS) {
        return false;
    }

    // 2. Single persistent UPDATE_AFTER_BIND descriptor pool & set
    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = K_TEX_MAX_IMAGES },
        { .type = VK_DESCRIPTOR_TYPE_SAMPLER,       .descriptorCount = K_TEX_SAMPLER_COUNT },
    };
    VkDescriptorPoolCreateInfo pool_ci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets       = 1u,
        .poolSizeCount = 2u,
        .pPoolSizes    = pool_sizes,
    };
    if (vkCreateDescriptorPool(vk->device, &pool_ci, nullptr, &s_bindless_pool) != VK_SUCCESS) {
        texture_shutdown(vk);
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_set = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = s_bindless_pool,
        .descriptorSetCount = 1u,
        .pSetLayouts        = &s_bindless_dsl,
    };
    if (vkAllocateDescriptorSets(vk->device, &alloc_set, &s_bindless_set) != VK_SUCCESS) {
        texture_shutdown(vk);
        return false;
    }

    // 3. Create 18 static samplers covering all glTF wrapS x wrapT x filter combinations
    const bool  aniso_ok = vk->device_info.supports_sampler_anisotropy;
    const float max_aniso = aniso_ok
        ? fminf(8.0f, vk->device_info.limits.maxSamplerAnisotropy)
        : 1.0f;

    VkDescriptorImageInfo samp_infos[K_TEX_SAMPLER_COUNT];
    for (uint32_t ws = 0u; ws < 3u; ++ws) {
        for (uint32_t wt = 0u; wt < 3u; ++wt) {
            for (uint32_t fl = 0u; fl < 2u; ++fl) {
                uint32_t idx = texture_sampler_index((TextureWrap)ws, (TextureWrap)wt, (TextureFilter)fl);
                bool nearest = (fl == (uint32_t)TEX_FILTER_NEAREST);
                VkSamplerCreateInfo sci = {
                    .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                    .magFilter        = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    .minFilter        = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                    .mipmapMode       = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR,
                    .addressModeU     = wrap_to_vk((TextureWrap)ws),
                    .addressModeV     = wrap_to_vk((TextureWrap)wt),
                    .addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                    .anisotropyEnable = (!nearest && aniso_ok && max_aniso >= 2.0f) ? VK_TRUE : VK_FALSE,
                    .maxAnisotropy    = max_aniso,
                    .minLod           = 0.0f,
                    .maxLod           = VK_LOD_CLAMP_NONE,
                };
                if (vkCreateSampler(vk->device, &sci, nullptr, &s_samplers[idx]) != VK_SUCCESS) {
                    texture_shutdown(vk);
                    return false;
                }
                samp_infos[idx] = (VkDescriptorImageInfo){
                    .sampler     = s_samplers[idx],
                    .imageView   = VK_NULL_HANDLE,
                    .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                };
            }
        }
    }
    VkWriteDescriptorSet samp_write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = s_bindless_set,
        .dstBinding      = 1u,
        .dstArrayElement = 0u,
        .descriptorCount = K_TEX_SAMPLER_COUNT,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo      = samp_infos,
    };
    vkUpdateDescriptorSets(vk->device, 1u, &samp_write, 0u, nullptr);

    // 4. Create host-mapped BDA PBR material table (slot 0 = default white dielectric)
    VkDeviceSize mat_bytes = (VkDeviceSize)K_TEX_MAX_MATERIALS * sizeof(PbrMaterialGpu);
    if (da_buffer_create(vk, mat_bytes,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         true, &s_mat_buf) != 0 || s_mat_buf.map == nullptr) {
        texture_shutdown(vk);
        return false;
    }
    memset(s_mat_buf.map, 0, (size_t)mat_bytes);
    PbrMaterialGpu *mats = (PbrMaterialGpu *)s_mat_buf.map;
    mats[0] = texture_pbr_material_default();
    s_mat_cursor = 1u;

    // 5. Queue built-in 1x1 fallback textures (slots 0..2)
    static const uint8_t k_white[4]       = { 255u, 255u, 255u, 255u };
    static const uint8_t k_flat_normal[4] = { 128u, 128u, 255u, 255u };
    if (upload_slot_rgba8(K_TEX_WHITE_SRGB,   k_white,       1u, 1u, true)  != 0 ||
        upload_slot_rgba8(K_TEX_WHITE_LINEAR, k_white,       1u, 1u, false) != 0 ||
        upload_slot_rgba8(K_TEX_FLAT_NORMAL,  k_flat_normal, 1u, 1u, false) != 0) {
        texture_shutdown(vk);
        return false;
    }

    s_slot_cursor = K_TEX_FIRST_USER;
    s_gpu_ready   = true;
    printf("[Texture] Vulkan 1.4 Bindless PBR heap armed (%u tex slots, %u samplers, %u BDA materials, aniso=%.0fx).\n",
           K_TEX_MAX_IMAGES, K_TEX_SAMPLER_COUNT, K_TEX_MAX_MATERIALS, (double)max_aniso);
    fflush(stdout);
    return true;
}

void texture_shutdown(VulkanContext *vk) {
    VulkanContext *dev_vk = vk != nullptr ? vk : s_vk;
    reap_retired(dev_vk, true);

    for (uint32_t i = 0u; i < K_TEX_MAX_IMAGES; ++i) {
        if (s_slots[i].in_use) {
            destroy_img_handles(dev_vk, s_slots[i].image, s_slots[i].memory, s_slots[i].view);
        }
        s_slots[i] = (TextureSlot){0};
    }

    if (dev_vk != nullptr && dev_vk->device != VK_NULL_HANDLE) {
        for (uint32_t i = 0u; i < K_TEX_SAMPLER_COUNT; ++i) {
            if (s_samplers[i] != VK_NULL_HANDLE) {
                vkDestroySampler(dev_vk->device, s_samplers[i], nullptr);
                s_samplers[i] = VK_NULL_HANDLE;
            }
        }
        if (s_bindless_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(dev_vk->device, s_bindless_pool, nullptr);
        }
        if (s_bindless_dsl != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev_vk->device, s_bindless_dsl, nullptr);
        }
    }
    da_buffer_destroy(dev_vk, &s_mat_buf);

    s_bindless_pool = VK_NULL_HANDLE;
    s_bindless_set  = VK_NULL_HANDLE;
    s_bindless_dsl  = VK_NULL_HANDLE;
    s_mat_cursor    = 1u;
    s_slot_cursor   = K_TEX_FIRST_USER;
    s_active_count  = 0u;
    s_active_vram   = 0u;
    s_active_mips   = 0u;
    s_timeline_sem  = VK_NULL_HANDLE;
    s_cur_signal_pt = 0u;
    s_gpu_ready     = false;
    s_ready         = false;
    s_vk            = nullptr;
}

void texture_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    s_timeline_sem  = acquire;
    s_cur_signal_pt = signal_point;
    if (s_gpu_ready) {
        reap_retired(s_vk, false);
    }
}

VkDescriptorSetLayout texture_bindless_set_layout(void) {
    return s_bindless_dsl;
}

VkDescriptorSet texture_bindless_set(void) {
    return s_bindless_set;
}

uint64_t texture_materials_bda(void) {
    return s_mat_buf.address;
}

static uint32_t alloc_user_slot(void) {
    const uint32_t span = K_TEX_MAX_IMAGES - K_TEX_FIRST_USER;
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        for (uint32_t step = 0u; step < span; ++step) {
            uint32_t idx = K_TEX_FIRST_USER + ((s_slot_cursor - K_TEX_FIRST_USER + step) % span);
            if (!s_slots[idx].in_use) {
                if (pass == 0u && s_slots[idx].retire_point != 0u &&
                    !point_retired(s_slots[idx].retire_point)) {
                    continue;
                }
                if (s_slots[idx].retire_point != 0u && !point_retired(s_slots[idx].retire_point) &&
                    s_timeline_sem != VK_NULL_HANDLE) {
                    VkSemaphoreWaitInfo wi = {
                        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                        .semaphoreCount = 1u,
                        .pSemaphores    = &s_timeline_sem,
                        .pValues        = &s_slots[idx].retire_point,
                    };
                    (void)vkWaitSemaphores(s_vk->device, &wi, 3000000000ull);
                }
                s_slots[idx].retire_point = 0u;
                s_slot_cursor = K_TEX_FIRST_USER + ((idx - K_TEX_FIRST_USER + 1u) % span);
                return idx;
            }
        }
    }
    return K_TEX_INVALID;
}

uint32_t texture_create_rgba8(const uint8_t *rgba, uint32_t width, uint32_t height, bool srgb) {
    if (!texture_ensure_ready() || rgba == nullptr || width == 0u || height == 0u) {
        return K_TEX_INVALID;
    }
    uint32_t slot = alloc_user_slot();
    if (slot == K_TEX_INVALID) {
        fprintf(stderr, "[Texture] Bindless texture heap full (%u/%u)\n",
                s_active_count, K_TEX_MAX_IMAGES);
        return K_TEX_INVALID;
    }
    if (upload_slot_rgba8(slot, rgba, width, height, srgb) != 0) {
        return K_TEX_INVALID;
    }
    return slot;
}

uint32_t texture_create_from_memory(const uint8_t *encoded, size_t len, bool srgb) {
    uint8_t *rgba = nullptr;
    uint32_t w = 0u, h = 0u;
    if (texture_decode_memory(encoded, len, &rgba, &w, &h) != 0) {
        return K_TEX_INVALID;
    }
    uint32_t id = texture_create_rgba8(rgba, w, h, srgb);
    free(rgba);
    return id;
}

uint32_t texture_create_from_file(const char *path, bool srgb) {
    uint8_t *rgba = nullptr;
    uint32_t w = 0u, h = 0u;
    if (texture_decode_file(path, &rgba, &w, &h) != 0) {
        return K_TEX_INVALID;
    }
    uint32_t id = texture_create_rgba8(rgba, w, h, srgb);
    free(rgba);
    return id;
}

uint32_t texture_register_material(const PbrMaterialGpu *mat) {
    if (!texture_ensure_ready() || mat == nullptr || s_mat_buf.map == nullptr) {
        return 0u;
    }
    if (s_mat_cursor >= K_TEX_MAX_MATERIALS) {
        fprintf(stderr, "[Texture] PBR material BDA table full (%u/%u)\n",
                s_mat_cursor, K_TEX_MAX_MATERIALS);
        return 0u;
    }
    uint32_t id = s_mat_cursor++;
    PbrMaterialGpu *mats = (PbrMaterialGpu *)s_mat_buf.map;
    mats[id] = *mat;
    return id;
}

void texture_reset_scene_assets(void) {
    if (!s_gpu_ready || s_vk == nullptr) {
        return;
    }
    uint64_t retire_pt = s_cur_signal_pt;
    // Cancel any unrecorded user-slot uploads cleanly; recorded ones reap on timeline.
    for (uint32_t i = 0u; i < k_tex_pending_cap; ++i) {
        if (s_pending[i].in_use && !s_pending[i].recorded && s_pending[i].slot >= K_TEX_FIRST_USER) {
            da_buffer_destroy(s_vk, &s_pending[i].stage);
            s_pending[i] = (TexturePendingUpload){0};
        }
    }
    for (uint32_t i = K_TEX_FIRST_USER; i < K_TEX_MAX_IMAGES; ++i) {
        if (!s_slots[i].in_use) {
            continue;
        }
        if (s_active_count > 0u) s_active_count--;
        if (s_active_vram >= s_slots[i].vram_bytes) s_active_vram -= s_slots[i].vram_bytes;
        if (s_active_mips >= s_slots[i].mip_levels) s_active_mips -= s_slots[i].mip_levels;

        push_retired(s_slots[i].image, s_slots[i].memory, s_slots[i].view, retire_pt);
        s_slots[i] = (TextureSlot){ .retire_point = retire_pt };
    }
    s_mat_cursor = 1u;
}

void texture_flush_uploads(VkCommandBuffer cmd) {
    if (!s_gpu_ready || cmd == VK_NULL_HANDLE) {
        return;
    }
    const bool          unified    = s_vk->device_info.supports_unified_image_layouts;
    const VkImageLayout dst_layout = unified ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageLayout xfer_dst   = unified ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageLayout xfer_src   = unified ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    for (uint32_t i = 0u; i < k_tex_pending_cap; ++i) {
        TexturePendingUpload *up = &s_pending[i];
        if (!up->in_use || up->recorded) {
            continue;
        }
        TextureSlot *sl = &s_slots[up->slot];
        if (!sl->in_use || sl->image == VK_NULL_HANDLE) {
            up->recorded     = true;
            up->signal_point = s_cur_signal_pt;
            continue;
        }

        // 1. Transition all mips UNDEFINED -> TRANSFER_DST (or GENERAL)
        VkImageMemoryBarrier2 to_dst = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask       = VK_ACCESS_2_NONE,
            .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = xfer_dst,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sl->image,
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0u,
                .levelCount     = up->mip_levels,
                .baseArrayLayer = 0u,
                .layerCount     = 1u,
            },
        };
        VkDependencyInfo dep = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers    = &to_dst,
        };
        vkCmdPipelineBarrier2(cmd, &dep);

        // 2. Copy mip 0 from staging buffer
        VkBufferImageCopy2 copy_reg = {
            .sType            = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset     = up->stage.offset,
            .imageSubresource = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0u,
                .baseArrayLayer = 0u,
                .layerCount     = 1u,
            },
            .imageExtent = { .width = up->width, .height = up->height, .depth = 1u },
        };
        VkCopyBufferToImageInfo2 copy_info = {
            .sType          = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
            .srcBuffer      = up->stage.buffer,
            .dstImage       = sl->image,
            .dstImageLayout = xfer_dst,
            .regionCount    = 1u,
            .pRegions       = &copy_reg,
        };
        vkCmdCopyBufferToImage2(cmd, &copy_info);

        // 3. Generate mip levels 1..N-1 via hardware linear vkCmdBlitImage2
        int32_t mip_w = (int32_t)up->width;
        int32_t mip_h = (int32_t)up->height;
        for (uint32_t m = 1u; m < up->mip_levels; ++m) {
            VkImageMemoryBarrier2 prev_to_src = {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout           = xfer_dst,
                .newLayout           = xfer_src,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = sl->image,
                .subresourceRange    = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = m - 1u,
                    .levelCount     = 1u,
                    .baseArrayLayer = 0u,
                    .layerCount     = 1u,
                },
            };
            VkDependencyInfo prev_dep = {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1u,
                .pImageMemoryBarriers    = &prev_to_src,
            };
            vkCmdPipelineBarrier2(cmd, &prev_dep);

            int32_t next_w = mip_w > 1 ? (mip_w >> 1) : 1;
            int32_t next_h = mip_h > 1 ? (mip_h >> 1) : 1;
            VkImageBlit2 blit_reg = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
                .srcSubresource = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = m - 1u,
                    .baseArrayLayer = 0u,
                    .layerCount     = 1u,
                },
                .srcOffsets = { { 0, 0, 0 }, { mip_w, mip_h, 1 } },
                .dstSubresource = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = m,
                    .baseArrayLayer = 0u,
                    .layerCount     = 1u,
                },
                .dstOffsets = { { 0, 0, 0 }, { next_w, next_h, 1 } },
            };
            VkBlitImageInfo2 blit_info = {
                .sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
                .srcImage       = sl->image,
                .srcImageLayout = xfer_src,
                .dstImage       = sl->image,
                .dstImageLayout = xfer_dst,
                .regionCount    = 1u,
                .pRegions       = &blit_reg,
                .filter         = VK_FILTER_LINEAR,
            };
            vkCmdBlitImage2(cmd, &blit_info);
            mip_w = next_w;
            mip_h = next_h;
        }

        // 4. Final transition to shader sampled read
        VkImageMemoryBarrier2 fin_bars[2];
        uint32_t n_fin = 0u;
        if (up->mip_levels > 1u) {
            fin_bars[n_fin++] = (VkImageMemoryBarrier2){
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
                .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout           = xfer_src,
                .newLayout           = dst_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = sl->image,
                .subresourceRange    = {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel   = 0u,
                    .levelCount     = up->mip_levels - 1u,
                    .baseArrayLayer = 0u,
                    .layerCount     = 1u,
                },
            };
        }
        fin_bars[n_fin++] = (VkImageMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout           = xfer_dst,
            .newLayout           = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = sl->image,
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = up->mip_levels - 1u,
                .levelCount     = 1u,
                .baseArrayLayer = 0u,
                .layerCount     = 1u,
            },
        };
        VkDependencyInfo fin_dep = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = n_fin,
            .pImageMemoryBarriers    = fin_bars,
        };
        vkCmdPipelineBarrier2(cmd, &fin_dep);

        up->recorded     = true;
        up->signal_point = s_cur_signal_pt;
    }
}

void texture_get_stats(TextureBenchStats *out_stats) {
    if (out_stats == nullptr) {
        return;
    }
    *out_stats = (TextureBenchStats){
        .texture_count    = s_active_count,
        .material_count   = s_gpu_ready ? (s_mat_cursor > 0u ? s_mat_cursor - 1u : 0u) : 0u,
        .vram_bytes       = s_active_vram,
        .mip_levels_total = s_active_mips,
        .bindless_active  = s_gpu_ready,
    };
}
