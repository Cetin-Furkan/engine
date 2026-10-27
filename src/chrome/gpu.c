#include "chrome/gpu.h"
#include "chrome/layer.h"
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

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_srgb = VK_NULL_HANDLE;
static VkPipeline       s_unorm = VK_NULL_HANDLE;
static uint32_t         s_logged_w = 0;
static uint32_t         s_logged_h = 0;

static void chrome_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Chrome Error] %s failed: %d\n", what, (int)res);
}

static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b, uint32_t flag) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (flag << 24);
}

static VkShaderModule make_module(VulkanContext *vk, const uint32_t *code, uint32_t bytes) {
    VkShaderModuleCreateInfo info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode    = code,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(vk->device, &info, nullptr, &module);
    if (res != VK_SUCCESS) {
        chrome_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

static VkPipeline make_pipeline(VulkanContext *vk, VkFormat format,
                                VkShaderModule vert, VkShaderModule frag) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName  = "main",
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName  = "main",
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState blend_att = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };
    VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamics,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &format,
        .depthAttachmentFormat   = VK_FORMAT_UNDEFINED,
    };
    VkGraphicsPipelineCreateInfo pipe = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex,
        .pInputAssemblyState = &assembly,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &samples,
        .pDepthStencilState  = &depth,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = s_layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline);
    if (res != VK_SUCCESS) {
        chrome_fail("vkCreateGraphicsPipelines", res);
        return VK_NULL_HANDLE;
    }
    return pipeline;
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

    VkShaderModule vert = make_module(vk, (const uint32_t *)k_vert_spv, (uint32_t)sizeof k_vert_spv);
    VkShaderModule frag = make_module(vk, (const uint32_t *)k_frag_spv, (uint32_t)sizeof k_frag_spv);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk->device, vert, nullptr);
        }
        if (frag != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk->device, frag, nullptr);
        }
        chrome_gpu_shutdown(vk);
        return -1;
    }

    s_srgb = make_pipeline(vk, VK_FORMAT_B8G8R8A8_SRGB, vert, frag);
    s_unorm = make_pipeline(vk, VK_FORMAT_B8G8R8A8_UNORM, vert, frag);
    vkDestroyShaderModule(vk->device, vert, nullptr);
    vkDestroyShaderModule(vk->device, frag, nullptr);
    if (s_srgb == VK_NULL_HANDLE || s_unorm == VK_NULL_HANDLE) {
        chrome_gpu_shutdown(vk);
        return -1;
    }
    printf("[Chrome] Bar, close, and X are drawn by a fragment shader\n");
    fflush(stdout);
    return 0;
}

void chrome_gpu_shutdown(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_srgb = VK_NULL_HANDLE;
        s_unorm = VK_NULL_HANDLE;
        return;
    }
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

void chrome_gpu_draw(VulkanContext *vk, VkCommandBuffer cmd,
                     uint32_t width, uint32_t height, VkFormat format) {
    if (vk == nullptr || cmd == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }
    VkPipeline pipeline = (format == VK_FORMAT_B8G8R8A8_SRGB) ? s_srgb : s_unorm;
    if (format != VK_FORMAT_B8G8R8A8_SRGB && format != VK_FORMAT_B8G8R8A8_UNORM) {
        return;
    }
    if (pipeline == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE) {
        return;
    }

    VkViewport viewport = {
        .width    = (float)width,
        .height   = (float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { .width = width, .height = height },
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

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
