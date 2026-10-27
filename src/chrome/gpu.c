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
static VkPipeline           s_srgb = VK_NULL_HANDLE;
static VkPipeline           s_unorm = VK_NULL_HANDLE;
static VkPipeline           s_scene_srgb = VK_NULL_HANDLE;
static VkPipeline           s_scene_unorm = VK_NULL_HANDLE;
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

static int make_pair(VulkanContext *vk, VkSampleCountFlagBits samples, VkFormat depth,
                     VkPipeline *srgb, VkPipeline *unorm) {
    VkFormat formats[2] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM };
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
    VkPipeline out[2] = {0};
    if (vulkan_gfx_pipelines(vk, &info, formats, nullptr, 2, out) != 0) {
        return -1;
    }
    *srgb = out[0];
    *unorm = out[1];
    return 0;
}

static void destroy_scene(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        s_scene_srgb = VK_NULL_HANDLE;
        s_scene_unorm = VK_NULL_HANDLE;
        return;
    }
    if (s_scene_srgb != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, s_scene_srgb, nullptr);
        s_scene_srgb = VK_NULL_HANDLE;
    }
    if (s_scene_unorm != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, s_scene_unorm, nullptr);
        s_scene_unorm = VK_NULL_HANDLE;
    }
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

    if (make_pair(vk, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, &s_srgb, &s_unorm) != 0) {
        chrome_gpu_shutdown(vk);
        return -1;
    }
    printf("[Chrome] Bar, close, and X are drawn by a fragment shader\n");
    fflush(stdout);
    return 0;
}

void chrome_gpu_shutdown(VulkanContext *vk) {
    s_scene_depth = VK_FORMAT_UNDEFINED;
    s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_srgb = VK_NULL_HANDLE;
        s_unorm = VK_NULL_HANDLE;
        s_scene_srgb = VK_NULL_HANDLE;
        s_scene_unorm = VK_NULL_HANDLE;
        return;
    }
    destroy_scene(vk->device);
    if (s_srgb != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_srgb, nullptr);
        s_srgb = VK_NULL_HANDLE;
    }
    if (s_unorm != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_unorm, nullptr);
        s_unorm = VK_NULL_HANDLE;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
}

int chrome_gpu_bind_pass(VulkanContext *vk, VkFormat depth_format,
                         VkSampleCountFlagBits samples) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE ||
        depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Chrome Error] Scene pipelines need the layout and a depth pass\n");
        return -1;
    }
    if (s_scene_srgb != VK_NULL_HANDLE && s_scene_unorm != VK_NULL_HANDLE &&
        s_scene_depth == depth_format && s_scene_samples == samples) {
        return 0;
    }
    destroy_scene(vk->device);
    if (make_pair(vk, samples, depth_format, &s_scene_srgb, &s_scene_unorm) != 0) {
        s_scene_depth = VK_FORMAT_UNDEFINED;
        return -1;
    }
    s_scene_depth = depth_format;
    s_scene_samples = samples;
    return 0;
}

static VkPipeline pick_pipeline(VkFormat format, VkFormat depth_format,
                                VkSampleCountFlagBits samples) {
    bool scene = depth_format != VK_FORMAT_UNDEFINED;
    if (scene && (depth_format != s_scene_depth || samples != s_scene_samples)) {
        return VK_NULL_HANDLE;
    }
    if (format == VK_FORMAT_B8G8R8A8_SRGB) {
        return scene ? s_scene_srgb : s_srgb;
    }
    if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        return scene ? s_scene_unorm : s_unorm;
    }
    return VK_NULL_HANDLE;
}

void chrome_gpu_draw(VulkanContext *vk, VkCommandBuffer cmd,
                     uint32_t width, uint32_t height, VkFormat format,
                     VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (vk == nullptr || cmd == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }
    VkPipeline pipeline = pick_pipeline(format, depth_format, samples);
    if (pipeline == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    uint32_t drawn = 0;
    for (uint32_t i = 0; i < ENG_CHROME_LAYERS; ++i) {
        ChromePaintLayer layer;
        if (!chrome_paint_layer(width, height, i, &layer)) {
            continue;
        }
        ChromeGpuPush push = {
            .rect_x      = layer.x,
            .rect_y      = layer.y,
            .rect_w      = layer.w,
            .rect_h      = layer.h,
            .fb_w        = width,
            .fb_h        = height,
            .layer_color = pack_rgb(layer.r, layer.g, layer.b, 0),
            .ncontrols   = layer.ncontrols,
            .srgb        = (format == VK_FORMAT_B8G8R8A8_SRGB) ? 1u : 0u,
        };
        for (uint32_t c = 0; c < layer.ncontrols && c < ENG_CHROME_CONTROLS; ++c) {
            const ChromePaintControl *ctl = &layer.controls[c];
            push.ctl[c].x = ctl->x;
            push.ctl[c].y = ctl->y;
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
    if (drawn > 0 && (width != s_logged_w || height != s_logged_h)) {
        printf("[Chrome] GPU bar and X into %ux%u (%u layer%s)\n",
               width, height, drawn, drawn == 1u ? "" : "s");
        s_logged_w = width;
        s_logged_h = height;
    }
}
