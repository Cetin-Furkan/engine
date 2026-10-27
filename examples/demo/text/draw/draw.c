#include "text/text_int.h"

#include "text/atlas/atlas.h"
#include "text/face/face.h"
#include "vulkan/memory.h"
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
    uint64_t atlas;
    uint32_t fb_w;
    uint32_t fb_h;
    uint32_t dim;
    uint32_t spread;
    uint32_t pad0;
    uint32_t pad1;
} TextPush;

static_assert(sizeof(TextPush) == 32u, "text push");
static_assert(offsetof(TextPush, fb_w) == 8u, "text fb");
static_assert(offsetof(TextPush, dim) == 16u, "text dim");
static_assert(offsetof(TextPush, spread) == 20u, "text spread");

enum : uint32_t { k_quad_cap = 4096 };

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_srgb = VK_NULL_HANDLE;
static VkPipeline       s_unorm = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkDevice         s_device = VK_NULL_HANDLE;
static uint8_t         *s_map = nullptr;
static VkDeviceSize     s_half_bytes = 0;
static uint32_t         s_half = 0;

static void text_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Text Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
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
        text_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

static VkPipeline make_pipeline(VulkanContext *vk, VkFormat format,
                                VkShaderModule vert, VkShaderModule frag) {
    uint32_t srgb = format == VK_FORMAT_B8G8R8A8_SRGB ? 1u : 0u;
    VkSpecializationMapEntry spec_entry = {
        .constantID = 0,
        .offset     = 0,
        .size       = sizeof srgb,
    };
    VkSpecializationInfo spec_info = {
        .mapEntryCount = 1,
        .pMapEntries   = &spec_entry,
        .dataSize      = sizeof srgb,
        .pData         = &srgb,
    };
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName  = "main",
        },
        {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage               = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module              = frag,
            .pName               = "main",
            .pSpecializationInfo = &spec_info,
        },
    };
    VkVertexInputBindingDescription binding = {
        .binding   = 0,
        .stride    = sizeof(TextVert),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attrs[5] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 8 },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16 },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 32 },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 48 },
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = 1,
        .pVertexBindingDescriptions      = &binding,
        .vertexAttributeDescriptionCount = 5,
        .pVertexAttributeDescriptions    = attrs,
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
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
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
        text_fail("vkCreateGraphicsPipelines", res);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

static void gpu_shutdown(VulkanContext *vk) {
    VkDevice device = s_device;
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        device = vk->device;
    }
    s_map = nullptr;
    s_half_bytes = 0;
    s_half = 0;
    if (device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_srgb = VK_NULL_HANDLE;
        s_unorm = VK_NULL_HANDLE;
        s_buffer = VK_NULL_HANDLE;
        s_memory = VK_NULL_HANDLE;
        s_device = VK_NULL_HANDLE;
        return;
    }
    if (s_srgb != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, s_srgb, nullptr);
        s_srgb = VK_NULL_HANDLE;
    }
    if (s_unorm != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, s_unorm, nullptr);
        s_unorm = VK_NULL_HANDLE;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
    if (s_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, s_buffer, nullptr);
        s_buffer = VK_NULL_HANDLE;
    }
    if (s_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, s_memory, nullptr);
        s_memory = VK_NULL_HANDLE;
    }
    s_device = VK_NULL_HANDLE;
}

static int vert_init(VulkanContext *vk) {
    s_half_bytes = (VkDeviceSize)k_quad_cap * 6u * sizeof(TextVert);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = s_half_bytes * 2u,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &s_buffer);
    if (res != VK_SUCCESS) {
        s_buffer = VK_NULL_HANDLE;
        text_fail("vkCreateBuffer", res);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetBufferMemoryRequirements(vk->device, s_buffer, &reqs);
    uint32_t type_index = 0;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, host, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        fprintf(stderr, "[Text Error] No host-visible memory for text vertices\n");
        return -1;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &s_memory);
    if (res != VK_SUCCESS) {
        text_fail("vkAllocateMemory", res);
        return -1;
    }
    res = vkBindBufferMemory(vk->device, s_buffer, s_memory, 0);
    if (res != VK_SUCCESS) {
        text_fail("vkBindBufferMemory", res);
        return -1;
    }
    void *mapped = nullptr;
    res = vkMapMemory(vk->device, s_memory, 0, reqs.size, 0, &mapped);
    if (res != VK_SUCCESS || mapped == nullptr) {
        text_fail("vkMapMemory", res);
        return -1;
    }
    s_map = mapped;
    memset(s_map, 0, (size_t)(s_half_bytes * 2u));
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
    VkShaderModule vert = make_module(vk, (const uint32_t *)k_vert_spv, (uint32_t)sizeof k_vert_spv);
    VkShaderModule frag = make_module(vk, (const uint32_t *)k_frag_spv, (uint32_t)sizeof k_frag_spv);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk->device, vert, nullptr);
        }
        if (frag != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vk->device, frag, nullptr);
        }
        text_shutdown(vk);
        return -1;
    }
    s_srgb = make_pipeline(vk, VK_FORMAT_B8G8R8A8_SRGB, vert, frag);
    s_unorm = make_pipeline(vk, VK_FORMAT_B8G8R8A8_UNORM, vert, frag);
    vkDestroyShaderModule(vk->device, vert, nullptr);
    vkDestroyShaderModule(vk->device, frag, nullptr);
    if (s_srgb == VK_NULL_HANDLE || s_unorm == VK_NULL_HANDLE) {
        text_shutdown(vk);
        return -1;
    }
    s_device = vk->device;
    text_place_boot();
    printf("[Text] Hold T, click, click again, then type.\n");
    fflush(stdout);
    return 0;
}

void text_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, VkFormat format) {
    if (cmd == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE || s_map == nullptr ||
        width == 0u || height == 0u) {
        return;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (format == VK_FORMAT_B8G8R8A8_SRGB) {
        pipeline = s_srgb;
    } else if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        pipeline = s_unorm;
    }
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    uint32_t half = s_half ^ 1u;
    TextVert *dst = (TextVert *)(s_map + (VkDeviceSize)half * s_half_bytes);
    uint32_t verts = text_fill(dst, k_quad_cap * 6u, width, height);
    if (verts == 0u) {
        return;
    }
    s_half = half;
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
    TextPush push = {
        .atlas  = text_atlas_address(),
        .fb_w   = width,
        .fb_h   = height,
        .dim    = TEXT_ATLAS_DIM,
        .spread = TEXT_SDF_SPREAD,
    };
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);
    VkDeviceSize offset = (VkDeviceSize)half * s_half_bytes;
    vkCmdBindVertexBuffers(cmd, 0, 1, &s_buffer, &offset);
    vkCmdDraw(cmd, verts, 1, 0, 0);
}
