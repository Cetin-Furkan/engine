#include "frame/mesh.h"
#include "frame/plot_file.h"
#include "frame/camera.h"
#include "frame/cube.h"
#include "frame/material.h"
#include "vulkan/memory.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "mesh.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "mesh.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "mesh vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "mesh frag spirv");

// The box sits above the cosine chart and left of the icosahedron.
// Z is centered on 0 so the click sphere does not need a center z.
static const float k_fit_x  = 280.0f;
static const float k_fit_y  = 170.0f;
static const float k_fit_hx = 90.0f;
static const float k_fit_hy = 70.0f;
static const float k_fit_hz = 48.0f;

typedef struct MeshPush {
    uint64_t verts;
    uint32_t width;
    uint32_t height;
    float    yaw_deg;
    float    origin_x;
    float    origin_y;
    float    depth_bias;
    float    selected;
    uint32_t pad;
    uint32_t tail0;
    uint32_t tail1;
    ViewPush view;
} MeshPush;

static_assert(offsetof(MeshPush, yaw_deg) == 16u, "mesh yaw");
static_assert(offsetof(MeshPush, origin_x) == 20u, "mesh origin x");
static_assert(offsetof(MeshPush, origin_y) == 24u, "mesh origin y");
static_assert(offsetof(MeshPush, depth_bias) == 28u, "mesh depth");
static_assert(offsetof(MeshPush, selected) == 32u, "mesh selected");
static_assert(offsetof(MeshPush, view) == 48u, "mesh view");
static_assert(sizeof(MeshPush) == 96u, "mesh push");

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkDeviceAddress  s_addr = 0;
static VkQueue          s_queue = VK_NULL_HANDLE;
static ObjCorner       *s_map = nullptr;
static bool             s_push2 = false;
static bool             s_drawn = false;
static bool             s_logged = false;
static uint32_t         s_count = 0;
static uint32_t         s_gen = 0;
static uint32_t         s_shown_gen = 0;
static float            s_radius = 0.0f;

static ObjCorner s_cpu[k_obj_tri_cap * 3u];

static void mesh_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Mesh Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
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
        mesh_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

static int create_buffer(VulkanContext *vk) {
    VkDeviceSize bytes = (VkDeviceSize)(k_obj_tri_cap * 3u) * sizeof(ObjCorner);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = bytes,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &s_buffer);
    if (res != VK_SUCCESS) {
        s_buffer = VK_NULL_HANDLE;
        mesh_fail("vkCreateBuffer", res);
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
        fprintf(stderr, "[Mesh Error] No host-visible memory for the mesh\n");
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
        s_memory = VK_NULL_HANDLE;
        mesh_fail("vkAllocateMemory", res);
        return -1;
    }
    res = vkBindBufferMemory(vk->device, s_buffer, s_memory, 0);
    if (res != VK_SUCCESS) {
        mesh_fail("vkBindBufferMemory", res);
        return -1;
    }
    void *mapped = nullptr;
    res = vkMapMemory(vk->device, s_memory, 0, reqs.size, 0, &mapped);
    if (res != VK_SUCCESS || mapped == nullptr) {
        mesh_fail("vkMapMemory", res);
        return -1;
    }
    s_map = mapped;
    memset(s_map, 0, (size_t)bytes);
    VkBufferDeviceAddressInfo address_info = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = s_buffer,
    };
    s_addr = vkGetBufferDeviceAddress(vk->device, &address_info);
    if (s_addr == 0) {
        fprintf(stderr, "[Mesh Error] Mesh device address is null\n");
        return -1;
    }
    return 0;
}

static void fit_corners(ObjCorner *dst, uint32_t corners) {
    s_radius = 0.0f;
    if (corners == 0u || dst == nullptr) {
        return;
    }
    float minx = dst[0].x;
    float maxx = minx;
    float miny = dst[0].y;
    float maxy = miny;
    float minz = dst[0].z;
    float maxz = minz;
    for (uint32_t i = 1; i < corners; ++i) {
        float x = dst[i].x;
        float y = dst[i].y;
        float z = dst[i].z;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        if (z < minz) minz = z;
        if (z > maxz) maxz = z;
    }
    float dx = maxx - minx;
    float dy = maxy - miny;
    float dz = maxz - minz;
    float s = 1.0e30f;
    if (dx > 1.0e-8f) {
        s = (k_fit_hx * 2.0f) / dx;
    }
    if (dy > 1.0e-8f) {
        float sy = (k_fit_hy * 2.0f) / dy;
        if (sy < s) {
            s = sy;
        }
    }
    if (dz > 1.0e-8f) {
        float sz = (k_fit_hz * 2.0f) / dz;
        if (sz < s) {
            s = sz;
        }
    }
    if (s > 1.0e29f) {
        s = 1.0f;
    }
    float cx = (minx + maxx) * 0.5f;
    float cy = (miny + maxy) * 0.5f;
    float cz = (minz + maxz) * 0.5f;
    for (uint32_t i = 0; i < corners; ++i) {
        dst[i].x = k_fit_x + (dst[i].x - cx) * s;
        dst[i].y = k_fit_y + (dst[i].y - cy) * s;
        dst[i].z = (dst[i].z - cz) * s;
    }
    float hx = dx * 0.5f * s;
    float hy = dy * 0.5f * s;
    float hz = dz * 0.5f * s;
    s_radius = sqrtf(hx * hx + hy * hy + hz * hz);
}

static bool store_corners(const ObjCorner *src, uint32_t corners) {
    if (s_map == nullptr) {
        return false;
    }
    if (corners > k_obj_tri_cap * 3u) {
        corners = k_obj_tri_cap * 3u;
    }
    bool same = corners == s_count &&
                (corners == 0u || memcmp(s_map, src, (size_t)corners * sizeof(ObjCorner)) == 0);
    if (same) {
        return false;
    }
    if (s_drawn && s_queue != VK_NULL_HANDLE) {
        VkResult waited = vkQueueWaitIdle(s_queue);
        if (waited != VK_SUCCESS) {
            mesh_fail("vkQueueWaitIdle reload", waited);
            return false;
        }
    }
    if (corners > 0u && src != nullptr) {
        memcpy(s_map, src, (size_t)corners * sizeof(ObjCorner));
    }
    if (corners < s_count) {
        memset(s_map + corners, 0, (size_t)(s_count - corners) * sizeof(ObjCorner));
    }
    s_count = corners;
    s_gen++;
    return true;
}

static void log_load(bool found, int corners, bool capped, bool crossed) {
    if (!found) {
        printf("[Mesh] plots/mesh.obj was not found.\n");
        fflush(stdout);
        return;
    }
    if (crossed) {
        printf("[Mesh] A face crosses itself. That face was left out.\n");
        fflush(stdout);
    }
    if (capped) {
        printf("[Mesh] Triangle cap is %u. The rest was left out.\n", k_obj_tri_cap);
        fflush(stdout);
    }
    if (corners < 3) {
        printf("[Mesh] plots/mesh.obj has no faces.\n");
        fflush(stdout);
        return;
    }
    printf("[Mesh] %u triangles from plots/mesh.obj.\n", (uint32_t)corners / 3u);
    fflush(stdout);
}

static bool load_file(RingCqeFn on_cqe, void *on_user, bool *has) {
    bool found = false;
    bool capped = false;
    bool crossed = false;
    int corners = plot_file_load_mesh(s_cpu, k_obj_tri_cap * 3u, &found, &capped, &crossed,
                                      on_cqe, on_user);
    if (corners < 0) {
        if (has != nullptr) {
            *has = s_count >= 3u;
        }
        return false;
    }
    log_load(found, corners, capped, crossed);
    float saved_radius = s_radius;
    uint32_t n = corners > 0 ? (uint32_t)corners : 0u;
    if (n > 0u) {
        fit_corners(s_cpu, n);
    } else {
        s_radius = 0.0f;
    }
    float next_radius = s_radius;
    s_radius = saved_radius;
    bool changed = store_corners(n > 0u ? s_cpu : nullptr, n);
    if (changed || n == s_count) {
        s_radius = next_radius;
    }
    if (has != nullptr) {
        *has = s_count >= 3u;
    }
    return changed;
}

void mesh_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_queue != VK_NULL_HANDLE) {
        (void)vkQueueWaitIdle(s_queue);
    }
    if (vk != nullptr && vk->device != VK_NULL_HANDLE &&
        s_memory != VK_NULL_HANDLE && s_map != nullptr) {
        vkUnmapMemory(vk->device, s_memory);
    }
    s_map = nullptr;
    s_addr = 0;
    s_queue = VK_NULL_HANDLE;
    s_count = 0;
    s_radius = 0.0f;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_pipeline = VK_NULL_HANDLE;
        s_layout = VK_NULL_HANDLE;
        s_buffer = VK_NULL_HANDLE;
        s_memory = VK_NULL_HANDLE;
        return;
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
    s_pipeline = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    s_buffer = VK_NULL_HANDLE;
    s_memory = VK_NULL_HANDLE;
}

int mesh_init(VulkanContext *vk, RingCqeFn on_cqe, void *on_user) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    if (!plot_file_mesh_check()) {
        fprintf(stderr, "[Mesh Error] Triangulation self-check failed\n");
    }
    VkFormat depth_format = cube_depth_format();
    VkSampleCountFlagBits samples = cube_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Mesh Error] The cube pass has no depth format yet\n");
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(MeshPush)) {
        fprintf(stderr, "[Mesh Error] Push constants are %u bytes, the mesh needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(MeshPush));
        return -1;
    }
    s_push2 = vk->device_info.supports_maintenance6;
    s_queue = vk->queues.graphics_queue;
    if (create_buffer(vk) != 0) {
        mesh_shutdown(vk);
        return -1;
    }
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(MeshPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        mesh_fail("vkCreatePipelineLayout", res);
        mesh_shutdown(vk);
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
        mesh_shutdown(vk);
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
        s_pipeline = VK_NULL_HANDLE;
        mesh_fail("vkCreateGraphicsPipelines", res);
        mesh_shutdown(vk);
        return -1;
    }
    (void)load_file(on_cqe, on_user, nullptr);
    s_gen = 0;
    s_shown_gen = 0;
    if (!s_logged) {
        printf("[Mesh] OBJ faces are a mesh. L reloads plots/mesh.obj.\n");
        fflush(stdout);
        s_logged = true;
    }
    return 0;
}

bool mesh_reload(RingCqeFn on_cqe, void *on_user, bool *has) {
    if (s_map == nullptr) {
        if (has != nullptr) {
            *has = false;
        }
        return false;
    }
    return load_file(on_cqe, on_user, has);
}

bool mesh_has_triangles(void) {
    return s_count >= 3u;
}

uint32_t mesh_vertex_count(void) {
    return s_count;
}

bool mesh_sphere(float *cx, float *cy, float *radius) {
    if (s_count < 3u || s_radius <= 0.0f) {
        return false;
    }
    if (cx != nullptr) {
        *cx = k_fit_x;
    }
    if (cy != nullptr) {
        *cy = k_fit_y;
    }
    if (radius != nullptr) {
        *radius = s_radius;
    }
    return true;
}

bool mesh_wants_frame(void) {
    return s_gen != s_shown_gen;
}

void mesh_commit(void) {
    s_shown_gen = s_gen;
}

void mesh_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
               float yaw_deg, bool selected) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || s_count < 3u ||
        width == 0u || height == 0u) {
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
    MeshPush draw_push = {
        .verts      = s_addr,
        .width      = width,
        .height     = height,
        .yaw_deg    = yaw_deg,
        .origin_x   = k_fit_x,
        .origin_y   = k_fit_y,
        .depth_bias = 0.0f,
        .selected   = selected ? 1.0f : 0.0f,
        .pad        = 0,
    };
    view_push_fill(&draw_push.view, k_mat_mesh);
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
    vkCmdDraw(cmd, s_count, 1, 0, 0);
    s_drawn = true;
}
