#include "frame/solid.h"
#include "frame/cube.h"
#include "vulkan/memory.h"
#include <stddef.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "solid.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "solid.frag.spv"
};
alignas(uint32_t) static const unsigned char k_bake_spv[] = {
#embed "solid.comp.spv"
};

// Keep these ranges in lockstep with shaders/solid.comp.
enum { k_solid_verts = 5964u, k_local = 64u };

static_assert(sizeof k_vert_spv % 4u == 0u, "solid vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "solid frag spirv");
static_assert(sizeof k_bake_spv % 4u == 0u, "solid bake spirv");

typedef struct SolidPush {
    uint64_t verts;
    uint32_t width;
    uint32_t height;
    float    yaw_deg;
    float    origin_x;
    float    origin_y;
    float    depth_bias;
    float    selected;
    uint32_t pad;
} SolidPush;

typedef struct BakePush {
    uint64_t verts;
} BakePush;

static_assert(offsetof(SolidPush, yaw_deg) == 16u, "solid yaw");
static_assert(offsetof(SolidPush, origin_x) == 20u, "solid origin x");
static_assert(offsetof(SolidPush, origin_y) == 24u, "solid origin y");
static_assert(offsetof(SolidPush, depth_bias) == 28u, "solid depth");
static_assert(offsetof(SolidPush, selected) == 32u, "solid selected");
static_assert(sizeof(SolidPush) == 40u, "solid push");
static_assert(sizeof(BakePush) == 8u, "solid bake push");

// [kind][0 faceted, 1 rounded]
static const uint32_t k_first[6][2] = {
    { 0u, 12u },
    { 204u, 228u },
    { 612u, 672u },
    { 1632u, 1704u },
    { 2280u, 2316u },
    { 2604u, 2892u },
};
static const uint32_t k_count[6][2] = {
    { 12u, 192u },
    { 24u, 384u },
    { 60u, 960u },
    { 72u, 576u },
    { 36u, 288u },
    { 288u, 3072u },
};

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkPipelineLayout s_bake_layout = VK_NULL_HANDLE;
static VkPipeline       s_bake = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkDeviceAddress  s_addr = 0;
static bool             s_push2 = false;

static void solid_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Solid Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static int create_da_buffer(VulkanContext *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer *buffer, VkDeviceMemory *memory, VkDeviceAddress *addr) {
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    *addr = 0;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = size,
        .usage = usage,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, buffer);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreateBuffer", res);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetBufferMemoryRequirements(vk->device, *buffer, &reqs);
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        vkDestroyBuffer(vk->device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        fprintf(stderr, "[Solid Error] No device memory for a mesh buffer\n");
        return -1;
    }
    VkMemoryAllocateFlagsInfo alloc_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = &alloc_flags,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, memory);
    if (res != VK_SUCCESS) {
        solid_fail("vkAllocateMemory", res);
        vkDestroyBuffer(vk->device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return -1;
    }
    res = vkBindBufferMemory(vk->device, *buffer, *memory, 0);
    if (res != VK_SUCCESS) {
        solid_fail("vkBindBufferMemory", res);
        vkFreeMemory(vk->device, *memory, nullptr);
        vkDestroyBuffer(vk->device, *buffer, nullptr);
        *memory = VK_NULL_HANDLE;
        *buffer = VK_NULL_HANDLE;
        return -1;
    }
    VkBufferDeviceAddressInfo address_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = *buffer,
    };
    *addr = vkGetBufferDeviceAddress(vk->device, &address_info);
    if (*addr == 0) {
        fprintf(stderr, "[Solid Error] Mesh device address is null\n");
        vkFreeMemory(vk->device, *memory, nullptr);
        vkDestroyBuffer(vk->device, *buffer, nullptr);
        *memory = VK_NULL_HANDLE;
        *buffer = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
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
        solid_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

static int bake_mesh(VulkanContext *vk) {
    if (vk->cmd_pool == VK_NULL_HANDLE || vk->queues.graphics_queue == VK_NULL_HANDLE ||
        s_bake == VK_NULL_HANDLE) {
        fprintf(stderr, "[Solid Error] Mesh bake needs the graphics queue and command pool\n");
        return -1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = vk->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkResult res = vkAllocateCommandBuffers(vk->device, &alloc_info, &cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkAllocateCommandBuffers bake", res);
        return -1;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(cmd, &begin_info);
    if (res != VK_SUCCESS) {
        solid_fail("vkBeginCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bake);
    BakePush bake_push = { .verts = s_addr };
    if (s_push2) {
        VkPushConstantsInfo push = {
            .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
            .layout     = s_bake_layout,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0,
            .size       = sizeof bake_push,
            .pValues    = &bake_push,
        };
        vkCmdPushConstants2(cmd, &push);
    } else {
        vkCmdPushConstants(cmd, s_bake_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof bake_push, &bake_push);
    }
    vkCmdDispatch(cmd, (k_solid_verts + k_local - 1u) / k_local, 1u, 1u);
    VkMemoryBarrier2 mesh = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mesh,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkEndCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    VkCommandBufferSubmitInfo cmd_submit = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit_info = {
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos    = &cmd_submit,
    };
    res = vkQueueSubmit2(vk->queues.graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        solid_fail("vkQueueSubmit2 bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    res = vkQueueWaitIdle(vk->queues.graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
    if (res != VK_SUCCESS) {
        solid_fail("vkQueueWaitIdle bake", res);
        return -1;
    }
    return 0;
}

void solid_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (s_bake != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, s_bake, nullptr);
        }
        if (s_bake_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, s_bake_layout, nullptr);
        }
        if (s_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, s_pipeline, nullptr);
        }
        if (s_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        }
        if (s_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(vk->device, s_buffer, nullptr);
        }
        if (s_memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, s_memory, nullptr);
        }
    }
    s_bake = VK_NULL_HANDLE;
    s_bake_layout = VK_NULL_HANDLE;
    s_pipeline = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    s_buffer = VK_NULL_HANDLE;
    s_memory = VK_NULL_HANDLE;
    s_addr = 0;
}

int solid_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = cube_depth_format();
    VkSampleCountFlagBits samples = cube_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Solid Error] The cube pass has no depth format yet\n");
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(SolidPush)) {
        fprintf(stderr, "[Solid Error] Push constants are %u bytes, a solid needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(SolidPush));
        return -1;
    }
    if (k_first[5][1] + k_count[5][1] != k_solid_verts) {
        fprintf(stderr, "[Solid Error] Mesh ranges do not cover %u verts\n", k_solid_verts);
        return -1;
    }
    s_push2 = vk->device_info.supports_maintenance6;
    if (create_da_buffer(vk, (VkDeviceSize)k_solid_verts * 36u,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         &s_buffer, &s_memory, &s_addr) != 0) {
        solid_shutdown(vk);
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(SolidPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreatePipelineLayout", res);
        solid_shutdown(vk);
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
        solid_shutdown(vk);
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
        .cullMode    = VK_CULL_MODE_BACK_BIT,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo sample_state = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = samples,
    };
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp   = VK_COMPARE_OP_LESS,
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
        solid_fail("vkCreateGraphicsPipelines", res);
        solid_shutdown(vk);
        return -1;
    }

    VkPushConstantRange bake_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(BakePush),
    };
    VkPipelineLayoutCreateInfo bake_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &bake_range,
    };
    res = vkCreatePipelineLayout(vk->device, &bake_layout_info, nullptr, &s_bake_layout);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreatePipelineLayout bake", res);
        solid_shutdown(vk);
        return -1;
    }
    VkShaderModule comp = make_module(vk, (const uint32_t *)k_bake_spv, (uint32_t)sizeof k_bake_spv);
    if (comp == VK_NULL_HANDLE) {
        solid_shutdown(vk);
        return -1;
    }
    VkPipelineShaderStageCreateInfo bake_stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = comp,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo bake_pipe = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = bake_stage,
        .layout = s_bake_layout,
    };
    res = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &bake_pipe, nullptr, &s_bake);
    vkDestroyShaderModule(vk->device, comp, nullptr);
    if (res != VK_SUCCESS) {
        solid_fail("vkCreateComputePipelines", res);
        solid_shutdown(vk);
        return -1;
    }
    if (bake_mesh(vk) != 0) {
        solid_shutdown(vk);
        return -1;
    }
    printf("[Solid] 6 meshes, %u verts, faceted and rounded, baked on the GPU\n", k_solid_verts);
    fflush(stdout);
    return 0;
}

void solid_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                uint32_t kind, bool curved, float yaw_deg,
                float origin_x, float origin_y, bool selected) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || width == 0 || height == 0 ||
        kind >= 6u) {
        return;
    }
    uint32_t cage = curved ? 1u : 0u;
    VkViewport viewport = {
        .x        = 0.0f,
        .y        = 0.0f,
        .width    = (float)width,
        .height   = (float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = { .width = width, .height = height },
    };
    SolidPush draw_push = {
        .verts      = s_addr,
        .width      = width,
        .height     = height,
        .yaw_deg    = yaw_deg,
        .origin_x   = origin_x,
        .origin_y   = origin_y,
        .depth_bias = 0.0f,
        .selected   = selected ? 1.0f : 0.0f,
        .pad        = 0,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    if (s_push2) {
        VkPushConstantsInfo push = {
            .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
            .layout     = s_layout,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset     = 0,
            .size       = sizeof draw_push,
            .pValues    = &draw_push,
        };
        vkCmdPushConstants2(cmd, &push);
    } else {
        vkCmdPushConstants(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof draw_push, &draw_push);
    }
    vkCmdDraw(cmd, k_count[kind][cage], 1, k_first[kind][cage], 0);
}

uint32_t solid_vertex_count(void) {
    return k_solid_verts;
}
