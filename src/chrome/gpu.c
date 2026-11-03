#include "chrome/gpu.h"
#include "chrome/layer.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "chrome.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "chrome.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "chrome vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "chrome frag spirv");
static_assert(ENG_CHROME_CONTROLS == 8u, "chrome shader control array");

// Scalar layout, shared with shaders/chrome.vert and shaders/chrome.frag.
typedef struct ChromeGpuControl {
    int32_t  x;
    int32_t  y;
    int32_t  w;
    int32_t  h;
    uint32_t color;
    uint32_t glyph;
} ChromeGpuControl;

typedef struct ChromeGpuPush {
    int32_t          rect_x;
    int32_t          rect_y;
    int32_t          rect_w;
    int32_t          rect_h;
    uint32_t         fb_w;
    uint32_t         fb_h;
    uint32_t         layer_color;
    uint32_t         ncontrols;
    uint32_t         srgb;
    uint32_t         pad;
    ChromeGpuControl ctl[ENG_CHROME_CONTROLS];
} ChromeGpuPush;

static_assert(sizeof(ChromeGpuControl) == 24u, "chrome control push");
static_assert(offsetof(ChromeGpuPush, ctl) == 40u, "chrome control offset");
static_assert(sizeof(ChromeGpuPush) == 232u, "chrome push size");

static VkPipelineLayout     s_layout = VK_NULL_HANDLE;
static VulkanGfxLive        s_overlay_srgb;
static VulkanGfxLive        s_overlay_unorm;
static VulkanGfxLive        s_scene_srgb;
static VulkanGfxLive        s_scene_unorm;
static VkFormat             s_scene_depth = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
static uint32_t             s_logged_w = 0;
static uint32_t             s_logged_h = 0;

static void chrome_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Chrome Error] %s failed: %d\n", what, (int)res);
}

static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t flag) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (flag << 24);
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
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(srgb, &info, VK_FORMAT_B8G8R8A8_SRGB, nullptr);
    vulkan_gfx_arm(unorm, &info, VK_FORMAT_B8G8R8A8_UNORM, nullptr);
}

static void disarm_scene(VulkanContext *vk) {
    vulkan_gfx_disarm(vk, &s_scene_srgb);
    vulkan_gfx_disarm(vk, &s_scene_unorm);
}

int chrome_gpu_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_layout != VK_NULL_HANDLE) {
        return 0;
    }

    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(ChromeGpuPush)) {
        fprintf(stderr, "[Chrome Error] Push constants are %u bytes, chrome needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(ChromeGpuPush));
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(ChromeGpuPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        chrome_fail("vkCreatePipelineLayout", res);
        chrome_gpu_shutdown(vk);
        return -1;
    }

    arm_pair(VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, &s_overlay_srgb, &s_overlay_unorm);
    printf("[Chrome] Bar, close, and X are drawn by a fragment shader\n");
    fflush(stdout);
    return 0;
}

void chrome_gpu_shutdown(VulkanContext *vk) {
    s_scene_depth = VK_FORMAT_UNDEFINED;
    s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
    disarm_scene(vk);
    vulkan_gfx_disarm(vk, &s_overlay_srgb);
    vulkan_gfx_disarm(vk, &s_overlay_unorm);
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
}

int chrome_gpu_bind_pass(VulkanContext *vk, VkFormat depth_format,
                         VkSampleCountFlagBits samples) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE ||
        depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Chrome Error] Scene pipelines need the layout and a depth pass\n");
        return -1;
    }
    if (s_scene_srgb.armed && s_scene_unorm.armed &&
        s_scene_depth == depth_format && s_scene_samples == samples) {
        return 0;
    }
    disarm_scene(vk);
    arm_pair(samples, depth_format, &s_scene_srgb, &s_scene_unorm);
    s_scene_depth = depth_format;
    s_scene_samples = samples;
    return 0;
}

static VkPipeline pick_pipeline(VulkanContext *vk, VkFormat format, VkFormat depth_format,
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
    return vulkan_gfx_use(vk, live);
}

void chrome_gpu_draw(VulkanContext *vk, VkCommandBuffer cmd,
                     uint32_t window_w, uint32_t window_h,
                     int32_t origin_x, int32_t origin_y,
                     uint32_t fb_w, uint32_t fb_h, VkFormat format,
                     VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (vk == nullptr || cmd == VK_NULL_HANDLE || window_w == 0 || window_h == 0 ||
        fb_w == 0 || fb_h == 0) {
        return;
    }
    VkPipeline pipeline = pick_pipeline(vk, format, depth_format, samples);
    if (pipeline == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    uint32_t drawn = 0;
    for (uint32_t i = 0; i < ENG_CHROME_LAYERS; ++i) {
        ChromePaintLayer layer;
        if (!chrome_paint_layer(window_w, window_h, i, &layer)) {
            continue;
        }
        if (layer.w <= 0 || layer.h <= 0) {
            continue;
        }
        if (layer.x + layer.w <= origin_x || layer.y + layer.h <= origin_y ||
            layer.x >= origin_x + (int32_t)fb_w || layer.y >= origin_y + (int32_t)fb_h) {
            continue;
        }
        ChromeGpuPush push = {
            .rect_x      = layer.x - origin_x,
            .rect_y      = layer.y - origin_y,
            .rect_w      = layer.w,
            .rect_h      = layer.h,
            .fb_w        = fb_w,
            .fb_h        = fb_h,
            .layer_color = pack_rgb(layer.r, layer.g, layer.b, 0),
            .ncontrols   = layer.ncontrols,
            .srgb        = (format == VK_FORMAT_B8G8R8A8_SRGB) ? 1u : 0u,
        };
        for (uint32_t c = 0; c < layer.ncontrols && c < ENG_CHROME_CONTROLS; ++c) {
            const ChromePaintControl *ctl = &layer.controls[c];
            push.ctl[c].x = ctl->x - origin_x;
            push.ctl[c].y = ctl->y - origin_y;
            push.ctl[c].w = ctl->w;
            push.ctl[c].h = ctl->h;
            push.ctl[c].color = pack_rgb(ctl->r, ctl->g, ctl->b, ctl->draw_glyph ? 1u : 0u);
            push.ctl[c].glyph = pack_rgb(ctl->gr, ctl->gg, ctl->gb, 0);
        }
        vulkan_cmd_push(cmd, s_layout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        sizeof push, &push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        drawn++;
    }
    if (drawn > 0 && (fb_w != s_logged_w || fb_h != s_logged_h)) {
        printf("[Chrome] GPU bar and X into %ux%u (%u layer%s)\n",
               fb_w, fb_h, drawn, drawn == 1u ? "" : "s");
        s_logged_w = fb_w;
        s_logged_h = fb_h;
    }
}
