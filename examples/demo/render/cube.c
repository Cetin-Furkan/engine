#include "frame/clock.h"
#include "frame/cube.h"
#include "frame/camera.h"
#include "frame/grid.h"
#include "frame/pass.h"
#include "render/cube_int.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

// Matches shaders/bevel.comp and shaders/face_grid.slang. k_grid is 16.
// The 888 fillet vertices stay. Each face adds a 16 by 16 grid whose
// corners are those fillet vertices. The bevel rails share that boundary.
// Interior bevel samples are appended after the face grid. One dispatch
// writes both, so the vertex count stays at least the quad count.
enum {
    k_arc          = 8,
    k_grid         = 16,
    k_fillet_verts = 6 * 4 + 12 * (k_arc + 1) * 2 + 8 * (k_arc + 1) * (k_arc + 1),
    k_face_samples = (k_grid + 1) * (k_grid + 1) - 4,
    k_grid_verts   = k_fillet_verts + 6 * k_face_samples,
    k_seam_verts   = 12 * (k_grid - 1) * (k_arc - 1),
    k_verts        = k_grid_verts + k_seam_verts,
    k_face_quads   = 6 * k_grid * k_grid,
    k_edge_quads   = 12 * k_grid * k_arc,
    k_corner_quads = 8 * k_arc * k_arc,
    k_quads        = k_face_quads + k_edge_quads + k_corner_quads,
    k_indices      = k_quads * 6,
    k_local        = 64,
};

static_assert(k_fillet_verts == 888u, "fillet vertex count");
static_assert(k_face_samples == 285u, "face sample count");
static_assert(k_grid_verts == 2598u, "face grid vertex count");
static_assert((uint32_t)k_grid_verts == (uint32_t)k_pick_grid_verts, "pick grid verts");
static_assert((uint32_t)k_verts == (uint32_t)k_pick_verts, "pick verts");
static_assert((uint32_t)k_indices == (uint32_t)k_pick_indices, "pick indices");
static_assert(k_seam_verts == 1260u, "bevel seam vertex count");
static_assert(k_verts == 3858u, "cube vertex count");
static_assert(k_quads == 3584u, "cube quad count");
static_assert(k_indices == 21504u, "cube index count");
static_assert(k_verts >= k_quads, "one bake dispatch covers quads");
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
    float    origin_z;
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
    k_heat_stride = ((k_verts * 32u) + 255u) & ~255u,
};
static_assert(k_heat_stride % 16u == 0u, "heat slice align");
static_assert(k_heat_stride >= k_verts * 32u, "heat slice fits");

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
    float    poke_z;
    float    yaw_deg;
    uint32_t pad;
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
static_assert(offsetof(DrawPush, origin_z) == 60u, "origin z");
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
static_assert(offsetof(HeatPush, poke_z) == 44u, "heat poke z");
static_assert(offsetof(HeatPush, yaw_deg) == 48u, "heat yaw");
static_assert(sizeof(HeatPush) == 56u, "heat push");

static VulkanContext   *s_vk = nullptr;
static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VulkanGfxLive    s_gfx;
static VkPipelineLayout s_compute_layout = VK_NULL_HANDLE;
static VkPipeline       s_compute = VK_NULL_HANDLE;
static DaBuffer         s_mesh_buf = {0};
static DaBuffer         s_index_buf = {0};
static VkDeviceAddress  s_mesh_addr = 0;
static VkDeviceAddress  s_index_addr = 0;
static VkPipelineLayout s_heat_layout = VK_NULL_HANDLE;
static VkPipeline       s_heat_pipe = VK_NULL_HANDLE;
static DaBuffer         s_heat_bufs[k_heat_copies] = {0};
static VkDeviceAddress  s_heat_addr[k_heat_copies] = {0};
// Host mirror used only when the device slab is not host-coherent. A mapped
// device copy is read in place after that submit's acquire wait returns.
static DaBuffer         s_heat_host = {0};
static VkDevice         s_device = VK_NULL_HANDLE;
static VkSemaphore      s_acquire = VK_NULL_HANDLE;

// Last two published copies of one slot, plus the disp the CPU already
// observed. copy is -1 until the first record_ok.
typedef struct HeatPub {
    int      copy;
    uint64_t point;
    int      prev_copy;
    uint64_t prev_point;
    int      cached_copy;
    uint64_t cached_point;
} HeatPub;

static HeatPub s_pub[k_heat_slots];
static float   s_disp_cache[k_heat_slots][k_grid_verts * 3u];

typedef struct HeatLive {
    int      read;
    int      draw;
    uint32_t step_dt;
    bool     started;
    bool     held;
    bool     dirty;
    bool     need_seed;
    float    poke_x;
    float    poke_y;
    float    poke_z;
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


// Inside one command buffer, a later step reads the previous step, so
// that edge targets compute. The draw reads the last write, so that edge
// targets the vertex shader. The next submit waits s_heat_published.
static void barrier_heat(VkCommandBuffer cmd, VkPipelineStageFlags2 dst_stage) {
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = dst_stage,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static void barrier_copy_to_host(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

static bool heat_direct(void) {
    for (uint32_t i = 0; i < k_heat_copies; ++i) {
        if (s_heat_bufs[i].map == nullptr) {
            return false;
        }
    }
    return true;
}

static const uint8_t *heat_slot_bytes(int copy, uint32_t slot) {
    if (copy < 0 || (uint32_t)copy >= k_heat_copies || slot >= k_heat_slots) {
        return nullptr;
    }
    if (s_heat_bufs[copy].map != nullptr) {
        return (const uint8_t *)s_heat_bufs[copy].map + (size_t)slot * k_heat_stride;
    }
    if (s_heat_host.map == nullptr) {
        return nullptr;
    }
    return (const uint8_t *)s_heat_host.map +
           ((size_t)copy * k_heat_slots + slot) * k_heat_stride;
}

static void copy_heat_slot(VkCommandBuffer cmd, int copy, uint32_t slot) {
    if (heat_direct() || copy < 0 || (uint32_t)copy >= k_heat_copies || slot >= k_heat_slots ||
        s_heat_host.buffer == VK_NULL_HANDLE || s_heat_bufs[copy].buffer == VK_NULL_HANDLE) {
        return;
    }
    VkBufferCopy region = {
        .srcOffset = s_heat_bufs[copy].offset + (VkDeviceSize)slot * (VkDeviceSize)k_heat_stride,
        .dstOffset = s_heat_host.offset +
                     ((VkDeviceSize)copy * k_heat_slots + slot) * (VkDeviceSize)k_heat_stride,
        .size = (VkDeviceSize)k_verts * 32u,
    };
    vkCmdCopyBuffer(cmd, s_heat_bufs[copy].buffer, s_heat_host.buffer, 1, &region);
}

// The draw reads this slot. A mapped device copy is visible to the CPU
// after the acquire wait. Otherwise the same command buffer copies the
// slot into the host mirror.
static void publish_heat_slot(VkCommandBuffer cmd, int copy, uint32_t slot) {
    VkPipelineStageFlags2 dst_stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    VkAccessFlags2 dst_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    if (heat_direct()) {
        dst_stage |= VK_PIPELINE_STAGE_2_HOST_BIT;
        dst_access |= VK_ACCESS_2_HOST_READ_BIT;
    } else {
        dst_stage |= VK_PIPELINE_STAGE_2_COPY_BIT;
        dst_access |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    VkMemoryBarrier2 barrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = dst_stage,
        .dstAccessMask = dst_access,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    if (!heat_direct()) {
        copy_heat_slot(cmd, copy, slot);
        if (s_heat_host.buffer != VK_NULL_HANDLE) {
            barrier_copy_to_host(cmd);
        }
    }
}

static void drop_pair(VulkanContext *vk, DaBuffer *a, DaBuffer *b) {
    da_buffer_destroy(vk, a);
    da_buffer_destroy(vk, b);
}

static bool heat_seed_looks_zero(void) {
    const uint8_t *base = heat_slot_bytes(0, 0u);
    if (base == nullptr) {
        fprintf(stderr, "[Cube Error] Heat readback is not mapped\n");
        return false;
    }
    const uint32_t ids[4] = { 0u, 10u, 100u, 1000u };
    for (uint32_t n = 0; n < 4u; ++n) {
        float d[3];
        memcpy(d, base + (size_t)ids[n] * 32u, sizeof d);
        if (!isfinite(d[0]) || !isfinite(d[1]) || !isfinite(d[2]) || fabsf(d[0]) > 1.0e-4f ||
            fabsf(d[1]) > 1.0e-4f || fabsf(d[2]) > 1.0e-4f) {
            fprintf(stderr, "[Cube Error] Heat readback disp at %u is %g %g %g\n", ids[n], (double)d[0],
                    (double)d[1], (double)d[2]);
            return false;
        }
    }
    return true;
}

static void barrier_heat_to_compute(VkCommandBuffer cmd) {
    barrier_heat(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

static bool heat_slot_ok(uint32_t slot) {
    return slot < k_heat_slots;
}

static void pub_clear_slot(uint32_t slot) {
    s_pub[slot] = (HeatPub){
        .copy        = -1,
        .prev_copy   = -1,
        .cached_copy = -1,
    };
    memset(s_disp_cache[slot], 0, sizeof s_disp_cache[slot]);
}

static void heat_live_clear(void) {
    for (uint32_t i = 0; i < k_heat_slots; ++i) {
        s_live[i] = (HeatLive){0};
        s_live[i].draw = -1;
        pub_clear_slot(i);
    }
    s_heat_announced = false;
    s_heat_published = 0;
}

static void pub_note(uint32_t slot, int copy, uint64_t point) {
    if (!heat_slot_ok(slot) || copy < 0 || point == 0) {
        return;
    }
    HeatPub *p = &s_pub[slot];
    if (p->point == point && p->copy == copy) {
        return;
    }
    p->prev_copy = p->copy;
    p->prev_point = p->point;
    p->copy = copy;
    p->point = point;
}

static bool host_sees(uint64_t point) {
    if (point == 0 || s_device == VK_NULL_HANDLE || s_acquire == VK_NULL_HANDLE ||
        (!heat_direct() && s_heat_host.map == nullptr)) {
        return false;
    }
    VkSemaphoreWaitInfo wait_info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &s_acquire,
        .pValues        = &point,
    };
    return vkWaitSemaphores(s_device, &wait_info, 0) == VK_SUCCESS;
}

static void copy_slot_disp(uint32_t slot, int copy, float *xyz) {
    const uint8_t *base = heat_slot_bytes(copy, slot);
    if (base == nullptr) {
        memset(xyz, 0, sizeof(float) * 3u * k_grid_verts);
        return;
    }
    for (uint32_t i = 0; i < k_grid_verts; ++i) {
        memcpy(xyz + (size_t)i * 3u, base + (size_t)i * 32u, sizeof(float) * 3u);
    }
}

void cube_heat_shown_disp(uint32_t slot, float *xyz) {
    if (xyz == nullptr) {
        return;
    }
    if (!heat_slot_ok(slot)) {
        memset(xyz, 0, sizeof(float) * 3u * k_grid_verts);
        return;
    }
    HeatPub *p = &s_pub[slot];
    int copy = -1;
    uint64_t point = 0;
    if (p->copy >= 0 && host_sees(p->point)) {
        copy = p->copy;
        point = p->point;
    } else if (p->prev_copy >= 0 && host_sees(p->prev_point)) {
        copy = p->prev_copy;
        point = p->prev_point;
    }
    if (copy >= 0 && (p->cached_point != point || p->cached_copy != copy)) {
        copy_slot_disp(slot, copy, s_disp_cache[slot]);
        p->cached_copy = copy;
        p->cached_point = point;
    }
    if (p->cached_copy < 0) {
        memset(xyz, 0, sizeof(float) * 3u * k_grid_verts);
        return;
    }
    memcpy(xyz, s_disp_cache[slot], sizeof s_disp_cache[slot]);
}

void cube_bind_acquire(VkSemaphore acquire) {
    if (acquire != VK_NULL_HANDLE) {
        s_acquire = acquire;
    }
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
        .poke_z  = poke != 0u ? live->poke_z : 0.0f,
        .yaw_deg = yaw_deg,
        .pad     = 0u,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_heat_pipe);
    vulkan_cmd_push(cmd, s_heat_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof heat_push, &heat_push);
    vkCmdDispatch(cmd, (k_verts + k_local - 1u) / k_local, 1u, 1u);
}

// One submit, then the queue is idle. The seed is visible without an acquire
// wait. A live step publishes the acquire value of its own submit.
static int bake_mesh(VulkanContext *vk) {
    VkCommandPool pool = vk->compute_cmd_pool != VK_NULL_HANDLE ? vk->compute_cmd_pool : vk->cmd_pool;
    VkQueue queue = vk->queues.compute_queue != VK_NULL_HANDLE ? vk->queues.compute_queue : vk->queues.graphics_queue;
    if (pool == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || s_compute == VK_NULL_HANDLE) {
        fprintf(stderr, "[Cube Error] Mesh bake needs compute queue and command pool\n");
        return -1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = pool,
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
        vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
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
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mesh,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    if (s_heat_pipe == VK_NULL_HANDLE || s_heat_addr[0] == 0) {
        fprintf(stderr, "[Cube Error] Heat seed needs its pipeline and buffer\n");
        vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
        return -1;
    }
    for (uint32_t slot = 0; slot < k_heat_slots; ++slot) {
        dispatch_heat(cmd, 0, 1u, -1, 0, slot, 0u, 0.0f);
    }
    DaBuffer mesh_host = {0};
    DaBuffer index_host = {0};
    if (da_buffer_create(vk, (VkDeviceSize)k_verts * 36u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &mesh_host) != 0 ||
        da_buffer_create(vk, (VkDeviceSize)k_indices * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                         &index_host) != 0 ||
        mesh_host.map == nullptr || index_host.map == nullptr) {
        fprintf(stderr, "[Cube Error] Mesh readback buffer was not mapped\n");
        drop_pair(vk, &mesh_host, &index_host);
        vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
        return -1;
    }
    VkMemoryBarrier2 to_copy = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
    };
    VkDependencyInfo copy_dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &to_copy,
    };
    vkCmdPipelineBarrier2(cmd, &copy_dep);
    VkBufferCopy mesh_region = {
        .srcOffset = s_mesh_buf.offset,
        .dstOffset = mesh_host.offset,
        .size      = (VkDeviceSize)k_verts * 36u,
    };
    VkBufferCopy index_region = {
        .srcOffset = s_index_buf.offset,
        .dstOffset = index_host.offset,
        .size      = (VkDeviceSize)k_indices * sizeof(uint16_t),
    };
    vkCmdCopyBuffer(cmd, s_mesh_buf.buffer, mesh_host.buffer, 1, &mesh_region);
    vkCmdCopyBuffer(cmd, s_index_buf.buffer, index_host.buffer, 1, &index_region);
    if (heat_direct()) {
        VkMemoryBarrier2 host = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        VkDependencyInfo host_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &host,
        };
        vkCmdPipelineBarrier2(cmd, &host_dep);
    } else {
        copy_heat_slot(cmd, 0, 0u);
        barrier_copy_to_host(cmd);
    }
    res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS) {
        cube_fail("vkEndCommandBuffer bake", res);
        drop_pair(vk, &mesh_host, &index_host);
        vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
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
    res = vkQueueSubmit2(queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        cube_fail("vkQueueSubmit2 bake", res);
        drop_pair(vk, &mesh_host, &index_host);
        vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
        return -1;
    }
    res = vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(vk->device, pool, 1, &cmd);
    bool stored = res == VK_SUCCESS && cube_pick_store_rest(mesh_host.map, index_host.map);
    bool seed_ok = stored && heat_seed_looks_zero();
    bool pick_ok = seed_ok && cube_surface_self_check();
    drop_pair(vk, &mesh_host, &index_host);
    if (res != VK_SUCCESS) {
        cube_fail("vkQueueWaitIdle bake", res);
        return -1;
    }
    if (!stored || !seed_ok || !pick_ok) {
        return -1;
    }
    return 0;
}

int cube_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    s_device = vk->device;
    s_vk = vk;
    if (s_layout != VK_NULL_HANDLE) {
        return 0;
    }
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits sample_count = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || sample_count == 0) {
        fprintf(stderr, "[Cube Error] The render pass has no depth format yet\n");
        return -1;
    }

    if (da_buffer_create(vk, (VkDeviceSize)k_verts * 36u,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false,
                         &s_mesh_buf) != 0 ||
        da_buffer_create(vk, (VkDeviceSize)k_indices * sizeof(uint16_t),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         false, &s_index_buf) != 0) {
        cube_shutdown(vk);
        return -1;
    }
    s_mesh_addr = s_mesh_buf.address;
    s_index_addr = s_index_buf.address;

    heat_live_clear();
    for (uint32_t i = 0; i < k_heat_copies; ++i) {
        if (da_buffer_create(vk, (VkDeviceSize)k_heat_slots * (VkDeviceSize)k_heat_stride,
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false,
                             &s_heat_bufs[i]) != 0) {
            cube_shutdown(vk);
            return -1;
        }
        s_heat_addr[i] = s_heat_bufs[i].address;
    }
    if (!heat_direct()) {
        if (da_buffer_create(vk, (VkDeviceSize)k_heat_copies * (VkDeviceSize)k_heat_slots * (VkDeviceSize)k_heat_stride,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &s_heat_host) != 0 ||
            s_heat_host.map == nullptr) {
            fprintf(stderr, "[Cube Error] Heat mirror was not mapped\n");
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
    vulkan_gfx_arm(&s_gfx, &gfx, VK_FORMAT_B8G8R8A8_SRGB, nullptr);

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
    printf("[Cube] 240px centered, %u verts, bevel welded to the %u by %u face, %ux\n",
           k_verts, k_grid, k_grid, sample_count_n(sample_count));
    if (heat_direct()) {
        printf("[Cube] Heat is read from the mapped device slab.\n");
    } else {
        printf("[Cube] Heat is copied to a host mirror.\n");
    }
    fflush(stdout);
    return 0;
}

static void release_heat_copies(VulkanContext *vk) {
    for (uint32_t i = 0; i < k_heat_copies; ++i) {
        da_buffer_destroy(vk, &s_heat_bufs[i]);
        s_heat_addr[i] = 0;
    }
    da_buffer_destroy(vk, &s_heat_host);
    s_acquire = VK_NULL_HANDLE;
    s_device = VK_NULL_HANDLE;
    cube_pick_reset();
    heat_live_clear();
}

void cube_shutdown(VulkanContext *vk) {
    s_mesh_addr = 0;
    s_index_addr = 0;
    vulkan_gfx_disarm(vk, &s_gfx);
    s_vk = nullptr;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_compute = VK_NULL_HANDLE;
        s_compute_layout = VK_NULL_HANDLE;
        s_heat_pipe = VK_NULL_HANDLE;
        s_heat_layout = VK_NULL_HANDLE;
        release_heat_copies(vk);
        da_buffer_destroy(vk, &s_mesh_buf);
        da_buffer_destroy(vk, &s_index_buf);
        return;
    }
    if (s_compute != VK_NULL_HANDLE) {
        vulkan_pipe_release(vk, s_compute);
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
        vulkan_pipe_release(vk, s_heat_pipe);
        s_heat_pipe = VK_NULL_HANDLE;
    }
    if (s_heat_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_heat_layout, nullptr);
        s_heat_layout = VK_NULL_HANDLE;
    }
    release_heat_copies(vk);
    da_buffer_destroy(vk, &s_mesh_buf);
    da_buffer_destroy(vk, &s_index_buf);
}

void cube_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, float yaw_deg,
               float origin_x, float origin_y, float origin_z, float depth_bias, float phase,
               bool selected, uint32_t mat, uint32_t heat_slot) {
    VkPipeline pipeline = vulkan_gfx_use(s_vk, &s_gfx);
    if (cmd == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE || width == 0 || height == 0 ||
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
        .origin_z   = origin_z,
    };
    view_push_fill(&draw_push.view, mat);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindIndexBuffer2(cmd, s_index_buf.buffer, s_index_buf.offset,
                          (VkDeviceSize)k_indices * sizeof(uint16_t),
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

uint32_t cube_heat_interval(uint32_t slot, uint32_t sim_dt_ms) {
    if (!heat_slot_ok(slot)) {
        return 0;
    }
    HeatLive *h = &s_live[slot];
    if (h->held) {
        h->step_dt = 0;
        return 0;
    }
    if (sim_dt_ms == 0u) {
        return h->step_dt;
    }
    // Several timer ticks can land before one present. Keep the sum, and
    // let the record clear it. A hitch stops at five quanta.
    uint32_t cap = FRAME_SIM_STEP_MS * FRAME_SIM_MAX_STEPS;
    uint32_t sum = h->step_dt + sim_dt_ms;
    if (sum < h->step_dt || sum > cap) {
        sum = cap;
    }
    h->step_dt = sum;
    return sum;
}

uint32_t cube_heat_step_dt(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return 0;
    }
    return s_live[slot].step_dt;
}

bool cube_heat_record(VkCommandBuffer cmd, uint32_t slot, float yaw_deg, uint32_t dt_ms) {
    if (!heat_slot_ok(slot) || cmd == VK_NULL_HANDLE || s_heat_pipe == VK_NULL_HANDLE) {
        return false;
    }
    HeatLive *h = &s_live[slot];
    if (h->draw >= 0 || s_mesh_addr == 0 || h->held) {
        return false;
    }
    int dst = (h->read + 1) % (int)k_heat_copies;
    if (heat_at(dst, slot) == 0) {
        return false;
    }
    if (h->need_seed) {
        dispatch_heat(cmd, 0, 1u, -1, dst, slot, 0u, yaw_deg);
        publish_heat_slot(cmd, dst, slot);
        h->draw = dst;
        h->need_seed = false;
        return true;
    }
    bool poke = h->poke_pending;
    uint32_t cap = FRAME_SIM_STEP_MS * FRAME_SIM_MAX_STEPS;
    if (dt_ms > cap) {
        dt_ms = cap;
    }
    if ((dt_ms == 0u && !poke) || heat_at(h->read, slot) == 0) {
        return false;
    }
    int src = h->read;
    uint32_t left = dt_ms;
    bool consumed_poke = false;
    for (;;) {
        uint32_t step = 0;
        if (left > 0u) {
            step = left > FRAME_SIM_STEP_MS ? FRAME_SIM_STEP_MS : left;
        } else if (!poke) {
            break;
        }
        dst = (src + 1) % (int)k_heat_copies;
        if (heat_at(dst, slot) == 0) {
            return false;
        }
        dispatch_heat(cmd, step, 0u, src, dst, slot, poke ? 1u : 0u, yaw_deg);
        if (poke) {
            consumed_poke = true;
            poke = false;
        }
        src = dst;
        left -= step;
        if (left > 0u) {
            barrier_heat_to_compute(cmd);
            continue;
        }
        publish_heat_slot(cmd, src, slot);
        break;
    }
    h->draw = src;
    h->step_dt = 0;
    if (consumed_poke) {
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
    int written = h->draw;
    h->read = h->draw;
    h->draw = -1;
    h->poke_armed = false;
    if (signal_point != 0) {
        s_heat_published = signal_point;
        pub_note(slot, written, signal_point);
    }
    h->started = true;
    if (s_heat_announced) {
        return;
    }
    s_heat_announced = true;
    printf("[Cube] Fillet heat started. Each cube keeps its own field. Upper half, 8 ms steps, then it holds.\n");
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

bool cube_heat_poke(uint32_t slot, float origin_x, float origin_y, float origin_z) {
    if (!heat_slot_ok(slot) || s_heat_pipe == VK_NULL_HANDLE) {
        return false;
    }
    HeatLive *h = &s_live[slot];
    h->poke_x = origin_x;
    h->poke_y = origin_y;
    h->poke_z = origin_z;
    h->poke_pending = true;
    h->held = false;
    h->pulse = true;
    h->pulse_have_t0 = false;
    printf("[Cube] Non-rigid impulse & heat poked at %+.0f,%+.0f,%+.0f.\n",
           (double)origin_x, (double)origin_y, (double)origin_z);
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
    pub_clear_slot(slot);
}

void cube_heat_retire(uint32_t slot) {
    if (!heat_slot_ok(slot)) {
        return;
    }
    cube_heat_hold(slot);
    s_live[slot].dirty = true;
}
