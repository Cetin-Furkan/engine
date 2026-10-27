#include "frame/mesh.h"
#include "frame/plot_file.h"
#include "frame/camera.h"
#include "frame/pass.h"
#include "frame/material.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
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

// One slice per submit still on the GPU. The draw slice stays put until a
// newer copy is published, so a failed submit can point back at it.
enum : uint32_t {
    k_slice_count   = 12u,
    k_slice_corners = k_obj_tri_cap * 3u,
};

static_assert((k_slice_corners * sizeof(ObjCorner)) % 16u == 0u, "mesh slice");

typedef struct MeshSlice {
    uint64_t point;
} MeshSlice;

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VkPipeline       s_pipeline = VK_NULL_HANDLE;
static DaBuffer         s_geom;
static VkDevice         s_device = VK_NULL_HANDLE;
static VkDeviceAddress  s_base = 0;
static VkQueue          s_queue = VK_NULL_HANDLE;
static ObjCorner       *s_map = nullptr;
static bool             s_logged = false;
static uint32_t         s_count = 0;
static uint32_t         s_gpu_count = 0;
static uint32_t         s_gen = 0;
static uint32_t         s_shown_gen = 0;
static float            s_radius = 0.0f;
static bool             s_dirty = false;
static bool             s_busy_warned = false;
static MeshSlice        s_slice[k_slice_count];
static int              s_draw_slice = -1;
static VkDeviceAddress  s_draw_addr = 0;
static int              s_armed = -1;
static uint64_t         s_armed_point = 0;
static bool             s_armed_published = false;
static int              s_prev_draw = -1;
static VkDeviceAddress  s_prev_addr = 0;
static uint32_t         s_prev_count = 0;

static ObjCorner s_cpu[k_slice_corners];
static ObjCorner s_held[k_slice_corners];

static void mesh_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Mesh Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static VkDeviceAddress mesh_slice_addr(uint32_t index) {
    return s_base + (VkDeviceAddress)index * (VkDeviceAddress)k_slice_corners * sizeof(ObjCorner);
}

static int create_buffer(VulkanContext *vk) {
    VkDeviceSize bytes = (VkDeviceSize)k_slice_count * k_slice_corners * sizeof(ObjCorner);
    if (da_buffer_create(vk, bytes, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true, &s_geom) != 0) {
        return -1;
    }
    if (s_geom.map == nullptr || s_geom.address == 0) {
        fprintf(stderr, "[Mesh Error] Mesh device address is null\n");
        return -1;
    }
    s_map = s_geom.map;
    s_base = s_geom.address;
    s_device = vk->device;
    memset(s_map, 0, (size_t)bytes);
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
    if (s_base == 0) {
        return false;
    }
    if (corners > k_slice_corners) {
        corners = k_slice_corners;
    }
    bool same = corners == s_count &&
                (corners == 0u || memcmp(s_held, src, (size_t)corners * sizeof(ObjCorner)) == 0);
    if (same) {
        return false;
    }
    if (corners > 0u && src != nullptr) {
        memcpy(s_held, src, (size_t)corners * sizeof(ObjCorner));
    }
    if (corners < s_count) {
        memset(s_held + corners, 0, (size_t)(s_count - corners) * sizeof(ObjCorner));
    }
    s_count = corners;
    s_dirty = true;
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
    int corners = plot_file_load_mesh(s_cpu, k_slice_corners, &found, &capped, &crossed,
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
    s_map = nullptr;
    s_base = 0;
    s_device = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    s_count = 0;
    s_gpu_count = 0;
    s_radius = 0.0f;
    s_dirty = false;
    s_draw_slice = -1;
    s_draw_addr = 0;
    s_armed = -1;
    s_armed_point = 0;
    s_armed_published = false;
    memset(s_slice, 0, sizeof s_slice);
    if (vk != nullptr && vk->device != VK_NULL_HANDLE) {
        if (s_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(vk->device, s_pipeline, nullptr);
        }
        if (s_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        }
    }
    s_pipeline = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE;
    da_buffer_destroy(vk, &s_geom);
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
    VkFormat depth_format = pass_depth_format();
    VkSampleCountFlagBits samples = pass_raster_samples();
    if (depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        fprintf(stderr, "[Mesh Error] The render pass has no depth format yet\n");
        return -1;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(MeshPush)) {
        fprintf(stderr, "[Mesh Error] Push constants are %u bytes, the mesh needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(MeshPush));
        return -1;
    }
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
    VkFormat color_format = VK_FORMAT_B8G8R8A8_SRGB;
    VulkanGfxPipe gfx = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth_format,
        .samples       = samples,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    if (vulkan_gfx_pipelines(vk, &gfx, &color_format, nullptr, 1, &s_pipeline) != 0) {
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
    if (s_base == 0) {
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
    return s_gen != s_shown_gen || s_dirty;
}

void mesh_commit(void) {
    s_shown_gen = s_gen;
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
            return (int)i;
        }
    }
    return -1;
}

void mesh_begin(VkSemaphore acquire, uint64_t signal_point) {
    s_armed = -1;
    s_armed_point = 0;
    s_armed_published = false;
    if (!s_dirty || s_map == nullptr || signal_point == 0u) {
        return;
    }
    int slot = claim_slice(acquire, signal_point);
    if (slot < 0) {
        if (!s_busy_warned) {
            fprintf(stderr, "[Mesh Error] Mesh corners are still on the GPU\n");
            s_busy_warned = true;
        }
        return;
    }
    ObjCorner *dst = s_map + (size_t)slot * k_slice_corners;
    if (s_count > 0u) {
        memcpy(dst, s_held, (size_t)s_count * sizeof(ObjCorner));
    }
    if (s_count < k_slice_corners) {
        memset(dst + s_count, 0, (size_t)(k_slice_corners - s_count) * sizeof(ObjCorner));
    }
    s_prev_draw = s_draw_slice;
    s_prev_addr = s_draw_addr;
    s_prev_count = s_gpu_count;
    s_draw_slice = slot;
    s_draw_addr = mesh_slice_addr((uint32_t)slot);
    s_gpu_count = s_count;
    s_dirty = false;
    s_armed = slot;
    s_armed_point = signal_point;
    s_armed_published = true;
}

void mesh_release(uint64_t signal_point) {
    if (signal_point == 0u) {
        return;
    }
    if (s_armed_published && s_armed_point == signal_point) {
        s_dirty = true;
        s_draw_slice = s_prev_draw;
        s_draw_addr = s_prev_addr;
        s_gpu_count = s_prev_count;
        s_armed_published = false;
    }
    for (uint32_t i = 0; i < k_slice_count; ++i) {
        if (s_slice[i].point == signal_point) {
            s_slice[i].point = 0;
        }
    }
    if (s_armed >= 0 && s_armed_point == signal_point) {
        s_armed = -1;
        s_armed_point = 0;
    }
}

void mesh_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height,
               float yaw_deg, bool selected) {
    if (cmd == VK_NULL_HANDLE || s_pipeline == VK_NULL_HANDLE || s_gpu_count < 3u ||
        s_draw_addr == 0 || width == 0u || height == 0u) {
        return;
    }
    MeshPush draw_push = {
        .verts      = s_draw_addr,
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof draw_push, &draw_push);
    vkCmdDraw(cmd, s_gpu_count, 1, 0, 0);
}
