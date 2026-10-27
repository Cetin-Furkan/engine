#include "frame/cube.h"
#include "frame/camera.h"
#include "frame/grid.h"
#include "frame/material.h"
#include "vulkan/memory.h"
#include <stddef.h>
#include <stdio.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "cube.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "cube.frag.spv"
};
alignas(uint32_t) static const unsigned char k_bevel_spv[] = {
#embed "bevel.comp.spv"
};
alignas(uint32_t) static const unsigned char k_heat_spv[] = {
#embed "heat.comp.spv"
};

// Matches shaders/bevel.comp and shaders/face_grid.inc. k_grid is 16.
// The 888 fillet vertices stay. Each face adds a 16 by 16 grid whose
// corners are those fillet vertices. Indices are still one quad's two triangles.
enum {
    k_arc          = 8,
    k_grid         = 16,
    k_fillet_verts = 6 * 4 + 12 * (k_arc + 1) * 2 + 8 * (k_arc + 1) * (k_arc + 1),
    k_face_samples = (k_grid + 1) * (k_grid + 1) - 4,
    k_verts        = k_fillet_verts + 6 * k_face_samples,
    k_face_quads   = 6 * k_grid * k_grid,
    k_quads        = k_face_quads + 12 * k_arc + 8 * k_arc * k_arc,
    k_indices      = k_quads * 6,
    k_local        = 64,
};

static_assert(k_fillet_verts == 888u, "fillet vertex count");
static_assert(k_face_samples == 285u, "face sample count");
static_assert(k_verts == 2598u, "cube vertex count");
static_assert(k_quads == 2144u, "cube quad count");
static_assert(k_indices == 12864u, "cube index count");
static_assert(k_verts < 65536u, "uint16 index");
static_assert(sizeof k_vert_spv % 4u == 0u, "vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "frag spirv");
static_assert(sizeof k_bevel_spv % 4u == 0u, "bevel spirv");
static_assert(sizeof k_heat_spv % 4u == 0u, "heat spirv");

typedef struct DrawPush {
    uint64_t verts;
    uint32_t width;
    uint32_t height;
    float    yaw_deg;
    float    origin_x;
    float    origin_y;
    float    depth_bias;
    uint64_t heat;
    float    phase;
    float    selected;
    uint64_t grid;
    ViewPush view;
} DrawPush;

enum { k_draw_push_bytes = 104 };

// Three copies so a frame still reading one buffer is not overwritten by the
// next step. Two slots can be in flight. The third is the one being written.
// The spare wait retires the image from two presents back. It does not make
// this step's write visible to the next submit. That submit waits
// s_heat_published.
enum { k_heat_copies = 3 };

typedef struct MeshPush {
    uint64_t verts;
    uint64_t indices;
} MeshPush;

typedef struct HeatPush {
    uint64_t src;
    uint64_t dst;
    uint64_t verts;
    uint32_t dt_ms;
    uint32_t seed;
    uint32_t poke;
    float    poke_x;
    float    poke_y;
    float    yaw_deg;
} HeatPush;

static_assert(offsetof(DrawPush, yaw_deg) == 16u, "yaw offset");
static_assert(offsetof(DrawPush, origin_x) == 20u, "origin x");
static_assert(offsetof(DrawPush, origin_y) == 24u, "origin y");
static_assert(offsetof(DrawPush, depth_bias) == 28u, "depth bias");
static_assert(offsetof(DrawPush, heat) == 32u, "heat address");
static_assert(offsetof(DrawPush, phase) == 40u, "hue phase");
static_assert(offsetof(DrawPush, selected) == 44u, "cube selected");
static_assert(offsetof(DrawPush, grid) == 48u, "grid address");
static_assert(offsetof(DrawPush, view) == 56u, "cube view");
static_assert(offsetof(ViewPush, qx) == 16u, "view quat");
static_assert(sizeof(ViewPush) == 48u, "view push");
static_assert(sizeof(DrawPush) == k_draw_push_bytes, "draw push");
static_assert(sizeof(MeshPush) == 16u, "mesh push");
static_assert(offsetof(HeatPush, verts) == 16u, "heat verts");
static_assert(offsetof(HeatPush, dt_ms) == 24u, "heat dt");
static_assert(offsetof(HeatPush, seed) == 28u, "heat seed");
static_assert(offsetof(HeatPush, poke) == 32u, "heat poke");
static_assert(offsetof(HeatPush, poke_x) == 36u, "heat poke x");
static_assert(offsetof(HeatPush, poke_y) == 40u, "heat poke y");
static_assert(offsetof(HeatPush, yaw_deg) == 44u, "heat yaw");
static_assert(sizeof(HeatPush) == 48u, "heat push");

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkPipelineLayout s_compute_layout = VK_NULL_HANDLE;
static VkPipeline       s_compute = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkBuffer         s_index = VK_NULL_HANDLE;
static VkDeviceMemory   s_index_memory = VK_NULL_HANDLE;
static VkDeviceAddress  s_mesh_addr = 0;
static VkDeviceAddress  s_index_addr = 0;
static VkPipelineLayout s_heat_layout = VK_NULL_HANDLE;
static VkPipeline       s_heat_pipe = VK_NULL_HANDLE;
static VkBuffer         s_heat_buf[k_heat_copies] = {0};
static VkDeviceMemory   s_heat_mem[k_heat_copies] = {0};
static VkDeviceAddress  s_heat_addr[k_heat_copies] = {0};
static int              s_heat_read = 0;
static int              s_heat_draw = -1;
static bool             s_heat_have_stamp = false;
static uint32_t         s_heat_stamp_ms = 0;
static bool             s_heat_started = false;
static bool             s_heat_held = false;
static uint64_t         s_heat_published = 0;
static float            s_poke_x = 0.0f;
static float            s_poke_y = 0.0f;
static bool             s_poke_pending = false;
static bool             s_poke_armed_cb = false;
static bool             s_pulse = false;
static bool             s_pulse_have_t0 = false;
static uint32_t         s_pulse_t0 = 0;
static VkFormat             s_depth_format = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_samples = VK_SAMPLE_COUNT_1_BIT;
static void cube_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Cube Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static bool depth_format_ok(VulkanContext *vk, VkFormat format) {
    VkFormatProperties props = {0};
    vkGetPhysicalDeviceFormatProperties(vk->physical_device, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

static VkSampleCountFlags image_sample_counts(VulkanContext *vk, VkFormat format,
                                              VkImageUsageFlags usage) {
    VkImageFormatProperties props = {0};
    VkResult res = vkGetPhysicalDeviceImageFormatProperties(
        vk->physical_device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        usage, 0, &props);
    if (res != VK_SUCCESS) {
        return 0;
    }
    return props.sampleCounts;
}

static VkSampleCountFlagBits pick_samples(VkSampleCountFlags counts) {
    if ((counts & VK_SAMPLE_COUNT_8_BIT) != 0) {
        return VK_SAMPLE_COUNT_8_BIT;
    }
    if ((counts & VK_SAMPLE_COUNT_4_BIT) != 0) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if ((counts & VK_SAMPLE_COUNT_2_BIT) != 0) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

static uint32_t sample_count_n(VkSampleCountFlagBits samples) {
    if (samples == VK_SAMPLE_COUNT_8_BIT) {
        return 8u;
    }
    if (samples == VK_SAMPLE_COUNT_4_BIT) {
        return 4u;
    }
    if (samples == VK_SAMPLE_COUNT_2_BIT) {
        return 2u;
    }
    return 1u;
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
        cube_fail("vkCreateBuffer", res);
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
        fprintf(stderr, "[Cube Error] No device memory for a mesh buffer\n");
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
        cube_fail("vkAllocateMemory", res);
        vkDestroyBuffer(vk->device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return -1;
    }
    res = vkBindBufferMemory(vk->device, *buffer, *memory, 0);
    if (res != VK_SUCCESS) {
        cube_fail("vkBindBufferMemory", res);
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
        fprintf(stderr, "[Cube Error] Mesh device address is null\n");
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
        cube_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

// This command buffer only. The vertex read of the buffer just written is
// in the draw that follows. The next submit's compute read is ordered by
// waiting s_heat_published, not by widening this barrier.
static void barrier_heat_to_vertex(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void dispatch_heat(VkCommandBuffer cmd, uint32_t dt_ms, uint32_t seed,
                          int src_slot, int dst_slot, uint32_t poke, float yaw_deg) {
    HeatPush heat_push = {
        .src     = src_slot >= 0 ? s_heat_addr[src_slot] : 0,
        .dst     = s_heat_addr[dst_slot],
        .verts   = s_mesh_addr,
        .dt_ms   = dt_ms,
        .seed    = seed,
        .poke    = poke,
        .poke_x  = poke != 0u ? s_poke_x : 0.0f,
        .poke_y  = poke != 0u ? s_poke_y : 0.0f,
        .yaw_deg = yaw_deg,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_heat_pipe);
    vulkan_cmd_push(cmd, s_heat_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof heat_push, &heat_push);
    vkCmdDispatch(cmd, (k_verts + k_local - 1u) / k_local, 1u, 1u);
    barrier_heat_to_vertex(cmd);
}

// One submit, then the queue is idle. The seed is visible without an acquire
// wait. A live step publishes the acquire value of its own submit.
static int bake_mesh(VulkanContext *vk) {
    if (vk->cmd_pool == VK_NULL_HANDLE || vk->queues.graphics_queue == VK_NULL_HANDLE ||
        s_compute == VK_NULL_HANDLE) {
        fprintf(stderr, "[Cube Error] Mesh bake needs the graphics queue and command pool\n");
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
        cube_fail("vkAllocateCommandBuffers bake", res);
        return -1;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(cmd, &begin_info);
    if (res != VK_SUCCESS) {
        cube_fail("vkBeginCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_compute);
    MeshPush mesh_push = {
        .verts   = s_mesh_addr,
        .indices = s_index_addr,
    };
    vulkan_cmd_push(cmd, s_compute_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof mesh_push, &mesh_push);
    vkCmdDispatch(cmd, (k_verts + k_local - 1u) / k_local, 1, 1);
    VkMemoryBarrier2 mesh = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mesh,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    if (s_heat_pipe == VK_NULL_HANDLE || s_heat_addr[0] == 0) {
        fprintf(stderr, "[Cube Error] Heat seed needs its pipeline and buffer\n");
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    dispatch_heat(cmd, 0, 1u, -1, 0, 0u, 0.0f);
    s_heat_read = 0;
    s_heat_draw = -1;
    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        cube_fail("vkEndCommandBuffer bake", res);
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
        cube_fail("vkQueueSubmit2 bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    res = vkQueueWaitIdle(vk->queues.graphics_queue);
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
    if (res != VK_SUCCESS) {
        cube_fail("vkQueueWaitIdle bake", res);
        return -1;
    }
    return 0;
}

int cube_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    if (depth_format_ok(vk, VK_FORMAT_D32_SFLOAT)) {
        s_depth_format = VK_FORMAT_D32_SFLOAT;
    } else if (depth_format_ok(vk, VK_FORMAT_D24_UNORM_S8_UINT)) {
        s_depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    } else if (depth_format_ok(vk, VK_FORMAT_D16_UNORM)) {
        s_depth_format = VK_FORMAT_D16_UNORM;
    } else {
        fprintf(stderr, "[Cube Error] No depth attachment format\n");
        return -1;
    }

    VkSampleCountFlags color_counts = image_sample_counts(
        vk, VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VkSampleCountFlags depth_counts = image_sample_counts(
        vk, s_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    s_samples = pick_samples(color_counts & depth_counts);

    if (create_da_buffer(vk, (VkDeviceSize)k_verts * 36u,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         &s_buffer, &s_memory, &s_mesh_addr) != 0 ||
        create_da_buffer(vk, (VkDeviceSize)k_indices * sizeof(uint16_t),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         &s_index, &s_index_memory, &s_index_addr) != 0) {
        cube_shutdown(vk);
        return -1;
    }
    s_heat_read = 0;
    s_heat_draw = -1;
    s_heat_have_stamp = false;
    s_heat_stamp_ms = 0;
    s_heat_started = false;
    s_heat_held = false;
    s_heat_published = 0;
    s_poke_x = 0.0f;
    s_poke_y = 0.0f;
    s_poke_pending = false;
    s_poke_armed_cb = false;
    s_pulse = false;
    s_pulse_have_t0 = false;
    s_pulse_t0 = 0;
    for (int i = 0; i < k_heat_copies; ++i) {
        if (create_da_buffer(vk, (VkDeviceSize)k_verts * sizeof(float),
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             &s_heat_buf[i], &s_heat_mem[i], &s_heat_addr[i]) != 0) {
            cube_shutdown(vk);
            return -1;
        }
    }

    VkResult res = VK_SUCCESS;
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = k_draw_push_bytes,
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreatePipelineLayout", res);
        cube_shutdown(vk);
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
        cube_shutdown(vk);
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
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = s_samples,
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
        .depthAttachmentFormat   = s_depth_format,
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
    res = vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipe, nullptr, &s_pipeline);
    vkDestroyShaderModule(vk->device, vert, nullptr);
    vkDestroyShaderModule(vk->device, frag, nullptr);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateGraphicsPipelines", res);
        cube_shutdown(vk);
        return -1;
    }

    VkPushConstantRange compute_push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(MeshPush),
    };
    VkPipelineLayoutCreateInfo compute_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &compute_push,
    };
    res = vkCreatePipelineLayout(vk->device, &compute_layout_info, nullptr, &s_compute_layout);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreatePipelineLayout compute", res);
        cube_shutdown(vk);
        return -1;
    }
    VkShaderModule comp = make_module(vk, (const uint32_t *)k_bevel_spv, (uint32_t)sizeof k_bevel_spv);
    if (comp == VK_NULL_HANDLE) {
        cube_shutdown(vk);
        return -1;
    }
    VkPipelineShaderStageCreateInfo compute_stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = comp,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo compute_pipe = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = compute_stage,
        .layout = s_compute_layout,
    };
    res = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &compute_pipe, nullptr, &s_compute);
    vkDestroyShaderModule(vk->device, comp, nullptr);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateComputePipelines", res);
        cube_shutdown(vk);
        return -1;
    }
    VkPushConstantRange heat_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(HeatPush),
    };
    VkPipelineLayoutCreateInfo heat_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &heat_range,
    };
    res = vkCreatePipelineLayout(vk->device, &heat_layout_info, nullptr, &s_heat_layout);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreatePipelineLayout heat", res);
        cube_shutdown(vk);
        return -1;
    }
    VkShaderModule heat = make_module(vk, (const uint32_t *)k_heat_spv, (uint32_t)sizeof k_heat_spv);
    if (heat == VK_NULL_HANDLE) {
        cube_shutdown(vk);
        return -1;
    }
    VkPipelineShaderStageCreateInfo heat_stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = heat,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo heat_pipe = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = heat_stage,
        .layout = s_heat_layout,
    };
    res = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &heat_pipe, nullptr, &s_heat_pipe);
    vkDestroyShaderModule(vk->device, heat, nullptr);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateComputePipelines heat", res);
        cube_shutdown(vk);
        return -1;
    }
    if (bake_mesh(vk) != 0) {
        cube_shutdown(vk);
        return -1;
    }
    printf("[Cube] 240px centered, %u verts, 12px GPU fillet, %u by %u face grid, %ux edge samples, directional sun\n",
           k_verts, k_grid, k_grid, sample_count_n(s_samples));
    fflush(stdout);
    return 0;
}

static void release_heat_copies(VulkanContext *vk, bool destroy) {
    for (int i = 0; i < k_heat_copies; ++i) {
        if (destroy && vk != nullptr && vk->device != VK_NULL_HANDLE) {
            if (s_heat_buf[i] != VK_NULL_HANDLE) {
                vkDestroyBuffer(vk->device, s_heat_buf[i], nullptr);
            }
            if (s_heat_mem[i] != VK_NULL_HANDLE) {
                vkFreeMemory(vk->device, s_heat_mem[i], nullptr);
            }
        }
        s_heat_buf[i] = VK_NULL_HANDLE;
        s_heat_mem[i] = VK_NULL_HANDLE;
        s_heat_addr[i] = 0;
    }
    s_heat_read = 0;
    s_heat_draw = -1;
    s_heat_have_stamp = false;
    s_heat_stamp_ms = 0;
    s_heat_started = false;
    s_heat_held = false;
    s_heat_published = 0;
    s_poke_x = 0.0f;
    s_poke_y = 0.0f;
    s_poke_pending = false;
    s_poke_armed_cb = false;
    s_pulse = false;
    s_pulse_have_t0 = false;
    s_pulse_t0 = 0;
}

void cube_shutdown(VulkanContext *vk) {
    s_samples = VK_SAMPLE_COUNT_1_BIT;
    s_mesh_addr = 0;
    s_index_addr = 0;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_pipeline = VK_NULL_HANDLE;
        s_layout = VK_NULL_HANDLE;
        s_compute = VK_NULL_HANDLE;
        s_compute_layout = VK_NULL_HANDLE;
        s_heat_pipe = VK_NULL_HANDLE;
        s_heat_layout = VK_NULL_HANDLE;
        s_buffer = VK_NULL_HANDLE;
        s_memory = VK_NULL_HANDLE;
        s_index = VK_NULL_HANDLE;
        s_index_memory = VK_NULL_HANDLE;
        release_heat_copies(vk, false);
        return;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_pipeline, nullptr);
        s_pipeline = VK_NULL_HANDLE;
    }
    if (s_compute != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_compute, nullptr);
        s_compute = VK_NULL_HANDLE;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
    if (s_compute_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_compute_layout, nullptr);
        s_compute_layout = VK_NULL_HANDLE;
    }
    if (s_heat_pipe != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_heat_pipe, nullptr);
        s_heat_pipe = VK_NULL_HANDLE;
    }
    if (s_heat_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_heat_layout, nullptr);
        s_heat_layout = VK_NULL_HANDLE;
    }
    release_heat_copies(vk, true);
    if (s_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, s_buffer, nullptr);
        s_buffer = VK_NULL_HANDLE;
    }
    if (s_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, s_memory, nullptr);
        s_memory = VK_NULL_HANDLE;
    }
    if (s_index != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, s_index, nullptr);
        s_index = VK_NULL_HANDLE;
    }
    if (s_index_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, s_index_memory, nullptr);
        s_index_memory = VK_NULL_HANDLE;
    }
}

void cube_depth_destroy(VulkanContext *vk, CubeDepth *depth) {
    if (depth == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (depth->view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, depth->view, nullptr);
        }
        if (depth->image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, depth->image, nullptr);
        }
        if (depth->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, depth->memory, nullptr);
        }
    }
    *depth = (CubeDepth){0};
}

int cube_depth_create(VulkanContext *vk, CubeDepth *depth, uint32_t width, uint32_t height) {
    if (vk == nullptr || depth == nullptr || width == 0 || height == 0 ||
        s_depth_format == VK_FORMAT_UNDEFINED) {
        return -1;
    }
    cube_depth_destroy(vk, depth);
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = s_depth_format,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = s_samples,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &depth->image);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateImage depth", res);
        cube_depth_destroy(vk, depth);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetImageMemoryRequirements(vk->device, depth->image, &reqs);
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        fprintf(stderr, "[Cube Error] No memory for the depth image\n");
        cube_depth_destroy(vk, depth);
        return -1;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &depth->memory);
    if (res != VK_SUCCESS) {
        cube_fail("vkAllocateMemory depth", res);
        cube_depth_destroy(vk, depth);
        return -1;
    }
    res = vkBindImageMemory(vk->device, depth->image, depth->memory, 0);
    if (res != VK_SUCCESS) {
        cube_fail("vkBindImageMemory depth", res);
        cube_depth_destroy(vk, depth);
        return -1;
    }
    VkImageViewCreateInfo view = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = depth->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = s_depth_format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    res = vkCreateImageView(vk->device, &view, nullptr, &depth->view);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateImageView depth", res);
        cube_depth_destroy(vk, depth);
        return -1;
    }
    return 0;
}

void cube_coverage_destroy(VulkanContext *vk, CubeCoverage *coverage) {
    if (coverage == nullptr) {
        return;
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (coverage->view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, coverage->view, nullptr);
        }
        if (coverage->image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, coverage->image, nullptr);
        }
        if (coverage->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, coverage->memory, nullptr);
        }
    }
    *coverage = (CubeCoverage){0};
}

int cube_coverage_create(VulkanContext *vk, CubeCoverage *coverage,
                         uint32_t width, uint32_t height) {
    if (vk == nullptr || coverage == nullptr || width == 0 || height == 0) {
        return -1;
    }
    cube_coverage_destroy(vk, coverage);
    if (s_samples == VK_SAMPLE_COUNT_1_BIT) {
        return 0;
    }
    VkImageCreateInfo image_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_B8G8R8A8_SRGB,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = s_samples,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(vk->device, &image_info, nullptr, &coverage->image);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateImage coverage", res);
        cube_coverage_destroy(vk, coverage);
        return -1;
    }
    VkMemoryRequirements reqs = {0};
    vkGetImageMemoryRequirements(vk->device, coverage->image, &reqs);
    uint32_t type_index = 0;
    VulkanResult mem = vulkan_find_memory_type(
        vk, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index);
    if (mem != VULKAN_SUCCESS) {
        mem = vulkan_find_memory_type(vk, reqs.memoryTypeBits, 0, &type_index);
    }
    if (mem != VULKAN_SUCCESS) {
        fprintf(stderr, "[Cube Error] No memory for the coverage image\n");
        cube_coverage_destroy(vk, coverage);
        return -1;
    }
    VkMemoryAllocateInfo alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = type_index,
    };
    res = vkAllocateMemory(vk->device, &alloc, nullptr, &coverage->memory);
    if (res != VK_SUCCESS) {
        cube_fail("vkAllocateMemory coverage", res);
        cube_coverage_destroy(vk, coverage);
        return -1;
    }
    res = vkBindImageMemory(vk->device, coverage->image, coverage->memory, 0);
    if (res != VK_SUCCESS) {
        cube_fail("vkBindImageMemory coverage", res);
        cube_coverage_destroy(vk, coverage);
        return -1;
    }
    VkImageViewCreateInfo view = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = coverage->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_B8G8R8A8_SRGB,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    res = vkCreateImageView(vk->device, &view, nullptr, &coverage->view);
    if (res != VK_SUCCESS) {
        cube_fail("vkCreateImageView coverage", res);
        cube_coverage_destroy(vk, coverage);
        return -1;
    }
    return 0;
}

void cube_prepare_draw(VkCommandBuffer cmd, VkImage depth) {
    if (cmd == VK_NULL_HANDLE || depth == VK_NULL_HANDLE) {
        return;
    }
    VkImageMemoryBarrier2 depth_to_attach = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = depth,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &depth_to_attach,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

void cube_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, float yaw_deg,
               float origin_x, float origin_y, float depth_bias, float phase, bool selected) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
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
    int heat_slot = s_heat_draw >= 0 ? s_heat_draw : s_heat_read;
    DrawPush draw_push = {
        .verts      = s_mesh_addr,
        .width      = width,
        .height     = height,
        .yaw_deg    = yaw_deg,
        .origin_x   = origin_x,
        .origin_y   = origin_y,
        .depth_bias = depth_bias,
        .heat       = s_heat_addr[heat_slot],
        .phase      = phase,
        .selected   = selected ? 1.0f : 0.0f,
        .grid       = grid_tint_addr(),
    };
    view_push_fill(&draw_push.view, k_mat_cube);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vkCmdBindIndexBuffer(cmd, s_index, 0, VK_INDEX_TYPE_UINT16);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, k_draw_push_bytes, &draw_push);
    vkCmdDrawIndexed(cmd, k_indices, 1, 0, 0, 0);
}

VkFormat cube_depth_format(void) {
    return s_depth_format;
}

VkSampleCountFlagBits cube_raster_samples(void) {
    return s_samples;
}

uint32_t cube_vertex_count(void) {
    return k_verts;
}

uint64_t cube_mesh_addr(void) {
    return s_mesh_addr;
}

uint32_t cube_heat_interval(uint32_t time_ms) {
    if (s_heat_held) {
        return 0;
    }
    if (!s_heat_have_stamp) {
        s_heat_have_stamp = true;
        s_heat_stamp_ms = time_ms;
        return 0;
    }
    uint32_t delta = time_ms - s_heat_stamp_ms;
    // A gap between revolutions is not a heat step.
    if (delta == 0u || delta > 1000u) {
        s_heat_stamp_ms = time_ms;
        return 0;
    }
    return delta;
}

void cube_heat_commit_stamp(uint32_t time_ms) {
    if (s_heat_held) {
        return;
    }
    s_heat_have_stamp = true;
    s_heat_stamp_ms = time_ms;
}

void cube_heat_record(VkCommandBuffer cmd, uint32_t dt_ms, float yaw_deg) {
    bool poke = s_poke_pending;
    if (cmd == VK_NULL_HANDLE || (dt_ms == 0u && !poke) || s_heat_held || s_heat_draw >= 0 ||
        s_heat_pipe == VK_NULL_HANDLE) {
        return;
    }
    int dst = (s_heat_read + 1) % k_heat_copies;
    if (s_heat_addr[s_heat_read] == 0 || s_heat_addr[dst] == 0 || s_mesh_addr == 0) {
        return;
    }
    dispatch_heat(cmd, dt_ms, 0u, s_heat_read, dst, poke ? 1u : 0u, yaw_deg);
    s_heat_draw = dst;
    if (poke) {
        s_poke_pending = false;
        s_poke_armed_cb = true;
    }
}

void cube_heat_record_ok(uint64_t signal_point) {
    if (s_heat_draw < 0) {
        return;
    }
    s_heat_read = s_heat_draw;
    s_heat_draw = -1;
    s_poke_armed_cb = false;
    if (signal_point != 0) {
        s_heat_published = signal_point;
    }
    if (s_heat_started) {
        return;
    }
    s_heat_started = true;
    printf("[Cube] Fillet heat started. Upper half, callback step, then it holds.\n");
    fflush(stdout);
}

void cube_heat_record_abort(void) {
    s_heat_draw = -1;
    if (s_poke_armed_cb) {
        s_poke_pending = true;
        s_poke_armed_cb = false;
    }
}

bool cube_heat_poke(float origin_x, float origin_y) {
    if (s_heat_pipe == VK_NULL_HANDLE) {
        return false;
    }
    s_poke_x = origin_x;
    s_poke_y = origin_y;
    s_poke_pending = true;
    s_heat_held = false;
    s_pulse = true;
    s_pulse_have_t0 = false;
    printf("[Cube] Fillet heat poked at %+.0f,%+.0f.\n", (double)origin_x, (double)origin_y);
    fflush(stdout);
    return true;
}

bool cube_heat_poke_pending(void) {
    return s_poke_pending;
}

bool cube_heat_wants_frame(uint32_t time_ms, bool *ended) {
    if (ended != nullptr) {
        *ended = false;
    }
    if (!s_pulse) {
        return false;
    }
    if (!s_pulse_have_t0) {
        s_pulse_t0 = time_ms;
        s_pulse_have_t0 = true;
        return true;
    }
    if (time_ms - s_pulse_t0 >= 5000u) {
        s_pulse = false;
        s_pulse_have_t0 = false;
        if (ended != nullptr) {
            *ended = true;
        }
        return false;
    }
    return true;
}

uint64_t cube_heat_published(void) {
    return s_heat_published;
}

bool cube_heat_step_open(void) {
    return s_heat_draw >= 0;
}

void cube_heat_hold(void) {
    if (s_heat_held) {
        return;
    }
    s_heat_held = true;
    s_pulse = false;
    s_pulse_have_t0 = false;
    s_poke_pending = false;
    s_poke_armed_cb = false;
    if (!s_heat_started) {
        return;
    }
    printf("[Cube] Fillet heat holds.\n");
    fflush(stdout);
}
