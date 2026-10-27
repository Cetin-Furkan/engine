#include "frame/gimbal.h"
#include "frame/camera.h"
#include "frame/pass.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "gimbal.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "gimbal.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "gimbal vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "gimbal frag spirv");

typedef struct GimbalPush {
    float    qx;
    float    qy;
    float    qz;
    float    qw;
    float    fb_w;
    float    fb_h;
    uint32_t ord;
    uint32_t pad;
} GimbalPush;

static_assert(sizeof(GimbalPush) == 32u, "gimbal push");
static_assert(offsetof(GimbalPush, fb_w) == 16u, "gimbal framebuffer");
static_assert(offsetof(GimbalPush, ord) == 24u, "gimbal order");

// Matches shaders/gimbal.vert: hub, three arcs, then six axes.
enum : uint32_t {
    k_gimbal_seg        = 16u,
    k_gimbal_axis_verts = 6u + k_gimbal_seg * 6u + k_gimbal_seg * 3u + 18u,
    k_gimbal_arc_verts  = 3u * k_gimbal_seg * 6u,
    k_gimbal_hub_verts  = k_gimbal_seg * 3u,
    k_gimbal_verts      = k_gimbal_hub_verts + k_gimbal_arc_verts + 6u * k_gimbal_axis_verts,
};

static_assert(k_gimbal_verts == 1344u, "gimbal vertex count matches gimbal.vert");

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static bool             s_logged = false;
static bool             s_want = true;
static bool             s_drawn = true;

static void gimbal_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Gimbal Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
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
        gimbal_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

void gimbal_shutdown(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_pipeline = VK_NULL_HANDLE;
        return;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_pipeline, nullptr);
        s_pipeline = VK_NULL_HANDLE;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
}

int gimbal_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Gimbal Error] The render pass has no depth format yet\n");
        return -1;
    }
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size       = sizeof(GimbalPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        gimbal_fail("vkCreatePipelineLayout", res);
        gimbal_shutdown(vk);
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
        gimbal_shutdown(vk);
        return -1;
    }
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
    VkPipelineMultisampleStateCreateInfo sample_state = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = samples,
    };
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
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
    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat   = depth_format,
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
        .pMultisampleState   = &sample_state,
        .pDepthStencilState  = &depth,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = s_layout,
    };
    res = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipe, nullptr, &s_pipeline);
    vkDestroyShaderModule(vk->device, vert, nullptr);
    vkDestroyShaderModule(vk->device, frag, nullptr);
    if (res != VK_SUCCESS) {
        s_pipeline = VK_NULL_HANDLE;
        gimbal_fail("vkCreateGraphicsPipelines", res);
        gimbal_shutdown(vk);
        return -1;
    }
    if (!s_logged) {
        printf("[Gimbal] View ball in the corner. Numpad 7 shows or hides it.\n");
        fflush(stdout);
        s_logged = true;
    }
    return 0;
}

// view_rotate from shaders/camera.inc. View +Z points at the camera.
static float view_z(float qx, float qy, float qz, float qw, float vx, float vy, float vz) {
    float tx = 2.0f * (qy * vz - qz * vy);
    float ty = 2.0f * (qz * vx - qx * vz);
    float tz = 2.0f * (qx * vy - qy * vx);
    float cz = qx * ty - qy * tx;
    return vz + qw * tz + cz;
}

// Six axis ids, far to near, three bits each. Slot 0 is drawn first.
static uint32_t gimbal_order(float qx, float qy, float qz, float qw) {
    static const float k_dir[6][3] = {
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        { -1.0f, 0.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, -1.0f },
    };
    float depth[6];
    uint32_t id[6];
    for (uint32_t i = 0; i < 6u; ++i) {
        depth[i] = view_z(qx, qy, qz, qw, k_dir[i][0], k_dir[i][1], k_dir[i][2]);
        id[i] = i;
    }
    for (uint32_t i = 1; i < 6u; ++i) {
        uint32_t key = id[i];
        float kz = depth[key];
        uint32_t j = i;
        while (j > 0u && depth[id[j - 1u]] > kz) {
            id[j] = id[j - 1u];
            j--;
        }
        id[j] = key;
    }
    uint32_t ord = 0;
    for (uint32_t i = 0; i < 6u; ++i) {
        ord |= (id[i] & 7u) << (i * 3u);
    }
    return ord;
}

bool gimbal_toggle(void) {
    s_want = !s_want;
    printf("%s", s_want ? "[Gimbal] Shown.\n" : "[Gimbal] Hidden.\n");
    fflush(stdout);
    return s_want != s_drawn;
}

bool gimbal_busy(void) {
    return s_want != s_drawn;
}

void gimbal_commit(void) {
    s_drawn = s_want;
}

void gimbal_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!s_want || cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }
    VkViewport viewport = {
        .width    = (float)width,
        .height   = (float)height,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { .width = width, .height = height },
    };
    ViewPush view;
    view_push_fill(&view, 0);
    GimbalPush push = {
        .qx   = view.qx,
        .qy   = view.qy,
        .qz   = view.qz,
        .qw   = view.qw,
        .fb_w = (float)width,
        .fb_h = (float)height,
        .ord  = gimbal_order(view.qx, view.qy, view.qz, view.qw),
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof push, &push);
    vkCmdDraw(cmd, k_gimbal_verts, 1, 0, 0);
}
