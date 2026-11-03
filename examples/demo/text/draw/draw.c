#include "text/text_int.h"

#include "frame/pass.h"
#include "text/atlas/atlas.h"
#include "text/face/face.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "text.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "text.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "text vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "text frag spirv");

typedef struct TextPush {
    uint64_t quads;
    uint64_t atlas;
    uint32_t fb_w;
    uint32_t fb_h;
    uint32_t dim;
    uint32_t spread;
} TextPush;

static_assert(sizeof(TextPush) == 32u, "text push");
static_assert(offsetof(TextPush, atlas) == 8u, "text atlas");
static_assert(offsetof(TextPush, fb_w) == 16u, "text fb");
static_assert(offsetof(TextPush, dim) == 24u, "text dim");
static_assert(offsetof(TextPush, spread) == 28u, "text spread");

// One slice per submit still on the GPU. Twelve covers both slots and the
// orphan ring. A slice stays until the acquire timeline reaches its point.
enum : uint32_t {
    k_quad_cap    = 4096u,
    k_slice_count = 12u,
    k_slice_bytes = k_quad_cap * (uint32_t)sizeof(TextQuad),
};

static_assert(k_slice_bytes % 16u == 0u, "text slice");

typedef struct TextSlice {
    uint64_t point;
} TextSlice;

static const uint32_t k_flag_srgb = 1u;
static const uint32_t k_flag_unorm = 0u;
static const VkSpecializationMapEntry k_text_entry = {
    .constantID = 0,
    .offset     = 0,
    .size       = sizeof(uint32_t),
};
static const VkSpecializationInfo k_spec_srgb = {
    .mapEntryCount = 1,
    .pMapEntries   = &k_text_entry,
    .dataSize      = sizeof k_flag_srgb,
    .pData         = &k_flag_srgb,
};
static const VkSpecializationInfo k_spec_unorm = {
    .mapEntryCount = 1,
    .pMapEntries   = &k_text_entry,
    .dataSize      = sizeof k_flag_unorm,
    .pData         = &k_flag_unorm,
};

static VulkanContext        *s_vk = nullptr;
static VkPipelineLayout      s_layout = VK_NULL_HANDLE;
static VulkanGfxLive         s_overlay_srgb;
static VulkanGfxLive         s_overlay_unorm;
static VulkanGfxLive         s_scene_srgb;
static VulkanGfxLive         s_scene_unorm;
static VkFormat              s_scene_depth = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
static DaBuffer              s_geom;
static VkDevice              s_device = VK_NULL_HANDLE;
static VkDeviceAddress       s_addr = 0;
static uint8_t              *s_map = nullptr;
static TextSlice             s_slice[k_slice_count];
static int                   s_active = -1;
static bool                  s_busy_warned = false;

static void text_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Text Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static void arm_pair(VkSampleCountFlagBits samples, VkFormat depth,
                     VulkanGfxLive *srgb, VulkanGfxLive *unorm) {
    VulkanGfxPipe info = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth,
        .samples       = samples,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_FALSE,
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_TRUE,
    };
    vulkan_gfx_arm(srgb, &info, VK_FORMAT_B8G8R8A8_SRGB, &k_spec_srgb);
    vulkan_gfx_arm(unorm, &info, VK_FORMAT_B8G8R8A8_UNORM, &k_spec_unorm);
}

static void gpu_shutdown(VulkanContext *vk) {
    s_map = nullptr;
    s_addr = 0;
    s_active = -1;
    memset(s_slice, 0, sizeof s_slice);
    VkDevice device = VK_NULL_HANDLE;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        device = vk->device;
    }
    vulkan_gfx_disarm(vk, &s_scene_srgb);
    vulkan_gfx_disarm(vk, &s_scene_unorm);
    vulkan_gfx_disarm(vk, &s_overlay_srgb);
    vulkan_gfx_disarm(vk, &s_overlay_unorm);
    s_vk = nullptr;
    if (device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
    s_scene_depth = VK_FORMAT_UNDEFINED;
    s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
    s_device = VK_NULL_HANDLE;
    da_buffer_destroy(vk, &s_geom);
}

static int vert_init(VulkanContext *vk) {
    VkDeviceSize bytes = (VkDeviceSize)k_slice_count * k_slice_bytes;
    if (da_buffer_create(vk, bytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, &s_geom) != 0) {
        return -1;
    }
    if (s_geom.map == nullptr || s_geom.address == 0) {
        fprintf(stderr, "[Text Error] Text device address is null\n");
        return -1;
    }
    s_map = s_geom.map;
    s_addr = s_geom.address;
    s_device = vk->device;
    memset(s_map, 0, (size_t)bytes);
    return 0;
}

void text_shutdown(VulkanContext *vk) {
    gpu_shutdown(vk);
    text_atlas_shutdown(vk);
    text_face_shutdown();
}

int text_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk = vk;
    if (s_layout != VK_NULL_HANDLE) {
        return 0;
    }
    if (text_face_init() != 0) {
        return -1;
    }
    if (text_atlas_init(vk) != 0) {
        text_shutdown(vk);
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(TextPush)) {
        fprintf(stderr, "[Text Error] Push constants are %u bytes, text needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(TextPush));
        text_shutdown(vk);
        return -1;
    }
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(TextPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        text_fail("vkCreatePipelineLayout", res);
        text_shutdown(vk);
        return -1;
    }
    if (vert_init(vk) != 0) {
        text_shutdown(vk);
        return -1;
    }
    arm_pair(VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, &s_overlay_srgb, &s_overlay_unorm);
    arm_pair(pass_raster_samples(), pass_depth_format(), &s_scene_srgb, &s_scene_unorm);
    s_scene_depth = pass_depth_format();
    s_scene_samples = pass_raster_samples();
    text_place_boot();
    printf("[Text] Hold T, click, click again, then type.\n");
    fflush(stdout);
    return 0;
}

static int claim_slice(VkSemaphore acquire, uint64_t signal_point) {
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            return (int)i;
        }
    }
    uint64_t counter = 0;
    bool have = false;
    if (s_device != VK_NULL_HANDLE && acquire != VK_NULL_HANDLE &&
        vkGetSemaphoreCounterValue(s_device, acquire, &counter) == VK_SUCCESS) {
        have = true;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == 0u || (have && counter >= s_slice[i].point)) {
            s_slice[i].point = signal_point;
            return (int)i;
        }
    }
    return -1;
}

void text_begin(VkSemaphore acquire, uint64_t signal_point) {
    s_active = -1;
    if (s_map == nullptr || signal_point == 0u) {
        return;
    }
    int slot = claim_slice(acquire, signal_point);
    if (slot < 0) {
        if (!s_busy_warned) {
            fprintf(stderr, "[Text Error] Text quads are still on the GPU\n");
            s_busy_warned = true;
        }
        return;
    }
    s_active = slot;
}

void text_release(uint64_t signal_point) {
    if (signal_point == 0u) {
        return;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            s_slice[i].point = 0;
        }
    }
    if (s_active >= 0 && (uint32_t)s_active < k_slice_count && s_slice[s_active].point == 0u) {
        s_active = -1;
    }
}

static VkPipeline pick_pipeline(VkFormat format, VkFormat depth_format,
                                VkSampleCountFlagBits samples) {
    bool scene = depth_format != VK_FORMAT_UNDEFINED;
    if (scene && (depth_format != s_scene_depth || samples != s_scene_samples)) {
        return VK_NULL_HANDLE;
    }
    VulkanGfxLive *live = nullptr;
    if (format == VK_FORMAT_B8G8R8A8_SRGB) {
        live = scene ? &s_scene_srgb : &s_overlay_srgb;
    } else if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        live = scene ? &s_scene_unorm : &s_overlay_unorm;
    }
    if (live == nullptr) {
        return VK_NULL_HANDLE;
    }
    return vulkan_gfx_use(s_vk, live);
}

void text_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, VkFormat format,
               VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (cmd == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE || s_active < 0 ||
        s_map == nullptr || width == 0u || height == 0u) {
        return;
    }
    VkPipeline pipeline = pick_pipeline(format, depth_format, samples);
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    TextQuad *dst = (TextQuad *)(s_map + (size_t)s_active * k_slice_bytes);
    uint32_t quads = text_fill(dst, k_quad_cap, width, height);
    if (quads == 0u) {
        return;
    }
    TextPush push = {
        .quads  = s_addr + (uint64_t)s_active * k_slice_bytes,
        .atlas  = text_atlas_address(),
        .fb_w   = width,
        .fb_h   = height,
        .dim    = TEXT_ATLAS_DIM,
        .spread = TEXT_SDF_SPREAD,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);
    vkCmdDraw(cmd, 6, quads, 0, 0);
}
