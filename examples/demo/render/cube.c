#include "frame/cube.h"
#include "frame/camera.h"
#include "frame/grid.h"
#include "frame/pass.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
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
    uint32_t grid_n;
    uint32_t grid_pad;
    ViewPush view;
} DrawPush;

enum { k_draw_push_bytes = 112 };

// Three copies so a frame still reading one buffer is not overwritten by the
// next step. Two copies can be in flight. The third is the one being written.
// The spare wait retires the image from two presents back. It does not make
// this step's write visible to the next submit. That submit waits
// s_heat_published.
// Each copy holds k_cube_heat_slots slices. A fillet reads only its slice.
enum : uint32_t {
    k_heat_copies = 3u,
    k_heat_slots  = k_cube_heat_slots,
    k_heat_stride = ((k_verts * (uint32_t)sizeof(float)) + 15u) & ~15u,
};
static_assert(k_heat_stride % 16u == 0u, "heat slice align");
static_assert(k_heat_stride >= k_verts * (uint32_t)sizeof(float), "heat slice fits");

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
static_assert(offsetof(DrawPush, grid_n) == 56u, "grid n");
static_assert(offsetof(DrawPush, view) == 64u, "cube view");
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

typedef struct HeatLive {
    int      read;
    int      draw;
    bool     have_stamp;
    uint32_t stamp_ms;
    uint32_t step_dt;
    bool     started;
    bool     held;
    bool     dirty;
    bool     need_seed;
    float    poke_x;
    float    poke_y;
    bool     poke_pending;
    bool     poke_armed;
    bool     pulse;
    bool     pulse_have_t0;
    uint32_t pulse_t0;
} HeatLive;

static HeatLive s_live[k_heat_slots];
static bool     s_heat_announced = false;
static uint64_t s_heat_published = 0;
static void cube_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Cube Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
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
    DaBuffer buf = {0};
    if (da_buffer_create(vk, size, usage, false, &buf) != 0) {
        return -1;
    }
    *buffer = buf.buffer;
    *memory = buf.memory;
    *addr = buf.address;
    return 0;
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

static bool heat_slot_ok(uint32_t slot) {
    return slot < k_heat_slots;
}

static void heat_live_clear(void) {
    for (uint32_t i = 0; i < k_heat_slots; ++i) {
        s_live[i] = (HeatLive){0};
        s_live[i].draw = -1;
    }
    s_heat_announced = false;
    s_heat_published = 0;
}

static VkDeviceAddress heat_at(int copy, uint32_t slot) {
    if (copy < 0 || (uint32_t)copy >= k_heat_copies || slot >= k_heat_slots) {
        return 0;
    }
    return s_heat_addr[copy] + (VkDeviceAddress)slot * (VkDeviceAddress)k_heat_stride;
}

static void dispatch_heat(VkCommandBuffer cmd, uint32_t dt_ms, uint32_t seed,
                          int src_copy, int dst_copy, uint32_t slot, uint32_t poke, float yaw_deg) {
    const HeatLive *live = &s_live[slot];
    HeatPush heat_push = {
        .src     = heat_at(src_copy, slot),
        .dst     = heat_at(dst_copy, slot),
        .verts   = s_mesh_addr,
        .dt_ms   = dt_ms,
        .seed    = seed,
        .poke    = poke,
        .poke_x  = poke != 0u ? live->poke_x : 0.0f,
        .poke_y  = poke != 0u ? live->poke_y : 0.0f,
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
    for (uint32_t slot = 0; slot < k_heat_slots; ++slot) {
        dispatch_heat(cmd, 0, 1u, -1, 0, slot, 0u, 0.0f);
    }
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
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits sample_count = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || sample_count == 0) {
        fprintf(stderr, "[Cube Error] The render pass has no depth format yet\n");
        return -1;
    }

    if (create_da_buffer(vk, (VkDeviceSize)k_verts * 36u,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         &s_buffer, &s_memory, &s_mesh_addr) != 0 ||
        create_da_buffer(vk, (VkDeviceSize)k_indices * sizeof(uint16_t),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         &s_index, &s_index_memory, &s_index_addr) != 0) {
        cube_shutdown(vk);
        return -1;
    }
    heat_live_clear();
    for (uint32_t i = 0; i < k_heat_copies; ++i) {
        if (create_da_buffer(vk, (VkDeviceSize)k_heat_slots * (VkDeviceSize)k_heat_stride,
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

    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
    VulkanGfxPipe gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth_format,
        .samples       = sample_count,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    if (vulkan_gfx_pipelines(vk, &gfx, &color_format, nullptr, 1, &s_pipeline) != 0) {
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
    s_compute = vulkan_compute_pipeline(vk, k_bevel_spv, sizeof k_bevel_spv, s_compute_layout);
    if (s_compute == VK_NULL_HANDLE) {
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
    s_heat_pipe = vulkan_compute_pipeline(vk, k_heat_spv, sizeof k_heat_spv, s_heat_layout);
    if (s_heat_pipe == VK_NULL_HANDLE) {
        cube_shutdown(vk);
        return -1;
    }
    if (bake_mesh(vk) != 0) {
        cube_shutdown(vk);
        return -1;
    }
    printf("[Cube] 240px centered, %u verts, 12px GPU fillet, %u by %u face grid, %ux edge samples, directional sun\n",
           k_verts, k_grid, k_grid, sample_count_n(sample_count));
    fflush(stdout);
    return 0;
}

static void release_heat_copies(VulkanContext *vk, bool destroy) {
    for (uint32_t i = 0; i < k_heat_copies; ++i) {
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
    heat_live_clear();
}

void cube_shutdown(VulkanContext *vk) {
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

void cube_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, float yaw_deg,
               float origin_x, float origin_y, float depth_bias, float phase, bool selected,
               uint32_t mat, uint32_t heat_slot) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || width == 0 || height == 0 ||
        !heat_slot_ok(heat_slot)) {
        return;
    }
    const HeatLive *live = &s_live[heat_slot];
    int heat_copy = live->draw >= 0 ? live->draw : live->read;
    DrawPush draw_push = {
        .verts      = s_mesh_addr,
        .width      = width,
        .height     = height,
        .yaw_deg    = yaw_deg,
        .origin_x   = origin_x,
        .origin_y   = origin_y,
        .depth_bias = depth_bias,
        .heat       = heat_at(heat_copy, heat_slot),
        .phase      = phase,
        .selected   = selected ? 1.0f : 0.0f,
        .grid       = grid_field_addr(),
        .grid_n     = grid_n(),
    };
    view_push_fill(&draw_push.view, mat);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vkCmdBindIndexBuffer2(cmd, s_index, 0, (VkDeviceSize)k_indices * sizeof(uint16_t),
                          VK_INDEX_TYPE_UINT16);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, k_draw_push_bytes, &draw_push);
    vkCmdDrawIndexed(cmd, k_indices, 1, 0, 0, 0);
}

uint32_t cube_vertex_count(void) {
    return k_verts;
}

uint64_t cube_mesh_addr(void) {
    return s_mesh_addr;
}

static bool heat_still_running(void) {
    for (uint32_t i = 0; i < k_heat_slots; ++i) {
        if (s_live[i].started && !s_live[i].held) {
            return true;
        }
    }
    return false;
}

uint32_t cube_heat_interval(uint32_t slot, uint32_t time_ms) {
    if (!heat_slot_ok(slot)) {
        return 0;
    }
    HeatLive *h = &s_live[slot];
    if (h->held) {
        h->step_dt = 0;
        return 0;
    }
    if (!h->have_stamp) {
        h->have_stamp = true;
        h->stamp_ms = time_ms;
        h->step_dt = 0;
        return 0;
    }
    uint32_t delta = time_ms - h->stamp_ms;
    // A gap between revolutions is not a heat step.
    if (delta == 0u || delta > 1000u) {
        h->stamp_ms = time_ms;
        h->step_dt = 0;
        return 0;
    }
    h->step_dt = delta;
    return delta;
}

uint32_t cube_heat_step_dt(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return 0;
    }
    return s_live[slot].step_dt;
}

void cube_heat_commit_stamp(uint32_t slot, uint32_t time_ms) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    HeatLive *h = &s_live[slot];
    if (h->held || h->step_dt == 0u) {
        return;
    }
    h->have_stamp = true;
    h->stamp_ms = time_ms;
}

bool cube_heat_record(VkCommandBuffer cmd, uint32_t slot, float yaw_deg) {
    if (!heat_slot_ok(slot) || cmd == VK_NULL_HANDLE || s_heat_pipe == VK_NULL_HANDLE) {
        return false;
    }
    HeatLive *h = &s_live[slot];
    if (h->draw >= 0 || s_mesh_addr == 0) {
        return false;
    }
    int dst = (h->read + 1) % (int)k_heat_copies;
    if (heat_at(dst, slot) == 0) {
        return false;
    }
    if (h->need_seed) {
        dispatch_heat(cmd, 0, 1u, -1, dst, slot, 0u, yaw_deg);
        h->draw = dst;
        h->need_seed = false;
        return true;
    }
    bool poke = h->poke_pending;
    if ((h->step_dt == 0u && !poke) || h->held || heat_at(h->read, slot) == 0) {
        return false;
    }
    dispatch_heat(cmd, h->step_dt, 0u, h->read, dst, slot, poke ? 1u : 0u, yaw_deg);
    h->draw = dst;
    if (poke) {
        h->poke_pending = false;
        h->poke_armed = true;
    }
    return true;
}

void cube_heat_record_ok(uint32_t slot, uint64_t signal_point) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    HeatLive *h = &s_live[slot];
    if (h->draw < 0) {
        return;
    }
    h->read = h->draw;
    h->draw = -1;
    h->poke_armed = false;
    if (signal_point != 0) {
        s_heat_published = signal_point;
    }
    h->started = true;
    if (s_heat_announced) {
        return;
    }
    s_heat_announced = true;
    printf("[Cube] Fillet heat started. Each cube keeps its own field. Upper half, callback step, then it holds.\n");
    fflush(stdout);
}

void cube_heat_record_abort(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    HeatLive *h = &s_live[slot];
    h->draw = -1;
    if (h->poke_armed) {
        h->poke_pending = true;
        h->poke_armed = false;
    }
}

bool cube_heat_poke(uint32_t slot, float origin_x, float origin_y) {
    if (!heat_slot_ok(slot) || s_heat_pipe == VK_NULL_HANDLE) {
        return false;
    }
    HeatLive *h = &s_live[slot];
    h->poke_x = origin_x;
    h->poke_y = origin_y;
    h->poke_pending = true;
    h->held = false;
    h->pulse = true;
    h->pulse_have_t0 = false;
    printf("[Cube] Fillet heat poked at %+.0f,%+.0f.\n", (double)origin_x, (double)origin_y);
    fflush(stdout);
    return true;
}

bool cube_heat_poke_pending(void) {
    for (uint32_t i = 0; i < k_heat_slots; ++i) {
        if (s_live[i].poke_pending) {
            return true;
        }
    }
    return false;
}

bool cube_heat_slot_pending(uint32_t slot) {
    return heat_slot_ok(slot) && s_live[slot].poke_pending;
}

bool cube_heat_wants_frame(uint32_t slot, uint32_t time_ms, bool *ended) {
    if (ended != nullptr) {
        *ended = false;
    }
    if (!heat_slot_ok(slot)) {
        return false;
    }
    HeatLive *h = &s_live[slot];
    if (!h->pulse) {
        return false;
    }
    if (!h->pulse_have_t0) {
        h->pulse_t0 = time_ms;
        h->pulse_have_t0 = true;
        return true;
    }
    if (time_ms - h->pulse_t0 >= 5000u) {
        h->pulse = false;
        h->pulse_have_t0 = false;
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
    for (uint32_t i = 0; i < k_heat_slots; ++i) {
        if (s_live[i].draw >= 0) {
            return true;
        }
    }
    return false;
}

bool cube_heat_slot_open(uint32_t slot) {
    return heat_slot_ok(slot) && s_live[slot].draw >= 0;
}

void cube_heat_hold(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    HeatLive *h = &s_live[slot];
    if (h->held) {
        return;
    }
    bool was_started = h->started;
    h->held = true;
    h->pulse = false;
    h->pulse_have_t0 = false;
    h->poke_pending = false;
    h->poke_armed = false;
    h->step_dt = 0;
    if (!was_started || heat_still_running()) {
        return;
    }
    printf("[Cube] Fillet heat holds.\n");
    fflush(stdout);
}

bool cube_heat_held(uint32_t slot) {
    return !heat_slot_ok(slot) || s_live[slot].held;
}

void cube_heat_claim(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    HeatLive *h = &s_live[slot];
    bool reseed = h->started || h->read != 0 || h->dirty || h->need_seed;
    *h = (HeatLive){0};
    h->draw = -1;
    h->need_seed = reseed;
}

void cube_heat_retire(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    cube_heat_hold(slot);
    s_live[slot].dirty = true;
}
