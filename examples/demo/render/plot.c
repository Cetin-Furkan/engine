#include "frame/plot.h"
#include "frame/plot_file.h"
#include "frame/camera.h"
#include "frame/pass.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
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

// Twelve slices cover both frame slots and the orphan ring. The draw slice
// stays until a newer one is published, so a failed submit still has the
// previous samples. Math is copied only from a slice whose timeline point
// has retired. The GPU write is the one compute dispatch in plot_prepare.
enum : uint32_t { k_slice_count = 12u };

static_assert((k_plot_points * sizeof(PlotSample)) % 16u == 0u, "plot slice");

typedef struct PlotSlice {
    uint64_t point;
    uint32_t mode;
    bool     math_ready;
} PlotSlice;

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static VkPipelineLayout s_bake_layout = VK_NULL_HANDLE;
static VkPipeline       s_bake = VK_NULL_HANDLE;
static DaBuffer         s_geom;
static VkDevice         s_device = VK_NULL_HANDLE;
static VkDeviceAddress  s_base = 0;
static VkQueue          s_queue = VK_NULL_HANDLE;
static PlotSample      *s_map = nullptr;
// Off until M. The pipeline stays. A cycle is sin, sinc, gaussian, off.
static uint32_t         s_mode = k_mode_off;
static bool             s_math_dirty = false;
static bool             s_file_dirty = false;
static uint32_t         s_file_n = 0;
static uint32_t         s_gen = 0;
static uint32_t         s_shown_gen = 0;
static bool             s_logged = false;
static bool             s_busy_warned = false;
static PlotSlice        s_slice[k_slice_count];
static int              s_draw_slice = -1;
static VkDeviceAddress  s_draw_addr = 0;
static uint32_t         s_draw_file_n = 0;
static int              s_armed = -1;
static uint64_t         s_armed_point = 0;
static bool             s_armed_published = false;
static bool             s_saved_math = false;
static bool             s_saved_file = false;
static int              s_prev_draw = -1;
static VkDeviceAddress  s_prev_addr = 0;
static uint32_t         s_prev_file_n = 0;

static PlotSample      s_built[k_file_samples];
static PlotSample      s_file_cpu[k_file_points];
static PlotSample      s_math_stage[k_math_points];

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

static VkDeviceAddress slice_addr(uint32_t index) {
    return s_base + (VkDeviceAddress)index * (VkDeviceAddress)k_plot_points * sizeof(PlotSample);
}

static PlotSample *slice_cpu(uint32_t index) {
    return s_map + (size_t)index * k_plot_points;
}

static void push_bytes(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stage,
                       const void *bytes, uint32_t size) {
    vulkan_cmd_push(cmd, layout, stage, size, bytes);
}

void plot_shutdown(VulkanContext *vk) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_queue != VK_NULL_HANDLE) {
        (void)vkQueueWaitIdle(s_queue);
    }
    s_map = nullptr;
    s_base = 0;
    s_device = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    s_draw_slice = -1;
    s_draw_addr = 0;
    s_draw_file_n = 0;
    s_armed = -1;
    s_armed_point = 0;
    s_armed_published = false;
    s_math_dirty = false;
    s_file_dirty = false;
    memset(s_slice, 0, sizeof s_slice);
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
    }
    s_bake = VK_NULL_HANDLE;
    s_bake_layout = VK_NULL_HANDLE;
    s_pipeline = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    da_buffer_destroy(vk, &s_geom);
}

static int create_buffer(VulkanContext *vk) {
    VkDeviceSize bytes = (VkDeviceSize)k_slice_count * k_plot_points * sizeof(PlotSample);
    if (da_buffer_create(vk, bytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, &s_geom) != 0) {
        return -1;
    }
    if (s_geom.map == nullptr || s_geom.address == 0 || (s_geom.address & 15u) != 0u) {
        fprintf(stderr, "[Plot Error] Plot device address is null\n");
        return -1;
    }
    s_map = s_geom.map;
    s_base = s_geom.address;
    s_device = vk->device;
    memset(s_map, 0, (size_t)bytes);
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
        .addr  = slice_addr(0),
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
    s_slice[0].point = 0;
    s_slice[0].mode = s_mode;
    s_slice[0].math_ready = true;
    s_draw_slice = 0;
    s_draw_addr = slice_addr(0);
    s_draw_file_n = s_file_n;
    s_math_dirty = false;
    return 0;
}

static int claim_slice(VkSemaphore acquire, uint64_t signal_point) {
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            return (int)i;
        }
    }
    uint64_t counter = 0;
    bool have = false;
    if (s_device != VK_NULL_HANDLE && acquire != VK_NULL_HANDLE &&
        vkGetSemaphoreCounterValue(s_device, acquire, &counter) == VK_SUCCESS) {
        have = true;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if ((int)i == s_draw_slice) {
            continue;
        }
        if (s_slice[i].point == 0u || (have && counter >= s_slice[i].point)) {
            s_slice[i].point = signal_point;
            s_slice[i].math_ready = false;
            s_slice[i].mode = 0;
            return (int)i;
        }
    }
    return -1;
}

static int retired_math(VkSemaphore acquire) {
    uint64_t counter = 0;
    bool have = false;
    if (s_device != VK_NULL_HANDLE && acquire != VK_NULL_HANDLE &&
        vkGetSemaphoreCounterValue(s_device, acquire, &counter) == VK_SUCCESS) {
        have = true;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (!s_slice[i].math_ready || s_slice[i].mode != s_mode) {
            continue;
        }
        if (s_slice[i].point == 0u || (have && counter >= s_slice[i].point)) {
            return (int)i;
        }
    }
    return -1;
}

static void copy_file_into(PlotSample *dst) {
    PlotSample *file = dst + k_math_points;
    if (s_file_n > 0u) {
        memcpy(file, s_file_cpu, (size_t)s_file_n * sizeof(PlotSample));
    }
    if (s_file_n < k_file_points) {
        memset(file + s_file_n, 0, (size_t)(k_file_points - s_file_n) * sizeof(PlotSample));
    }
}

static void publish_draw(int slot) {
    s_prev_draw = s_draw_slice;
    s_prev_addr = s_draw_addr;
    s_prev_file_n = s_draw_file_n;
    s_draw_slice = slot;
    s_draw_addr = slice_addr((uint32_t)slot);
    s_draw_file_n = s_file_n;
    s_saved_math = s_math_dirty;
    s_saved_file = s_file_dirty;
    s_math_dirty = false;
    s_file_dirty = false;
    s_armed_published = true;
}

void plot_begin(VkSemaphore acquire, uint64_t signal_point) {
    s_armed = -1;
    s_armed_point = 0;
    s_armed_published = false;
    s_saved_math = false;
    s_saved_file = false;
    if (s_map == nullptr || signal_point == 0u || (!s_math_dirty && !s_file_dirty)) {
        return;
    }
    bool need_math = !s_math_dirty && s_mode != k_mode_off;
    if (need_math) {
        int src = retired_math(acquire);
        if (src < 0) {
            return;
        }
        memcpy(s_math_stage, slice_cpu((uint32_t)src), sizeof s_math_stage);
    }
    int slot = claim_slice(acquire, signal_point);
    if (slot < 0) {
        if (!s_busy_warned) {
            fprintf(stderr, "[Plot Error] Plot samples are still on the GPU\n");
            s_busy_warned = true;
        }
        return;
    }
    PlotSample *dst = slice_cpu((uint32_t)slot);
    if (need_math) {
        memcpy(dst, s_math_stage, sizeof s_math_stage);
        s_slice[slot].math_ready = true;
        s_slice[slot].mode = s_mode;
    }
    copy_file_into(dst);
    s_armed = slot;
    s_armed_point = signal_point;
    if (!s_math_dirty) {
        publish_draw(slot);
    }
}

void plot_release(uint64_t signal_point) {
    if (signal_point == 0u) {
        return;
    }
    if (s_armed_published && s_armed_point == signal_point) {
        s_math_dirty = s_saved_math;
        s_file_dirty = s_saved_file;
        s_draw_slice = s_prev_draw;
        s_draw_addr = s_prev_addr;
        s_draw_file_n = s_prev_file_n;
        s_armed_published = false;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            s_slice[i].point = 0;
            s_slice[i].math_ready = false;
            s_slice[i].mode = 0;
        }
    }
    if (s_armed >= 0 && s_armed_point == signal_point) {
        s_armed = -1;
        s_armed_point = 0;
    }
}

void plot_prepare(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || !s_math_dirty || s_bake == VK_NULL_HANDLE) {
        if (s_mode == k_mode_off) {
            s_math_dirty = false;
        }
        return;
    }
    if (s_armed < 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_bake);
    PlotBake bake = {
        .addr  = slice_addr((uint32_t)s_armed),
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
    s_slice[s_armed].math_ready = true;
    s_slice[s_armed].mode = s_mode;
    publish_draw(s_armed);
}

static void draw_range(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                       uint32_t base, uint32_t count, uint32_t kind,
                       float r, float g, float b) {
    if (s_draw_addr == 0 || count == 0) {
        return;
    }
    if (kind == 0u && count < 2u) {
        return;
    }
    uint32_t verts = kind == 0u ? (count - 1u) * 6u : count * 6u;
    ViewPush view;
    view_push_fill(&view, 0);
    PlotPush push = {
        .addr   = s_draw_addr,
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
    push_bytes(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, &push, sizeof push);
    vkCmdDraw(cmd, verts, 1, 0, 0);
}

void plot_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (cmd == VK_NULL_HANDLE || width == 0 || height == 0 || s_pipeline == VK_NULL_HANDLE ||
        s_draw_addr == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    bool math_ok = s_mode != k_mode_off && s_draw_slice >= 0 &&
                   s_slice[s_draw_slice].math_ready && s_slice[s_draw_slice].mode == s_mode;
    if (math_ok) {
        draw_range(cmd, width, height, 0u, k_math_axes, 0u, 0.82f, 0.84f, 0.88f);
        draw_range(cmd, width, height, k_math_axes, k_math_samples, 0u, 0.95f, 0.72f, 0.28f);
    }
    if (s_draw_file_n > k_math_axes) {
        uint32_t data = s_draw_file_n - k_math_axes;
        draw_range(cmd, width, height, k_math_points, k_math_axes, 0u, 0.82f, 0.84f, 0.88f);
        draw_range(cmd, width, height, k_math_points + k_math_axes, data, 0u, 0.40f, 0.86f, 0.95f);
        draw_range(cmd, width, height, k_math_points + k_math_axes, data, 1u, 0.93f, 0.95f, 0.90f);
    }
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
    return s_gen != s_shown_gen || s_math_dirty || s_file_dirty;
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
    if (s_base == 0) {
        return false;
    }
    if (n > k_file_samples) {
        n = k_file_samples;
    }
    bool changed = false;
    if (n == 0u) {
        changed = s_file_n != 0u;
    } else {
        fit_built(n);
        changed = s_file_n != k_math_axes + n ||
                  memcmp(s_file_cpu + k_math_axes, s_built, (size_t)n * sizeof(PlotSample)) != 0;
    }
    if (!changed) {
        return false;
    }
    if (n == 0u) {
        memset(s_file_cpu, 0, (size_t)s_file_n * sizeof(PlotSample));
        s_file_n = 0;
        s_file_dirty = true;
        return true;
    }
    uint32_t total = k_math_axes + n;
    write_axes(s_file_cpu);
    memcpy(s_file_cpu + k_math_axes, s_built, (size_t)n * sizeof(PlotSample));
    if (total < s_file_n) {
        memset(s_file_cpu + total, 0, (size_t)(s_file_n - total) * sizeof(PlotSample));
    }
    s_file_n = total;
    s_file_dirty = true;
    return true;
}

bool plot_reload(RingCqeFn on_cqe, void *on_user) {
    bool found = false;
    uint32_t n = plot_file_load(s_built, k_file_samples, &found, nullptr, on_cqe, on_user);
    if (s_base == 0) {
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

    s_bake = vulkan_compute_pipeline(vk, k_bake_spv, sizeof k_bake_spv, s_bake_layout);
    if (s_bake == VK_NULL_HANDLE) {
        plot_shutdown(vk);
        return -1;
    }
    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
    VulkanGfxPipe gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth_format,
        .samples       = samples,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    if (vulkan_gfx_pipelines(vk, &gfx, &color_format, nullptr, 1, &s_pipeline) != 0) {
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
