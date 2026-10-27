#include "frame/shape.h"
#include "frame/camera.h"
#include "frame/pass.h"
#include "vulkan/memory.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "shape.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "shape.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "shape vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "shape frag spirv");

// Matches shaders/shape.vert. Scalar layout, no padding. The view tail is
// the same block every other draw push ends with.
typedef struct ShapePush {
    uint64_t points;
    uint64_t indices;
    uint32_t count;
    uint32_t mode;
    uint32_t closed;
    float    half;
    float    center_x;
    float    center_y;
    float    angle_rad;
    float    depth;
    uint32_t width;
    uint32_t height;
    float    r, g, b, a;
    ViewPush view;
} ShapePush;

static_assert(offsetof(ShapePush, points) == 0u, "shape points");
static_assert(offsetof(ShapePush, indices) == 8u, "shape indices");
static_assert(offsetof(ShapePush, count) == 16u, "shape count");
static_assert(offsetof(ShapePush, mode) == 20u, "shape mode");
static_assert(offsetof(ShapePush, closed) == 24u, "shape closed");
static_assert(offsetof(ShapePush, half) == 28u, "shape half");
static_assert(offsetof(ShapePush, center_x) == 32u, "shape center");
static_assert(offsetof(ShapePush, center_y) == 36u, "shape center y");
static_assert(offsetof(ShapePush, angle_rad) == 40u, "shape angle");
static_assert(offsetof(ShapePush, depth) == 44u, "shape depth");
static_assert(offsetof(ShapePush, width) == 48u, "shape width");
static_assert(offsetof(ShapePush, height) == 52u, "shape height");
static_assert(offsetof(ShapePush, r) == 56u, "shape color");
static_assert(offsetof(ShapePush, view) == 72u, "shape view");
static_assert(sizeof(ViewPush) == 48u, "shape view bytes");
static_assert(sizeof(ShapePush) == 120u, "shape push");

// One slice per submit still on the GPU. The turn records the next image
// while the shown image's command buffer can still be reading its points.
// Twelve covers both slots and the orphan ring. A slice is free once the
// acquire timeline has reached the point that submitted it.
enum : uint32_t {
    k_poly_points = 64u,
    k_poly_tris   = 62u,
    k_slice_count = 12u,
    k_slice_bytes = 65536u,
};

static_assert(k_slice_bytes % 8u == 0u, "slice align");

typedef struct ShapeSlice {
    uint64_t point;
    uint32_t used;
} ShapeSlice;

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkDevice         s_device = VK_NULL_HANDLE;
static VkDeviceAddress  s_addr = 0;
static uint8_t         *s_map = nullptr;
static ShapeSlice       s_slice[k_slice_count];
static int              s_active = -1;
static bool             s_geom_warned = false;
static bool             s_busy_warned = false;

static void shape_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Shape Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
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
        shape_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

static void shape_geom_reset(void) {
    s_map = nullptr;
    s_addr = 0;
    s_active = -1;
    s_device = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        s_slice[i] = (ShapeSlice){0};
    }
}

void shape_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
        s_map != nullptr && s_memory != VK_NULL_HANDLE) {
        vkUnmapMemory(vk->device, s_memory);
    }
    shape_geom_reset();
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_pipeline = VK_NULL_HANDLE;
        s_buffer = VK_NULL_HANDLE;
        s_memory = VK_NULL_HANDLE;
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
    if (s_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, s_buffer, nullptr);
        s_buffer = VK_NULL_HANDLE;
    }
    if (s_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, s_memory, nullptr);
        s_memory = VK_NULL_HANDLE;
    }
}

static int shape_geom_init(VulkanContext *vk) {
    VkDeviceSize bytes = (VkDeviceSize)k_slice_count * k_slice_bytes;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = bytes,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &s_buffer);
    if (res != VK_SUCCESS) {
        s_buffer = VK_NULL_HANDLE;
        shape_fail("vkCreateBuffer", res);
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
        fprintf(stderr, "[Shape Error] No host-visible memory for face geometry\n");
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
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &s_memory);
    if (res != VK_SUCCESS) {
        shape_fail("vkAllocateMemory", res);
        return -1;
    }
    res = vkBindBufferMemory(vk->device, s_buffer, s_memory, 0);
    if (res != VK_SUCCESS) {
        shape_fail("vkBindBufferMemory", res);
        return -1;
    }
    void *mapped = nullptr;
    res = vkMapMemory(vk->device, s_memory, 0, reqs.size, 0, &mapped);
    if (res != VK_SUCCESS || mapped == nullptr) {
        shape_fail("vkMapMemory", res);
        return -1;
    }
    s_map = mapped;
    memset(s_map, 0, (size_t)bytes);
    VkBufferDeviceAddressInfo address_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = s_buffer,
    };
    s_addr = vkGetBufferDeviceAddress(vk->device, &address_info);
    if (s_addr == 0 || (s_addr & 7u) != 0u) {
        fprintf(stderr, "[Shape Error] Face geometry device address is null\n");
        return -1;
    }
    s_device = vk->device;
    return 0;
}

static void *geom_alloc(uint32_t bytes, uint32_t align) {
    if (s_active < 0 || s_map == nullptr || bytes == 0u || align == 0u) {
        return nullptr;
    }
    ShapeSlice *slice = &s_slice[s_active];
    uint32_t mask = align - 1u;
    uint32_t aligned = (slice->used + mask) & ~mask;
    if (aligned > k_slice_bytes || bytes > k_slice_bytes - aligned) {
        if (!s_geom_warned) {
            fprintf(stderr, "[Shape Error] Face geometry buffer is full\n");
            s_geom_warned = true;
        }
        return nullptr;
    }
    uint32_t offset = (uint32_t)s_active * k_slice_bytes + aligned;
    slice->used = aligned + bytes;
    return s_map + offset;
}

static uint64_t geom_addr(const void *p) {
    return s_addr + (uint64_t)((const uint8_t *)p - s_map);
}

int shape_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Shape Error] The render pass has no depth format yet\n");
        return -1;
    }

    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(ShapePush)) {
        fprintf(stderr, "[Shape Error] Push constants are %u bytes, the fill needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(ShapePush));
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(ShapePush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        shape_fail("vkCreatePipelineLayout", res);
        shape_shutdown(vk);
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
        shape_shutdown(vk);
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
        s_pipeline = VK_NULL_HANDLE;
        shape_fail("vkCreateGraphicsPipelines", res);
        shape_shutdown(vk);
        return -1;
    }
    if (shape_geom_init(vk) != 0) {
        shape_shutdown(vk);
        return -1;
    }
    printf("[Shape] Polygon fill, stroke, and marks from one point buffer\n");
    fflush(stdout);
    return 0;
}

void shape_begin(VkSemaphore acquire, uint64_t signal_point) {
    s_active = -1;
    if (s_map == nullptr || signal_point == 0u) {
        return;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            s_slice[i].used = 0;
            s_active = (int)i;
            return;
        }
    }
    uint64_t counter = 0;
    bool have_counter = false;
    if (s_device != VK_NULL_HANDLE && acquire != VK_NULL_HANDLE &&
        vkGetSemaphoreCounterValue(s_device, acquire, &counter) == VK_SUCCESS) {
        have_counter = true;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == 0u || (have_counter && counter >= s_slice[i].point)) {
            s_slice[i].point = signal_point;
            s_slice[i].used = 0;
            s_active = (int)i;
            return;
        }
    }
    if (!s_busy_warned) {
        fprintf(stderr, "[Shape Error] Face geometry is still on the GPU\n");
        s_busy_warned = true;
    }
}

void shape_release(uint64_t signal_point) {
    if (signal_point == 0u) {
        return;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            s_slice[i].point = 0;
            s_slice[i].used = 0;
        }
    }
    if (s_active >= 0 && (uint32_t)s_active < k_slice_count && s_slice[s_active].point == 0u) {
        s_active = -1;
    }
}

static void shape_push_draw(VkCommandBuffer cmd, const ShapePush *push,
                            uint32_t vertices, uint32_t instances) {
    if (instances == 0u || vertices == 0u) {
        return;
    }
    vulkan_cmd_push(cmd, s_layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    (uint32_t)sizeof *push, push);
    vkCmdDraw(cmd, vertices, instances, 0, 0);
}

void shape_draw_polygon(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                        const float *x, const float *y, uint32_t n,
                        const uint16_t *idx, uint32_t tri_n, bool closed,
                        float center_x, float center_y, float angle_rad,
                        float fill_depth, float edge_depth, float mark_depth,
                        float fill_r, float fill_g, float fill_b,
                        float edge_r, float edge_g, float edge_b,
                        float stroke_half, float mark_half, uint32_t mat_index) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE ||
        s_active < 0 || x == nullptr || y == nullptr || width == 0u || height == 0u ||
        n == 0u || n > k_poly_points || tri_n > k_poly_tris) {
        return;
    }
    float *pts = geom_alloc(n * 8u, 8u);
    if (pts == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        pts[i * 2u] = x[i];
        pts[i * 2u + 1u] = y[i];
    }
    uint64_t indices_addr = 0;
    if (tri_n > 0u && idx != nullptr) {
        uint32_t *ix = geom_alloc(tri_n * 3u * 4u, 4u);
        if (ix == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < tri_n * 3u; ++i) {
            ix[i] = idx[i];
        }
        indices_addr = geom_addr(ix);
    }

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
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);

    ShapePush push = {
        .points    = geom_addr(pts),
        .indices   = indices_addr,
        .count     = n,
        .closed    = closed ? 1u : 0u,
        .center_x  = center_x,
        .center_y  = center_y,
        .angle_rad = angle_rad,
        .width     = width,
        .height    = height,
        .a         = 1.0f,
    };
    view_push_fill(&push.view, mat_index);

    if (tri_n > 0u && indices_addr != 0u) {
        push.mode = 0;
        push.half = 0.0f;
        push.depth = fill_depth;
        push.r = fill_r;
        push.g = fill_g;
        push.b = fill_b;
        shape_push_draw(cmd, &push, 3u, tri_n);
    }
    uint32_t edges = 0;
    if (n >= 2u) {
        edges = closed ? n : n - 1u;
    }
    if (edges > 0u) {
        push.mode = 1;
        push.half = stroke_half;
        push.depth = edge_depth;
        push.r = edge_r;
        push.g = edge_g;
        push.b = edge_b;
        shape_push_draw(cmd, &push, 6u, edges);
    }
    push.mode = 2;
    push.half = mark_half;
    push.depth = mark_depth;
    push.r = edge_r;
    push.g = edge_g;
    push.b = edge_b;
    shape_push_draw(cmd, &push, 6u, n);
}
