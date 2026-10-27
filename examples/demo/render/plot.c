#include "frame/plot.h"
#include "frame/plot_file.h"
#include "frame/camera.h"
#include "frame/pass.h"
#include "vulkan/memory.h"
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "plot.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "plot.frag.spv"
};
alignas(uint32_t) static const unsigned char k_bake_spv[] = {
#embed "plot.comp.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "plot vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "plot frag spirv");
static_assert(sizeof k_bake_spv % 4u == 0u, "plot bake spirv");

// Keep the sample count in lockstep with shaders/plot.comp. Six axis ends
// sit in front of the samples. The file region starts after that.
enum : uint32_t {
    k_math_samples = 1024u,
    k_math_axes    = 6u,
    k_math_points  = k_math_axes + k_math_samples,
    k_file_points  = 8192u,
    k_file_samples = k_file_points - k_math_axes,
    k_plot_points  = k_math_points + k_file_points,
};

enum : uint32_t {
    k_mode_off   = 0u,
    k_mode_sin   = 1u,
    k_mode_sinc  = 2u,
    k_mode_gauss = 3u,
};

typedef struct PlotPush {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t base;
    uint32_t count;
    uint32_t kind;
    float    r;
    float    g;
    float    b;
    float    qx;
    float    qy;
    float    qz;
    float    qw;
    float    cam_x;
    float    cam_y;
    float    cam_z;
    float    zoom;
} PlotPush;

typedef struct PlotBake {
    uint64_t addr;
    uint32_t mode;
    uint32_t count;
} PlotBake;

static_assert(offsetof(PlotPush, width) == 8u, "plot width");
static_assert(offsetof(PlotPush, kind) == 24u, "plot kind");
static_assert(offsetof(PlotPush, r) == 28u, "plot color");
static_assert(offsetof(PlotPush, qx) == 40u, "plot quat");
static_assert(offsetof(PlotPush, zoom) == 68u, "plot zoom");
static_assert(sizeof(PlotPush) == 72u, "plot push");
static_assert(offsetof(PlotBake, mode) == 8u, "plot bake mode");
static_assert(sizeof(PlotBake) == 16u, "plot bake");

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkPipelineLayout s_bake_layout = VK_NULL_HANDLE;
static VkPipeline       s_bake = VK_NULL_HANDLE;
static VkBuffer         s_buffer = VK_NULL_HANDLE;
static VkDeviceMemory   s_memory = VK_NULL_HANDLE;
static VkDeviceAddress  s_addr = 0;
static VkQueue          s_queue = VK_NULL_HANDLE;
static PlotSample      *s_map = nullptr;
// Off until M. The pipeline stays. A cycle is sin, sinc, gaussian, off.
static uint32_t         s_mode = k_mode_off;
static bool             s_math_dirty = false;
static uint32_t         s_file_n = 0;
static uint32_t         s_gen = 0;
static uint32_t         s_shown_gen = 0;
static bool             s_logged = false;
static bool             s_drawn = false;

static PlotSample      s_built[k_file_samples];

static const float k_file_cx = 300.0f;
static const float k_file_cy = -15.0f;
static const float k_file_hx = 100.0f;
static const float k_file_hy = 70.0f;
static const float k_file_hz = 48.0f;

static void plot_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Plot Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static const char *mode_name(uint32_t mode) {
    if (mode == k_mode_sin) {
        return "sin";
    }
    if (mode == k_mode_sinc) {
        return "sinc";
    }
    if (mode == k_mode_gauss) {
        return "gaussian";
    }
    return "off";
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
        plot_fail("vkCreateShaderModule", res);
        return VK_NULL_HANDLE;
    }
    return module;
}

static void push_bytes(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stage,
                       const void *bytes, uint32_t size) {
    vulkan_cmd_push(cmd, layout, stage, size, bytes);
}

void plot_shutdown(VulkanContext *vk) {
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
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_bake = VK_NULL_HANDLE;
        s_bake_layout = VK_NULL_HANDLE;
        s_pipeline = VK_NULL_HANDLE;
        s_layout = VK_NULL_HANDLE;
        s_buffer = VK_NULL_HANDLE;
        s_memory = VK_NULL_HANDLE;
        return;
    }
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
    s_bake = VK_NULL_HANDLE;
    s_bake_layout = VK_NULL_HANDLE;
    s_pipeline = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    s_buffer = VK_NULL_HANDLE;
    s_memory = VK_NULL_HANDLE;
}

static int create_buffer(VulkanContext *vk) {
    VkDeviceSize bytes = (VkDeviceSize)k_plot_points * sizeof(PlotSample);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size  = bytes,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VkResult res = vkCreateBuffer(vk->device, &buffer_info, nullptr, &s_buffer);
    if (res != VK_SUCCESS) {
        s_buffer = VK_NULL_HANDLE;
        plot_fail("vkCreateBuffer", res);
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
        fprintf(stderr, "[Plot Error] No host-visible memory for the plot\n");
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
        plot_fail("vkAllocateMemory", res);
        return -1;
    }
    res = vkBindBufferMemory(vk->device, s_buffer, s_memory, 0);
    if (res != VK_SUCCESS) {
        plot_fail("vkBindBufferMemory", res);
        return -1;
    }
    void *mapped = nullptr;
    res = vkMapMemory(vk->device, s_memory, 0, reqs.size, 0, &mapped);
    if (res != VK_SUCCESS || mapped == nullptr) {
        plot_fail("vkMapMemory", res);
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
        fprintf(stderr, "[Plot Error] Plot device address is null\n");
        return -1;
    }
    return 0;
}

static int bake_math(VulkanContext *vk) {
    if (s_mode == k_mode_off || s_bake == VK_NULL_HANDLE || s_queue == VK_NULL_HANDLE) {
        return 0;
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
        plot_fail("vkAllocateCommandBuffers bake", res);
        return -1;
    }
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    res = vkBeginCommandBuffer(cmd, &begin_info);
    if (res != VK_SUCCESS) {
        plot_fail("vkBeginCommandBuffer bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bake);
    PlotBake bake = {
        .addr  = s_addr,
        .mode  = s_mode,
        .count = k_math_samples,
    };
    push_bytes(cmd, s_bake_layout, VK_SHADER_STAGE_COMPUTE_BIT, &bake, sizeof bake);
    vkCmdDispatch(cmd, (k_math_points + 63u) / 64u, 1u, 1u);
    VkMemoryBarrier2 done = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &done,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        plot_fail("vkEndCommandBuffer bake", res);
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
    res = vkQueueSubmit2(s_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        plot_fail("vkQueueSubmit2 bake", res);
        vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
        return -1;
    }
    res = vkQueueWaitIdle(s_queue);
    vkFreeCommandBuffers(vk->device, vk->cmd_pool, 1, &cmd);
    if (res != VK_SUCCESS) {
        plot_fail("vkQueueWaitIdle bake", res);
        return -1;
    }
    s_math_dirty = false;
    return 0;
}

void plot_prepare(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || !s_math_dirty || s_bake == VK_NULL_HANDLE) {
        if (s_mode == k_mode_off) {
            s_math_dirty = false;
        }
        return;
    }
    if (s_drawn && s_queue != VK_NULL_HANDLE && vkQueueWaitIdle(s_queue) != VK_SUCCESS) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bake);
    PlotBake bake = {
        .addr  = s_addr,
        .mode  = s_mode,
        .count = k_math_samples,
    };
    push_bytes(cmd, s_bake_layout, VK_SHADER_STAGE_COMPUTE_BIT, &bake, sizeof bake);
    vkCmdDispatch(cmd, (k_math_points + 63u) / 64u, 1u, 1u);
    VkMemoryBarrier2 done = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &done,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    s_math_dirty = false;
}

static void draw_range(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                       uint32_t base, uint32_t count, uint32_t kind,
                       float r, float g, float b) {
    if (s_pipeline == VK_NULL_HANDLE || s_addr == 0 || count == 0) {
        return;
    }
    if (kind == 0u && count < 2u) {
        return;
    }
    uint32_t verts = kind == 0u ? (count - 1u) * 6u : count * 6u;
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
    PlotPush push = {
        .addr   = s_addr,
        .width  = width,
        .height = height,
        .base   = base,
        .count  = count,
        .kind   = kind,
        .r = r, .g = g, .b = b,
        .qx = view.qx, .qy = view.qy, .qz = view.qz, .qw = view.qw,
        .cam_x = view.cam_x, .cam_y = view.cam_y, .cam_z = view.cam_z,
        .zoom = view.zoom,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    push_bytes(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, &push, sizeof push);
    vkCmdDraw(cmd, verts, 1, 0, 0);
}

void plot_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (cmd == VK_NULL_HANDLE || width == 0 || height == 0 || s_pipeline == VK_NULL_HANDLE) {
        return;
    }
    if (s_mode != k_mode_off) {
        draw_range(cmd, width, height, 0u, k_math_axes, 0u, 0.82f, 0.84f, 0.88f);
        draw_range(cmd, width, height, k_math_axes, k_math_samples, 0u, 0.95f, 0.72f, 0.28f);
    }
    if (s_file_n > k_math_axes) {
        uint32_t data = s_file_n - k_math_axes;
        draw_range(cmd, width, height, k_math_points, k_math_axes, 0u, 0.82f, 0.84f, 0.88f);
        draw_range(cmd, width, height, k_math_points + k_math_axes, data, 0u, 0.40f, 0.86f, 0.95f);
        draw_range(cmd, width, height, k_math_points + k_math_axes, data, 1u, 0.93f, 0.95f, 0.90f);
    }
    s_drawn = true;
}

bool plot_cycle(void) {
    if (s_mode == k_mode_sin) {
        s_mode = k_mode_sinc;
    } else if (s_mode == k_mode_sinc) {
        s_mode = k_mode_gauss;
    } else if (s_mode == k_mode_gauss) {
        s_mode = k_mode_off;
    } else {
        s_mode = k_mode_sin;
    }
    s_math_dirty = s_mode != k_mode_off;
    s_gen++;
    printf("[Plot] Math %s.\n", mode_name(s_mode));
    fflush(stdout);
    return true;
}

bool plot_wants_frame(void) {
    return s_gen != s_shown_gen || s_math_dirty;
}

void plot_commit(void) {
    s_shown_gen = s_gen;
}

static void fit_built(uint32_t n) {
    if (n == 0u) {
        return;
    }
    float minx = s_built[0].x;
    float maxx = minx;
    float miny = s_built[0].y;
    float maxy = miny;
    float minz = s_built[0].z;
    float maxz = minz;
    for (uint32_t i = 1; i < n; ++i) {
        float x = s_built[i].x;
        float y = s_built[i].y;
        float z = s_built[i].z;
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
    if (dx < 1.0e-4f) dx = 1.0e-4f;
    if (dy < 1.0e-4f) dy = 1.0e-4f;
    float sx = (k_file_hx * 2.0f) / dx;
    float sy = (k_file_hy * 2.0f) / dy;
    float s = sx < sy ? sx : sy;
    if (dz > 1.0e-3f) {
        float sz = (k_file_hz * 2.0f) / dz;
        if (sz < s) {
            s = sz;
        }
    }
    float cx = (minx + maxx) * 0.5f;
    float cy = (miny + maxy) * 0.5f;
    float cz = (minz + maxz) * 0.5f;
    for (uint32_t i = 0; i < n; ++i) {
        s_built[i].x = k_file_cx + (s_built[i].x - cx) * s;
        s_built[i].y = k_file_cy + (s_built[i].y - cy) * s;
        s_built[i].z = (s_built[i].z - cz) * s;
    }
}

static void write_axes(PlotSample *dst) {
    dst[0] = (PlotSample){ k_file_cx - k_file_hx, k_file_cy, 0.0f, 1.0f };
    dst[1] = (PlotSample){ k_file_cx + k_file_hx, k_file_cy, 0.0f, 0.0f };
    dst[2] = (PlotSample){ k_file_cx, k_file_cy - k_file_hy, 0.0f, 1.0f };
    dst[3] = (PlotSample){ k_file_cx, k_file_cy + k_file_hy, 0.0f, 0.0f };
    dst[4] = (PlotSample){ k_file_cx, k_file_cy, -k_file_hz, 1.0f };
    dst[5] = (PlotSample){ k_file_cx, k_file_cy, k_file_hz, 0.0f };
}

static bool store_file(uint32_t n) {
    if (s_map == nullptr) {
        return false;
    }
    if (n > k_file_samples) {
        n = k_file_samples;
    }
    PlotSample *dst = s_map + k_math_points;
    bool changed = false;
    if (n == 0u) {
        changed = s_file_n != 0u;
    } else {
        fit_built(n);
        changed = s_file_n != k_math_axes + n ||
                  memcmp(dst + k_math_axes, s_built, (size_t)n * sizeof(PlotSample)) != 0;
    }
    if (!changed) {
        return false;
    }
    if (s_drawn && s_queue != VK_NULL_HANDLE) {
        VkResult waited = vkQueueWaitIdle(s_queue);
        if (waited != VK_SUCCESS) {
            plot_fail("vkQueueWaitIdle reload", waited);
            return false;
        }
    }
    if (n == 0u) {
        memset(dst, 0, (size_t)s_file_n * sizeof(PlotSample));
        s_file_n = 0;
        return true;
    }
    uint32_t total = k_math_axes + n;
    write_axes(dst);
    memcpy(dst + k_math_axes, s_built, (size_t)n * sizeof(PlotSample));
    if (total < s_file_n) {
        memset(dst + total, 0, (size_t)(s_file_n - total) * sizeof(PlotSample));
    }
    s_file_n = total;
    return true;
}

bool plot_reload(RingCqeFn on_cqe, void *on_user) {
    bool found = false;
    uint32_t n = plot_file_load(s_built, k_file_samples, &found, nullptr, on_cqe, on_user);
    if (s_map == nullptr) {
        return false;
    }
    bool changed = store_file(found ? n : 0u);
    if (changed) {
        s_gen++;
    }
    return changed;
}

int plot_init(VulkanContext *vk, RingCqeFn on_cqe, void *on_user) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_pipeline != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Plot Error] The render pass has no depth format yet\n");
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(PlotPush)) {
        fprintf(stderr, "[Plot Error] Push constants are %u bytes, the plot needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(PlotPush));
        return -1;
    }
    s_queue = vk->queues.graphics_queue;
    if (create_buffer(vk) != 0) {
        plot_shutdown(vk);
        return -1;
    }

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size       = sizeof(PlotPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        plot_fail("vkCreatePipelineLayout", res);
        plot_shutdown(vk);
        return -1;
    }
    VkPushConstantRange bake_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size       = sizeof(PlotBake),
    };
    VkPipelineLayoutCreateInfo bake_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &bake_range,
    };
    res = vkCreatePipelineLayout(vk->device, &bake_layout_info, nullptr, &s_bake_layout);
    if (res != VK_SUCCESS) {
        plot_fail("vkCreatePipelineLayout bake", res);
        plot_shutdown(vk);
        return -1;
    }

    VkShaderModule vert = make_module(vk, (const uint32_t *)k_vert_spv, (uint32_t)sizeof k_vert_spv);
    VkShaderModule frag = make_module(vk, (const uint32_t *)k_frag_spv, (uint32_t)sizeof k_frag_spv);
    VkShaderModule comp = make_module(vk, (const uint32_t *)k_bake_spv, (uint32_t)sizeof k_bake_spv);
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE || comp == VK_NULL_HANDLE) {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, vert, nullptr);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, frag, nullptr);
        if (comp != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, comp, nullptr);
        plot_shutdown(vk);
        return -1;
    }
    VkPipelineShaderStageCreateInfo comp_stage = {
        .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = comp,
        .pName  = "main",
    };
    VkComputePipelineCreateInfo bake_pipe = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = comp_stage,
        .layout = s_bake_layout,
    };
    res = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &bake_pipe, nullptr, &s_bake);
    vkDestroyShaderModule(vk->device, comp, nullptr);
    if (res != VK_SUCCESS) {
        plot_fail("vkCreateComputePipelines", res);
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, vert, nullptr);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(vk->device, frag, nullptr);
        plot_shutdown(vk);
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
        plot_fail("vkCreateGraphicsPipelines", res);
        plot_shutdown(vk);
        return -1;
    }
    if (vk->cmd_pool == VK_NULL_HANDLE || s_queue == VK_NULL_HANDLE) {
        fprintf(stderr, "[Plot Error] Math bake needs the graphics queue\n");
        plot_shutdown(vk);
        return -1;
    }
    if (bake_math(vk) != 0) {
        plot_shutdown(vk);
        return -1;
    }
    (void)plot_reload(on_cqe, on_user);
    s_gen = 0;
    s_shown_gen = 0;
    if (!s_logged) {
        printf("[Plot] Math curve on the GPU, off until M. M cycles sin, sinc, gaussian, off. L reloads plots/data.csv and plots/mesh.obj.\n");
        fflush(stdout);
        s_logged = true;
    }
    return 0;
}
