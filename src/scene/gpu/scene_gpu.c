#include "scene/scene.h"
#include "lighting/lighting.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include "vulkan/pipe.h"
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_cull_comp_spv[] = {
#embed "scene_cull.comp.spv"
};
alignas(uint32_t) static const unsigned char k_hiz_comp_spv[] = {
#embed "scene_hiz.comp.spv"
};
alignas(uint32_t) static const unsigned char k_visbuf_vert_spv[] = {
#embed "scene_visbuf.vert.spv"
};
alignas(uint32_t) static const unsigned char k_visbuf_frag_spv[] = {
#embed "scene_visbuf.frag.spv"
};
alignas(uint32_t) static const unsigned char k_resolve_vert_spv[] = {
#embed "scene_visbuf_resolve.vert.spv"
};
alignas(uint32_t) static const unsigned char k_resolve_frag_spv[] = {
#embed "scene_visbuf_resolve.frag.spv"
};
alignas(uint32_t) static const unsigned char k_meshlet_vert_spv[] = {
#embed "scene_meshlet.vert.spv"
};
alignas(uint32_t) static const unsigned char k_meshlet_frag_spv[] = {
#embed "scene_meshlet.frag.spv"
};

static_assert(sizeof k_cull_comp_spv % 4u == 0u, "scene_cull.comp.spv align");
static_assert(sizeof k_hiz_comp_spv % 4u == 0u, "scene_hiz.comp.spv align");
static_assert(sizeof k_visbuf_vert_spv % 4u == 0u, "scene_visbuf.vert.spv align");
static_assert(sizeof k_visbuf_frag_spv % 4u == 0u, "scene_visbuf.frag.spv align");
static_assert(sizeof k_resolve_vert_spv % 4u == 0u, "scene_visbuf_resolve.vert.spv align");
static_assert(sizeof k_resolve_frag_spv % 4u == 0u, "scene_visbuf_resolve.frag.spv align");
static_assert(sizeof k_meshlet_vert_spv % 4u == 0u, "scene_meshlet.vert.spv align");
static_assert(sizeof k_meshlet_frag_spv % 4u == 0u, "scene_meshlet.frag.spv align");

enum : uint32_t {
    k_vert_slab_cap      = 32768u,
    k_tri_slab_cap       = 65536u,
    k_cull_ring_slots    = 4u,
    k_max_draws          = 1024u,
    k_hiz_auto_uma_min   = 128u,
    k_hiz_auto_dgpu_min  = 64u,
};

typedef enum SceneHizPolicy : uint8_t {
    SCENE_HIZ_OFF  = 0u,
    SCENE_HIZ_ON   = 1u,
    SCENE_HIZ_AUTO = 2u,
} SceneHizPolicy;

enum : size_t {
    k_slab_off_meshlets  = 0u,
    k_slab_off_vertices  = K_SCENE_MAX_MESHLETS * sizeof(SceneMeshlet),
    k_slab_off_triangles = k_slab_off_vertices + (size_t)k_vert_slab_cap * sizeof(ScenePackedVertex),
    k_slab_total_bytes   = k_slab_off_triangles + (size_t)k_tri_slab_cap * sizeof(uint32_t),
};

typedef struct SceneHizMipInfo {
    uint32_t offset_floats;
    uint32_t width;
    uint32_t height;
    uint32_t pad;
} SceneHizMipInfo;

typedef struct SceneHizHeader {
    alignas(64) uint32_t full_width;
    uint32_t        full_height;
    uint32_t        mip_count;
    uint32_t        valid;
    float           prev_qx, prev_qy, prev_qz, prev_qw;
    float           prev_cam_x, prev_cam_y, prev_cam_z, prev_zoom;
    uint32_t        pad[4];
    SceneHizMipInfo mips[K_SCENE_HIZ_MAX_MIPS];
} SceneHizHeader;

static_assert(sizeof(SceneHizHeader) == 256u, "SceneHizHeader must be 256 bytes");

typedef struct SceneDrawIndirectCmd {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
} SceneDrawIndirectCmd;

static_assert(sizeof(SceneDrawIndirectCmd) == 16u, "SceneDrawIndirectCmd must be 16 bytes");

typedef struct SceneCullSlot {
    alignas(64) uint32_t phase1_draw_count;
    uint32_t             phase2_draw_count;
    uint32_t             rejected_count;
    uint32_t             active_meshlets;
    uint32_t             cone_culled;
    uint32_t             frustum_culled;
    uint32_t             hiz_culled;
    uint32_t             pad_hdr[9];
    SceneDrawIndirectCmd phase1_single_cmd;
    SceneDrawIndirectCmd phase2_single_cmd;
    uint32_t             pad_single[8];
    SceneDrawIndirectCmd phase1_cmds[k_max_draws];
    SceneDrawIndirectCmd phase2_cmds[k_max_draws];
    uint32_t             rejected_pairs[k_max_draws];
    uint32_t             work_pairs[k_max_draws];
    SceneInstance        instances[K_SCENE_MAX_INSTANCES];
} SceneCullSlot;

static_assert(offsetof(SceneCullSlot, phase1_single_cmd) == 64u, "slot single1 offset");
static_assert(offsetof(SceneCullSlot, phase2_single_cmd) == 80u, "slot single2 offset");
static_assert(offsetof(SceneCullSlot, phase1_cmds) == 128u, "slot cmds1 offset");
static_assert(offsetof(SceneCullSlot, phase2_cmds) == 16512u, "slot cmds2 offset");
static_assert(offsetof(SceneCullSlot, rejected_pairs) == 32896u, "slot rejected offset");
static_assert(offsetof(SceneCullSlot, work_pairs) == 36992u, "slot work offset");
static_assert(offsetof(SceneCullSlot, instances) == 41088u, "slot instances offset");

typedef struct SceneCullPush {
    uint64_t meshlets_bda;
    uint64_t cull_slot_bda;
    uint64_t hiz_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    uint32_t width;
    uint32_t height;
    uint32_t work_count;
    uint32_t phase;
    uint32_t enable_hiz;
    uint32_t pad0;
} SceneCullPush;

typedef struct SceneHizPush {
    uint64_t hiz_bda;
    uint64_t cull_slot_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    uint32_t mode;
    uint32_t commit_prev;
    uint32_t pad0;
    uint32_t pad1;
} SceneHizPush;

typedef struct SceneDrawPush {
    uint64_t        meshlets_bda;
    uint64_t        vertices_bda;
    uint64_t        triangles_bda;
    uint64_t        cull_slot_bda;
    uint32_t        width;
    uint32_t        height;
    uint32_t        phase_cmd_mode;
    uint32_t        pad0;
    SceneViewParams view;
} SceneDrawPush;

static_assert(sizeof(SceneCullPush) == 80u, "SceneCullPush size");
static_assert(sizeof(SceneHizPush) == 64u, "SceneHizPush size");
static_assert(offsetof(SceneDrawPush, view) == 48u, "SceneDrawPush view offset");
static_assert(sizeof(SceneDrawPush) == 96u, "SceneDrawPush size");

typedef struct SceneMeshRecord {
    bool     used;
    uint32_t first_meshlet;
    uint32_t meshlet_count;
    uint32_t vert_offset;
    uint32_t vert_count;
    uint32_t tri_offset;
    uint32_t tri_count;
    float    local_center[3];
    float    half_extents[3];
    float    avg_albedo[3];
    float    avg_roughness;
} SceneMeshRecord;

typedef struct SceneTargetImage {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint32_t       width;
    uint32_t       height;
} SceneTargetImage;

static VulkanContext        *s_vk              = nullptr;
static VkFormat              s_pass_depth_fmt  = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_pass_samples    = VK_SAMPLE_COUNT_1_BIT;
static bool                  s_ready           = false;
static bool                  s_gpu_ready       = false;
static bool                  s_visbuf_enabled  = false;
static bool                  s_can_hiz_push    = false;
static SceneHizPolicy        s_hiz_policy      = SCENE_HIZ_AUTO;
static bool                  s_used_hiz_this_frame = false;

static DaBuffer              s_geom_slab       = {0};
static DaBuffer              s_cull_buf        = {0};
static DaBuffer              s_hiz_buf         = {0};

static SceneMeshRecord       s_meshes[K_SCENE_MAX_MESHES];
static uint32_t              s_mesh_count      = 0u;
static uint32_t              s_slab_ml_cursor  = 0u;
static uint32_t              s_slab_vx_cursor  = 0u;
static uint32_t              s_slab_tr_cursor  = 0u;

static SceneTargetImage      s_vis_depth       = {0};
static SceneTargetImage      s_vis_color       = {0};
static SceneHizHeader        s_hiz_hdr_cpu     = {0};
static bool                  s_hiz_header_dirty = true;

static VkDescriptorSetLayout s_tex_set_layout  = VK_NULL_HANDLE;
static VkPipelineLayout      s_cull_layout     = VK_NULL_HANDLE;
static VkPipeline            s_cull_pipe       = VK_NULL_HANDLE;
static VkPipelineLayout      s_hiz_layout      = VK_NULL_HANDLE;
static VkPipeline            s_hiz_pipe        = VK_NULL_HANDLE;
static VkPipelineLayout      s_draw_layout     = VK_NULL_HANDLE;
static VulkanGfxLive         s_depth_only_gfx  = {0};
static VulkanGfxLive         s_visbuf_gfx      = {0};
static VkPipelineLayout      s_resolve_layout  = VK_NULL_HANDLE;
static VulkanGfxLive         s_resolve_gfx     = {0};
static VulkanGfxLive         s_meshlet_gfx     = {0};

static uint32_t              s_frame_slot      = 0u;
static uint64_t              s_frame_counter   = 0u;
static uint32_t              s_cur_instances   = 0u;
static uint32_t              s_cur_work_items  = 0u;
static SceneViewParams       s_cur_view        = {0};
static bool                  s_prepared_this_frame = false;

static bool env_contains_ci(const char *hay, const char *needle) {
    if (hay == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    size_t nlen = strlen(needle);
    for (size_t i = 0u; hay[i] != '\0'; ++i) {
        size_t j = 0u;
        while (j < nlen && hay[i + j] != '\0' &&
               tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

static void destroy_target_image(VulkanContext *vk, SceneTargetImage *img) {
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && img != nullptr) {
        if (img->view != VK_NULL_HANDLE) {
            vkDestroyImageView(vk->device, img->view, nullptr);
        }
        if (img->image != VK_NULL_HANDLE) {
            vkDestroyImage(vk->device, img->image, nullptr);
        }
        if (img->memory != VK_NULL_HANDLE) {
            vkFreeMemory(vk->device, img->memory, nullptr);
        }
    }
    if (img != nullptr) {
        *img = (SceneTargetImage){0};
    }
}

static int create_target_image(VulkanContext *vk, uint32_t width, uint32_t height,
                               VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, SceneTargetImage *out) {
    destroy_target_image(vk, out);

    VkImageCreateInfo img_info = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { .width = width, .height = height, .depth = 1u },
        .mipLevels     = 1u,
        .arrayLayers   = 1u,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(vk->device, &img_info, nullptr, &out->image) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    VkMemoryRequirements reqs = {0};
    vkGetImageMemoryRequirements(vk->device, out->image, &reqs);
    uint32_t mem_type = 0u;
    if (vulkan_find_memory_type(vk, reqs.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mem_type) != VULKAN_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = reqs.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(vk->device, &alloc_info, nullptr, &out->memory) != VK_SUCCESS ||
        vkBindImageMemory(vk->device, out->image, out->memory, 0u) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    VkImageViewCreateInfo view_info = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = aspect,
            .baseMipLevel   = 0u,
            .levelCount     = 1u,
            .baseArrayLayer = 0u,
            .layerCount     = 1u,
        },
    };
    if (vkCreateImageView(vk->device, &view_info, nullptr, &out->view) != VK_SUCCESS) {
        destroy_target_image(vk, out);
        return -1;
    }

    out->width  = width;
    out->height = height;
    return 0;
}

static int ensure_screen_resources(uint32_t width, uint32_t height,
                                   bool need_vis_color, bool need_hiz_buf) {
    if (s_vk == nullptr || width == 0u || height == 0u) {
        return -1;
    }
    const bool depth_ok = s_vis_depth.image != VK_NULL_HANDLE &&
                          s_vis_depth.width == width && s_vis_depth.height == height;
    const bool color_ok = !need_vis_color ||
                          (s_vis_color.image != VK_NULL_HANDLE &&
                           s_vis_color.width == width && s_vis_color.height == height);
    const bool hiz_ok   = !need_hiz_buf ||
                          (s_hiz_buf.buffer != VK_NULL_HANDLE &&
                           s_hiz_hdr_cpu.full_width == width &&
                           s_hiz_hdr_cpu.full_height == height);
    if (depth_ok && color_ok && hiz_ok) {
        return 0;
    }

    (void)vkDeviceWaitIdle(s_vk->device);

    if (!depth_ok) {
        if (create_target_image(s_vk, width, height, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT, &s_vis_depth) != 0) {
            return -1;
        }
    }

    if (need_vis_color && !color_ok) {
        if (create_target_image(s_vk, width, height, VK_FORMAT_R32_UINT,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, &s_vis_color) != 0) {
            return -1;
        }
    }

    if (!need_hiz_buf) {
        return 0;
    }

    // Build Hi-Z mip pyramid layout for (width, height): Mip 0 = ceil(w/2) x ceil(h/2) .. 1x1.
    SceneHizHeader hdr = {0};
    hdr.full_width  = width;
    hdr.full_height = height;
    hdr.valid       = 0u;

    uint32_t mw = (width + 1u) >> 1u;
    uint32_t mh = (height + 1u) >> 1u;
    uint32_t float_off = 0u;
    uint32_t mips = 0u;
    while (mips < K_SCENE_HIZ_MAX_MIPS) {
        hdr.mips[mips] = (SceneHizMipInfo){
            .offset_floats = float_off,
            .width         = mw,
            .height        = mh,
            .pad           = 0u,
        };
        float_off += mw * mh;
        mips++;
        if (mw == 1u && mh == 1u) {
            break;
        }
        mw = (mw + 1u) >> 1u;
        mh = (mh + 1u) >> 1u;
    }
    hdr.mip_count = mips;
    s_hiz_hdr_cpu = hdr;
    s_hiz_header_dirty = true;

    VkDeviceSize needed_bytes = 256ull + (VkDeviceSize)float_off * sizeof(float);
    needed_bytes = (needed_bytes + 255ull) & ~255ull;
    if (s_hiz_buf.buffer == VK_NULL_HANDLE || s_hiz_buf.bytes < needed_bytes) {
        if (da_buffer_replace(s_vk, &s_hiz_buf, needed_bytes,
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              false) != 0) {
            return -1;
        }
    }
    return 0;
}

static int upload_slab_bytes(VkDeviceSize dst_off, const void *src, VkDeviceSize bytes) {
    if (bytes == 0u) {
        return 0;
    }
    if (s_geom_slab.buffer == VK_NULL_HANDLE || dst_off + bytes > s_geom_slab.bytes) {
        return -1;
    }
    // UMA_IGPU Zero-Copy Fast-Path: direct memcpy into LLC-cached device-local BDA slab!
    if (s_geom_slab.map != nullptr) {
        memcpy((uint8_t *)s_geom_slab.map + dst_off, src, (size_t)bytes);
        return 0;
    }

    // DISCRETE_DIRECT / HYBRID_MUXLESS Dedicated VRAM Path: stage & DMA copy into VRAM.
    DaBuffer stage = {0};
    if (da_buffer_create(s_vk, bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &stage) != 0 || stage.map == nullptr) {
        da_buffer_destroy(s_vk, &stage);
        return -1;
    }
    memcpy(stage.map, src, (size_t)bytes);

    VkCommandBufferAllocateInfo alloc_info = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = s_vk->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s_vk->device, &alloc_info, &cmd) != VK_SUCCESS) {
        da_buffer_destroy(s_vk, &stage);
        return -1;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    (void)vkBeginCommandBuffer(cmd, &begin_info);
    VkBufferCopy2 region = {
        .sType     = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .srcOffset = stage.offset,
        .dstOffset = s_geom_slab.offset + dst_off,
        .size      = bytes,
    };
    VkCopyBufferInfo2 copy_info = {
        .sType       = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer   = stage.buffer,
        .dstBuffer   = s_geom_slab.buffer,
        .regionCount = 1u,
        .pRegions    = &region,
    };
    vkCmdCopyBuffer2(cmd, &copy_info);
    (void)vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo csi = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit = {
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1u,
        .pCommandBufferInfos    = &csi,
    };
    VkResult res = vkQueueSubmit2(s_vk->queues.graphics_queue, 1u, &submit, VK_NULL_HANDLE);
    if (res == VK_SUCCESS) {
        res = vkQueueWaitIdle(s_vk->queues.graphics_queue);
    }
    vkFreeCommandBuffers(s_vk->device, s_vk->cmd_pool, 1u, &cmd);
    da_buffer_destroy(s_vk, &stage);
    return res == VK_SUCCESS ? 0 : -1;
}

static void compute_mesh_proxy_meta(SceneMeshRecord *rec, const SceneMeshletBuild *b) {
    if (rec == nullptr || b == nullptr || b->meshlet_count == 0u) {
        return;
    }
    float bmin[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
    float bmax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
    for (uint32_t i = 0u; i < b->meshlet_count; ++i) {
        const SceneMeshlet *ml = &b->meshlets[i];
        for (int a = 0; a < 3; ++a) {
            float lo = ml->origin[a];
            float hi = ml->origin[a] + ml->scale[a];
            if (lo < bmin[a]) bmin[a] = lo;
            if (hi > bmax[a]) bmax[a] = hi;
        }
    }
    for (int a = 0; a < 3; ++a) {
        rec->local_center[a] = 0.5f * (bmin[a] + bmax[a]);
        rec->half_extents[a] = fmaxf(0.5f * (bmax[a] - bmin[a]), 1.0e-3f);
    }

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_a = 0.0;
    uint32_t vc  = b->vertex_count;
    for (uint32_t v = 0u; v < vc; ++v) {
        uint32_t rgba = b->vertices[v].rgba8;
        sum_r += (double)( rgba         & 0xFFu) * (1.0 / 255.0);
        sum_g += (double)((rgba >>  8u) & 0xFFu) * (1.0 / 255.0);
        sum_b += (double)((rgba >> 16u) & 0xFFu) * (1.0 / 255.0);
        sum_a += (double)((rgba >> 24u) & 0xFFu) * (1.0 / 255.0);
    }
    if (vc > 0u) {
        double inv = 1.0 / (double)vc;
        rec->avg_albedo[0] = (float)(sum_r * inv);
        rec->avg_albedo[1] = (float)(sum_g * inv);
        rec->avg_albedo[2] = (float)(sum_b * inv);
        rec->avg_roughness = (float)(sum_a * inv);
    } else {
        rec->avg_albedo[0] = 0.5f;
        rec->avg_albedo[1] = 0.5f;
        rec->avg_albedo[2] = 0.5f;
        rec->avg_roughness = 0.35f;
    }
}

static int upload_mesh_build(uint32_t mesh_id, const SceneMeshletBuild *b) {
    if (mesh_id >= K_SCENE_MAX_MESHES || b == nullptr) {
        return -1;
    }
    if (b->meshlet_count == 0u) {
        s_meshes[mesh_id] = (SceneMeshRecord){ .used = true };
        return 0;
    }

    if (s_slab_ml_cursor + b->meshlet_count > K_SCENE_MAX_MESHLETS ||
        s_slab_vx_cursor + b->vertex_count > k_vert_slab_cap ||
        s_slab_tr_cursor + b->triangle_count > k_tri_slab_cap) {
        fprintf(stderr, "[Scene] Meshlet BDA slab full (ml=%u/%u, vx=%u/%u, tr=%u/%u)\n",
                s_slab_ml_cursor + b->meshlet_count, K_SCENE_MAX_MESHLETS,
                s_slab_vx_cursor + b->vertex_count, k_vert_slab_cap,
                s_slab_tr_cursor + b->triangle_count, k_tri_slab_cap);
        return -1;
    }

    uint32_t ml_base = s_slab_ml_cursor;
    uint32_t vx_base = s_slab_vx_cursor;
    uint32_t tr_base = s_slab_tr_cursor;

    SceneMeshlet *patched = malloc((size_t)b->meshlet_count * sizeof(*patched));
    if (patched == nullptr) {
        return -1;
    }
    for (uint32_t i = 0u; i < b->meshlet_count; ++i) {
        patched[i] = b->meshlets[i];
        patched[i].vert_offset += vx_base;
        patched[i].tri_offset  += tr_base;
        patched[i].mesh_id      = mesh_id;
    }

    int rc = 0;
    if (upload_slab_bytes(k_slab_off_meshlets + (VkDeviceSize)ml_base * sizeof(SceneMeshlet),
                          patched, (VkDeviceSize)b->meshlet_count * sizeof(SceneMeshlet)) != 0 ||
        upload_slab_bytes(k_slab_off_vertices + (VkDeviceSize)vx_base * sizeof(ScenePackedVertex),
                          b->vertices, (VkDeviceSize)b->vertex_count * sizeof(ScenePackedVertex)) != 0 ||
        upload_slab_bytes(k_slab_off_triangles + (VkDeviceSize)tr_base * sizeof(uint32_t),
                          b->triangles, (VkDeviceSize)b->triangle_count * sizeof(uint32_t)) != 0) {
        rc = -1;
    }
    free(patched);
    if (rc != 0) {
        return -1;
    }

    s_slab_ml_cursor += b->meshlet_count;
    s_slab_vx_cursor += b->vertex_count;
    s_slab_tr_cursor += b->triangle_count;

    s_meshes[mesh_id] = (SceneMeshRecord){
        .used          = true,
        .first_meshlet = ml_base,
        .meshlet_count = b->meshlet_count,
        .vert_offset   = vx_base,
        .vert_count    = b->vertex_count,
        .tri_offset    = tr_base,
        .tri_count     = b->triangle_count,
    };
    compute_mesh_proxy_meta(&s_meshes[mesh_id], b);
    return 0;
}

int scene_init(VulkanContext *vk, VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || depth_format == VK_FORMAT_UNDEFINED) {
        return -1;
    }
    if (!scene_self_check()) {
        fprintf(stderr, "[Scene Error] Startup mathematical self-check failed\n");
        return -1;
    }

    s_vk             = vk;
    s_pass_depth_fmt = depth_format;
    s_pass_samples   = samples == 0 ? VK_SAMPLE_COUNT_1_BIT : samples;
    s_can_hiz_push   = vk->device_info.supports_push_descriptors &&
                       vk->dispatch.vkCmdPushDescriptorSet != nullptr;

    // Select VisBuf deferred shading policy:
    // - ENGINE_VISBUF=1/on/true: enable 1x R32_UINT Visibility Buffer deferred shading
    // - ENGINE_VISBUF=0/off/false: use hardware MSAA indirect meshlet rasterization
    // - ENGINE_VISBUF=auto (default): enable VisBuf automatically at 1x MSAA, or MSAA indirect at >=2x
    s_visbuf_enabled = (s_pass_samples == VK_SAMPLE_COUNT_1_BIT) && s_can_hiz_push;
    const char *env_vb = getenv("ENGINE_VISBUF");
    if (env_vb != nullptr && env_vb[0] != '\0' && !env_contains_ci(env_vb, "auto")) {
        if (strcmp(env_vb, "1") == 0 || env_contains_ci(env_vb, "on") || env_contains_ci(env_vb, "true")) {
            s_visbuf_enabled = s_can_hiz_push;
        } else if (strcmp(env_vb, "0") == 0 || env_contains_ci(env_vb, "off") || env_contains_ci(env_vb, "false")) {
            s_visbuf_enabled = false;
        }
    }

    s_hiz_policy = SCENE_HIZ_AUTO;
    const char *env_hiz = getenv("ENGINE_HIZ");
    if (env_hiz != nullptr && env_hiz[0] != '\0' && !env_contains_ci(env_hiz, "auto")) {
        if (strcmp(env_hiz, "1") == 0 || env_contains_ci(env_hiz, "on") || env_contains_ci(env_hiz, "true")) {
            s_hiz_policy = SCENE_HIZ_ON;
        } else if (strcmp(env_hiz, "0") == 0 || env_contains_ci(env_hiz, "off") || env_contains_ci(env_hiz, "false")) {
            s_hiz_policy = SCENE_HIZ_OFF;
        }
    }

    s_ready               = true;
    s_gpu_ready           = false;
    s_used_hiz_this_frame = false;
    return 0;
}

static bool scene_ensure_gpu_ready(void) {
    if (s_gpu_ready) {
        return true;
    }
    if (!s_ready || s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    VulkanContext *vk = s_vk;

    if (da_buffer_create(vk, k_slab_total_bytes,
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         false, &s_geom_slab) != 0) {
        return false;
    }

    if (da_buffer_create(vk, (VkDeviceSize)k_cull_ring_slots * sizeof(SceneCullSlot),
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                         true, &s_cull_buf) != 0 || s_cull_buf.map == nullptr) {
        return false;
    }
    memset(s_cull_buf.map, 0, (size_t)k_cull_ring_slots * sizeof(SceneCullSlot));

    // Push-descriptor layout (ZERO VkDescriptorPool / ZERO VkDescriptorSet allocations!)
    if (s_can_hiz_push) {
        VkDescriptorSetLayoutBinding tex_binding = {
            .binding         = 0u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1u,
            .stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        };
        VkDescriptorSetLayoutCreateInfo dsl_info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
            .bindingCount = 1u,
            .pBindings    = &tex_binding,
        };
        if (vkCreateDescriptorSetLayout(vk->device, &dsl_info, nullptr, &s_tex_set_layout) != VK_SUCCESS) {
            s_tex_set_layout = VK_NULL_HANDLE;
            s_can_hiz_push   = false;
            s_visbuf_enabled = false;
        }
    }

    // 1. Two-Phase Meshlet Cull Compute Pipeline
    VkPushConstantRange cull_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(SceneCullPush),
    };
    VkPipelineLayoutCreateInfo cull_li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &cull_range,
    };
    if (vkCreatePipelineLayout(vk->device, &cull_li, nullptr, &s_cull_layout) != VK_SUCCESS) {
        return false;
    }
    s_cull_pipe = vulkan_compute_pipeline(vk, k_cull_comp_spv, sizeof k_cull_comp_spv, s_cull_layout);
    if (s_cull_pipe == VK_NULL_HANDLE) {
        return false;
    }

    // 2. Subgroup Hi-Z Pyramid Compute Pipeline
    if (s_can_hiz_push) {
        VkPushConstantRange hiz_range = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0u,
            .size       = sizeof(SceneHizPush),
        };
        VkPipelineLayoutCreateInfo hiz_li = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1u,
            .pSetLayouts            = &s_tex_set_layout,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &hiz_range,
        };
        if (vkCreatePipelineLayout(vk->device, &hiz_li, nullptr, &s_hiz_layout) != VK_SUCCESS) {
            return false;
        }
        s_hiz_pipe = vulkan_compute_pipeline(vk, k_hiz_comp_spv, sizeof k_hiz_comp_spv, s_hiz_layout);
        if (s_hiz_pipe == VK_NULL_HANDLE) {
            return false;
        }
    }

    // 3. Shared Draw Pipeline Layout (Depth-Only Prepass, 1x R32_UINT VisBuf, and MSAA Meshlet)
    VkPushConstantRange draw_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0u,
        .size       = sizeof(SceneDrawPush),
    };
    VkPipelineLayoutCreateInfo draw_li = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &draw_range,
    };
    if (vkCreatePipelineLayout(vk->device, &draw_li, nullptr, &s_draw_layout) != VK_SUCCESS) {
        return false;
    }

    VulkanGfxPipe depth_only_cfg = {
        .vert          = k_visbuf_vert_spv,
        .vert_bytes    = sizeof k_visbuf_vert_spv,
        .frag          = nullptr,
        .frag_bytes    = 0u,
        .layout        = s_draw_layout,
        .depth_format  = VK_FORMAT_D32_SFLOAT,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_depth_only_gfx, &depth_only_cfg, VK_FORMAT_UNDEFINED, nullptr);

    VulkanGfxPipe visbuf_cfg = {
        .vert          = k_visbuf_vert_spv,
        .vert_bytes    = sizeof k_visbuf_vert_spv,
        .frag          = k_visbuf_frag_spv,
        .frag_bytes    = sizeof k_visbuf_frag_spv,
        .layout        = s_draw_layout,
        .depth_format  = VK_FORMAT_D32_SFLOAT,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_visbuf_gfx, &visbuf_cfg, VK_FORMAT_R32_UINT, nullptr);

    VulkanGfxPipe meshlet_cfg = {
        .vert          = k_meshlet_vert_spv,
        .vert_bytes    = sizeof k_meshlet_vert_spv,
        .frag          = k_meshlet_frag_spv,
        .frag_bytes    = sizeof k_meshlet_frag_spv,
        .layout        = s_draw_layout,
        .depth_format  = s_pass_depth_fmt,
        .samples       = s_pass_samples,
        .cull          = VK_CULL_MODE_BACK_BIT,
        .depth_test    = VK_TRUE,
        .depth_write   = VK_TRUE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_meshlet_gfx, &meshlet_cfg, VK_FORMAT_B8G8R8A8_SRGB, nullptr);

    // 4. Fullscreen R32_UINT Visibility Buffer Analytical Resolve Pipeline
    if (s_can_hiz_push) {
        VkPipelineLayoutCreateInfo res_li = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1u,
            .pSetLayouts            = &s_tex_set_layout,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &draw_range,
        };
        if (vkCreatePipelineLayout(vk->device, &res_li, nullptr, &s_resolve_layout) != VK_SUCCESS) {
            return false;
        }
        VulkanGfxPipe resolve_cfg = {
            .vert          = k_resolve_vert_spv,
            .vert_bytes    = sizeof k_resolve_vert_spv,
            .frag          = k_resolve_frag_spv,
            .frag_bytes    = sizeof k_resolve_frag_spv,
            .layout        = s_resolve_layout,
            .depth_format  = s_pass_depth_fmt,
            .samples       = s_pass_samples,
            .cull          = VK_CULL_MODE_NONE,
            .depth_test    = VK_TRUE,
            .depth_write   = VK_TRUE,
            .depth_compare = VK_COMPARE_OP_LESS,
            .blend         = VK_FALSE,
        };
        vulkan_gfx_arm(&s_resolve_gfx, &resolve_cfg, VK_FORMAT_B8G8R8A8_SRGB, nullptr);
    }

    s_gpu_ready = true;
    const char *hiz_mode_str = !s_can_hiz_push ? "Frustum+Cone"
        : (s_hiz_policy == SCENE_HIZ_ON ? "Two-Phase (Forced)"
        : (s_hiz_policy == SCENE_HIZ_OFF ? "Frustum+Cone (ENGINE_HIZ=0)" : "Adaptive Two-Phase"));
    printf("[Scene] Unified 64v/124t quantized meshlet kernel armed (16B/vert, Subgroup Hi-Z=%s, VisBuf=%s, Y toggles).\n",
           hiz_mode_str,
           s_visbuf_enabled ? "ON (R32_UINT)" : "OFF (Hardware MSAA Indirect)");
    fflush(stdout);
    return true;
}

void scene_shutdown(VulkanContext *vk) {
    VulkanContext *dev_vk = vk != nullptr ? vk : s_vk;
    vulkan_gfx_disarm(dev_vk, &s_depth_only_gfx);
    vulkan_gfx_disarm(dev_vk, &s_visbuf_gfx);
    vulkan_gfx_disarm(dev_vk, &s_resolve_gfx);
    vulkan_gfx_disarm(dev_vk, &s_meshlet_gfx);

    if (dev_vk != nullptr && dev_vk->device != VK_NULL_HANDLE) {
        if (s_cull_pipe != VK_NULL_HANDLE) {
            vulkan_pipe_release(dev_vk, s_cull_pipe);
        }
        if (s_hiz_pipe != VK_NULL_HANDLE) {
            vulkan_pipe_release(dev_vk, s_hiz_pipe);
        }
        if (s_resolve_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev_vk->device, s_resolve_layout, nullptr);
        }
        if (s_draw_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev_vk->device, s_draw_layout, nullptr);
        }
        if (s_hiz_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev_vk->device, s_hiz_layout, nullptr);
        }
        if (s_cull_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev_vk->device, s_cull_layout, nullptr);
        }
        if (s_tex_set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev_vk->device, s_tex_set_layout, nullptr);
        }
    }

    destroy_target_image(dev_vk, &s_vis_depth);
    destroy_target_image(dev_vk, &s_vis_color);
    da_buffer_destroy(dev_vk, &s_hiz_buf);
    da_buffer_destroy(dev_vk, &s_cull_buf);
    da_buffer_destroy(dev_vk, &s_geom_slab);

    memset(s_meshes, 0, sizeof s_meshes);
    s_mesh_count       = 0u;
    s_slab_ml_cursor   = 0u;
    s_slab_vx_cursor   = 0u;
    s_slab_tr_cursor   = 0u;
    s_cull_pipe        = VK_NULL_HANDLE;
    s_hiz_pipe         = VK_NULL_HANDLE;
    s_resolve_layout   = VK_NULL_HANDLE;
    s_draw_layout      = VK_NULL_HANDLE;
    s_hiz_layout       = VK_NULL_HANDLE;
    s_cull_layout      = VK_NULL_HANDLE;
    s_tex_set_layout   = VK_NULL_HANDLE;
    s_gpu_ready        = false;
    s_ready            = false;
    s_vk               = nullptr;
}

uint32_t scene_register_mesh(const SceneCorner *corners, uint32_t corner_count,
                             float default_roughness) {
    if (!s_ready || s_mesh_count >= K_SCENE_MAX_MESHES || !scene_ensure_gpu_ready()) {
        return K_SCENE_INVALID_MESH;
    }
    SceneMeshletBuild build = {0};
    if (scene_build_meshlets(corners, corner_count, default_roughness, &build) != 0) {
        return K_SCENE_INVALID_MESH;
    }
    uint32_t id = s_mesh_count;
    if (upload_mesh_build(id, &build) != 0) {
        scene_meshlet_build_free(&build);
        return K_SCENE_INVALID_MESH;
    }
    s_mesh_count++;
    scene_meshlet_build_free(&build);
    return id;
}

int scene_update_mesh(uint32_t mesh_id, const SceneCorner *corners,
                      uint32_t corner_count, float default_roughness) {
    if (!s_ready || mesh_id >= s_mesh_count) {
        return -1;
    }
    SceneMeshletBuild build = {0};
    if (scene_build_meshlets(corners, corner_count, default_roughness, &build) != 0) {
        return -1;
    }

    // Reuse existing slice in-place after idling if the new mesh fits, or append safely.
    SceneMeshRecord *rec = &s_meshes[mesh_id];
    if (build.meshlet_count == 0u) {
        rec->meshlet_count = 0u;
        scene_meshlet_build_free(&build);
        scene_invalidate_hiz();
        return 0;
    }

    if (build.meshlet_count <= rec->meshlet_count &&
        build.vertex_count <= rec->vert_count &&
        build.triangle_count <= rec->tri_count) {
        (void)vkDeviceWaitIdle(s_vk->device);
        SceneMeshlet *patched = malloc((size_t)build.meshlet_count * sizeof(*patched));
        if (patched == nullptr) {
            scene_meshlet_build_free(&build);
            return -1;
        }
        for (uint32_t i = 0u; i < build.meshlet_count; ++i) {
            patched[i] = build.meshlets[i];
            patched[i].vert_offset += rec->vert_offset;
            patched[i].tri_offset  += rec->tri_offset;
            patched[i].mesh_id      = mesh_id;
        }
        int rc = 0;
        if (upload_slab_bytes(k_slab_off_meshlets + (VkDeviceSize)rec->first_meshlet * sizeof(SceneMeshlet),
                              patched, (VkDeviceSize)build.meshlet_count * sizeof(SceneMeshlet)) != 0 ||
            upload_slab_bytes(k_slab_off_vertices + (VkDeviceSize)rec->vert_offset * sizeof(ScenePackedVertex),
                              build.vertices, (VkDeviceSize)build.vertex_count * sizeof(ScenePackedVertex)) != 0 ||
            upload_slab_bytes(k_slab_off_triangles + (VkDeviceSize)rec->tri_offset * sizeof(uint32_t),
                              build.triangles, (VkDeviceSize)build.triangle_count * sizeof(uint32_t)) != 0) {
            rc = -1;
        }
        free(patched);
        if (rc == 0) {
            rec->meshlet_count = build.meshlet_count;
            compute_mesh_proxy_meta(rec, &build);
            scene_invalidate_hiz();
        }
        scene_meshlet_build_free(&build);
        return rc;
    }

    (void)vkDeviceWaitIdle(s_vk->device);
    int rc = upload_mesh_build(mesh_id, &build);
    scene_meshlet_build_free(&build);
    if (rc == 0) {
        scene_invalidate_hiz();
    }
    return rc;
}

void scene_invalidate_hiz(void) {
    s_hiz_hdr_cpu.valid = 0u;
    s_hiz_header_dirty  = true;
}

bool scene_visbuf_active(void) {
    return s_visbuf_enabled;
}

bool scene_toggle_visbuf(void) {
    if (!s_ready || !s_can_hiz_push) {
        return false;
    }
    s_visbuf_enabled = !s_visbuf_enabled;
    printf("[Scene] Visibility Buffer deferred shading: %s\n",
           s_visbuf_enabled ? "ON (1x R32_UINT + Analytical Barycentrics + Specular AA)"
                            : "OFF (Hardware MSAA Indirect Meshlets)");
    fflush(stdout);
    return true;
}

static SceneCullSlot *cull_slot_cpu(uint32_t slot) {
    return (SceneCullSlot *)s_cull_buf.map + (slot % k_cull_ring_slots);
}

static uint64_t cull_slot_bda(uint32_t slot) {
    return s_cull_buf.address + (uint64_t)(slot % k_cull_ring_slots) * sizeof(SceneCullSlot);
}

static VkDeviceSize cull_slot_buf_offset(uint32_t slot) {
    return s_cull_buf.offset + (VkDeviceSize)(slot % k_cull_ring_slots) * sizeof(SceneCullSlot);
}

void scene_begin_frame(uint32_t width, uint32_t height, const SceneViewParams *view) {
    (void)width;
    (void)height;
    s_prepared_this_frame = false;
    if (!s_ready || view == nullptr) {
        s_cur_instances  = 0u;
        s_cur_work_items = 0u;
        return;
    }
    s_frame_slot     = (uint32_t)(s_frame_counter % k_cull_ring_slots);
    s_cur_instances  = 0u;
    s_cur_work_items = 0u;
    s_cur_view       = *view;
}

void scene_submit_instance(uint32_t mesh_id, float origin_x, float origin_y, float origin_z,
                           float uniform_scale, float pitch_rad, float yaw_rad,
                           float squash, float roughness, float selected,
                           float depth_bias, uint32_t mat_index, uint32_t flags) {
    if (!s_ready || mesh_id >= s_mesh_count || !s_meshes[mesh_id].used ||
        s_meshes[mesh_id].meshlet_count == 0u || s_cur_instances >= K_SCENE_MAX_INSTANCES) {
        return;
    }
    const SceneMeshRecord *rec = &s_meshes[mesh_id];
    if (s_cur_work_items + rec->meshlet_count > k_max_draws) {
        return;
    }

    SceneCullSlot *slot = cull_slot_cpu(s_frame_slot);
    uint32_t inst_idx = s_cur_instances++;
    slot->instances[inst_idx] = (SceneInstance){
        .origin_x      = origin_x,
        .origin_y      = origin_y,
        .origin_z      = origin_z,
        .uniform_scale = uniform_scale,
        .pitch_rad     = pitch_rad,
        .yaw_rad       = yaw_rad,
        .squash        = squash,
        .roughness     = roughness,
        .selected      = selected,
        .depth_bias    = depth_bias,
        .mat_index     = mat_index,
        .mesh_id       = mesh_id,
        .first_meshlet = rec->first_meshlet,
        .meshlet_count = rec->meshlet_count,
        .flags         = flags,
        .pad0          = 0u,
    };

    for (uint32_t m = 0u; m < rec->meshlet_count; ++m) {
        slot->work_pairs[s_cur_work_items++] = (inst_idx << 16u) | (rec->first_meshlet + m);
    }

    // Submit 3D analytical occluder & emitter proxy to Core Engine Lighting (HRC + G-Buffer)
    {
        float cx = cosf(pitch_rad), sx = sinf(pitch_rad);
        float cy = cosf(yaw_rad),   sy = sinf(yaw_rad);
        float lx = rec->local_center[0];
        float ly = rec->local_center[1] * cx - rec->local_center[2] * sx;
        float lz = rec->local_center[1] * sx + rec->local_center[2] * cx;
        float wx =  lx * cy + lz * sy;
        float wy =  ly;
        float wz = -lx * sy + lz * cy;

        float sz  = fminf(fmaxf(1.0f + squash, 0.45f), 1.85f);
        float sxy = 1.0f / sqrtf(sz);

        float center[3] = {
            origin_x + wx * uniform_scale * sxy,
            origin_y + wy * uniform_scale * sxy,
            origin_z + wz * uniform_scale * sz,
        };
        float extents[3] = {
            rec->half_extents[0] * uniform_scale * sxy,
            rec->half_extents[1] * uniform_scale * sxy,
            rec->half_extents[2] * uniform_scale * sz,
        };

        float sel = fminf(fmaxf(selected, 0.0f), 1.0f) * 0.42f;
        float alb[3] = {
            rec->avg_albedo[0] + (1.0f - rec->avg_albedo[0]) * sel,
            rec->avg_albedo[1] + (1.0f - rec->avg_albedo[1]) * sel,
            rec->avg_albedo[2] + (1.0f - rec->avg_albedo[2]) * sel,
        };
        float emi[3] = {0};
        lighting_material_get_emissive(mat_index, emi);
        float rgh = roughness > 0.0f ? roughness : rec->avg_roughness;

        // When squash is active on an isotropic sphere, world-space Z squash is axis-aligned
        bool  iso_squash = fabsf(squash) > 1.0e-3f &&
                           fabsf(rec->half_extents[0] - rec->half_extents[2]) < 0.08f * rec->half_extents[0];
        float pr = iso_squash ? 0.0f : pitch_rad;
        float yr = iso_squash ? 0.0f : yaw_rad;

        LightingProxy proxy = {0};
        lighting_proxy_fill(&proxy, LIGHTING_PROXY_ELLIPSOID, center, extents,
                            pr, yr, rgh, alb, emi);
        lighting_submit_proxy(&proxy);
    }
}

static void cmd_push_sampled_image(VkCommandBuffer cmd, VkPipelineBindPoint bind_point,
                                   VkPipelineLayout layout, VkImageView view) {
    VkDescriptorImageInfo img_info = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = VK_NULL_HANDLE,
        .dstBinding      = 0u,
        .dstArrayElement = 0u,
        .descriptorCount = 1u,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo      = &img_info,
    };
    s_vk->dispatch.vkCmdPushDescriptorSet(cmd, bind_point, layout, 0u, 1u, &write);
}

static void emit_indirect_draws(VkCommandBuffer cmd, VkPipeline pipeline, uint32_t phase,
                                uint32_t width, uint32_t height) {
    const bool full_indirect = s_vk->device_info.supports_draw_indirect_count &&
                               s_vk->device_info.supports_multi_draw_indirect &&
                               s_vk->device_info.supports_draw_indirect_first_instance;

    SceneDrawPush push = {
        .meshlets_bda   = s_geom_slab.address + k_slab_off_meshlets,
        .vertices_bda   = s_geom_slab.address + k_slab_off_vertices,
        .triangles_bda  = s_geom_slab.address + k_slab_off_triangles,
        .cull_slot_bda  = cull_slot_bda(s_frame_slot),
        .width          = width,
        .height         = height,
        .phase_cmd_mode = full_indirect ? 0u : (phase == 0u ? 1u : 2u),
        .pad0           = 0u,
        .view           = s_cur_view,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vulkan_cmd_push(cmd, s_draw_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);

    VkDeviceSize base_off = cull_slot_buf_offset(s_frame_slot);
    if (full_indirect) {
        VkDeviceSize cmds_off  = base_off + (phase == 0u ? offsetof(SceneCullSlot, phase1_cmds)
                                                         : offsetof(SceneCullSlot, phase2_cmds));
        VkDeviceSize count_off = base_off + (phase == 0u ? offsetof(SceneCullSlot, phase1_draw_count)
                                                         : offsetof(SceneCullSlot, phase2_draw_count));
        vkCmdDrawIndirectCount(cmd, s_cull_buf.buffer, cmds_off,
                               s_cull_buf.buffer, count_off,
                               s_cur_work_items, sizeof(SceneDrawIndirectCmd));
    } else {
        VkDeviceSize single_off = base_off + (phase == 0u ? offsetof(SceneCullSlot, phase1_single_cmd)
                                                          : offsetof(SceneCullSlot, phase2_single_cmd));
        vkCmdDrawIndirect(cmd, s_cull_buf.buffer, single_off, 1u, sizeof(SceneDrawIndirectCmd));
    }
}

static void record_offscreen_pass(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                  uint32_t phase, bool clear_attachments) {
    VkPipeline pipe = s_visbuf_enabled
        ? vulkan_gfx_use(s_vk, &s_visbuf_gfx)
        : vulkan_gfx_use(s_vk, &s_depth_only_gfx);
    if (pipe == VK_NULL_HANDLE) {
        return;
    }

    VkRenderingAttachmentInfo color_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = s_vis_color.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = clear_attachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .uint32 = { 0u, 0u, 0u, 0u } } },
    };
    VkRenderingAttachmentInfo depth_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = s_vis_depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp      = clear_attachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .depthStencil = { .depth = 1.0f, .stencil = 0u } },
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { .offset = { 0, 0 }, .extent = { width, height } },
        .layerCount           = 1u,
        .colorAttachmentCount = s_visbuf_enabled ? 1u : 0u,
        .pColorAttachments    = s_visbuf_enabled ? &color_att : nullptr,
        .pDepthAttachment     = &depth_att,
    };

    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)width, .height = (float)height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D sc = { .offset = { 0, 0 }, .extent = { width, height } };
    vkCmdSetViewport(cmd, 0u, 1u, &vp);
    vkCmdSetScissor(cmd, 0u, 1u, &sc);
    emit_indirect_draws(cmd, pipe, phase, width, height);
    vkCmdEndRendering(cmd);
}

static void record_hiz_pyramid(VkCommandBuffer cmd, bool commit_prev) {
    if (!s_can_hiz_push || s_hiz_pipe == VK_NULL_HANDLE || s_hiz_hdr_cpu.mip_count == 0u) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_hiz_pipe);
    cmd_push_sampled_image(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_hiz_layout, s_vis_depth.view);

    // Pass 0: Subgroup 32x32 tile downsample across Mip 0..4
    SceneHizPush p0 = {
        .hiz_bda       = s_hiz_buf.address,
        .cull_slot_bda = cull_slot_bda(s_frame_slot),
        .qx            = s_cur_view.qx,
        .qy            = s_cur_view.qy,
        .qz            = s_cur_view.qz,
        .qw            = s_cur_view.qw,
        .cam_x         = s_cur_view.cam_x,
        .cam_y         = s_cur_view.cam_y,
        .cam_z         = s_cur_view.cam_z,
        .zoom          = s_cur_view.zoom,
        .mode          = 0u,
        .commit_prev   = commit_prev ? 1u : 0u,
    };
    vulkan_cmd_push(cmd, s_hiz_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof p0, &p0);
    uint32_t wg_x = (s_hiz_hdr_cpu.mips[0].width + 15u) / 16u;
    uint32_t wg_y = (s_hiz_hdr_cpu.mips[0].height + 15u) / 16u;
    vkCmdDispatch(cmd, wg_x, wg_y, 1u);

    VkMemoryBarrier2 mid_bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    VkDependencyInfo mid_dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &mid_bar,
    };
    vkCmdPipelineBarrier2(cmd, &mid_dep);

    // Pass 1: Coarse Mip 5..N reduction + optional SceneHizHeader camera commit
    if (s_hiz_hdr_cpu.mip_count > 5u || commit_prev) {
        SceneHizPush p1 = p0;
        p1.mode        = 1u;
        p1.commit_prev = commit_prev ? 1u : 0u;
        vulkan_cmd_push(cmd, s_hiz_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof p1, &p1);
        vkCmdDispatch(cmd, 1u, 1u, 1u);

        vkCmdPipelineBarrier2(cmd, &mid_dep);
    }
}

void scene_prepare_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    s_prepared_this_frame = false;
    s_used_hiz_this_frame = false;
    if (!s_ready || !s_gpu_ready || cmd == VK_NULL_HANDLE || width == 0u || height == 0u ||
        s_cur_work_items == 0u) {
        return;
    }

    const uint32_t hiz_auto_min =
        (s_vk->device_info.execution_profile == VULKAN_PROFILE_UMA_IGPU)
            ? k_hiz_auto_uma_min
            : k_hiz_auto_dgpu_min;
    bool want_hiz = false;
    if (s_can_hiz_push) {
        if (s_hiz_policy == SCENE_HIZ_ON) {
            want_hiz = true;
        } else if (s_hiz_policy == SCENE_HIZ_AUTO) {
            // Activate the Subgroup Hi-Z depth pyramid + Two-Phase disocclusion pass automatically
            // once active meshlet count amortizes full-screen Hi-Z pyramid bandwidth (>=128 on UMA, >=64 on dGPU).
            want_hiz = (s_cur_work_items >= hiz_auto_min);
        }
    }

    const bool need_offscreen = s_visbuf_enabled || want_hiz;
    const bool screen_ok      = !need_offscreen ||
                                ensure_screen_resources(width, height, s_visbuf_enabled, want_hiz) == 0;
    const bool use_hiz        = want_hiz && screen_ok;
    const bool use_visbuf     = s_visbuf_enabled && screen_ok;

    if (use_hiz && s_hiz_header_dirty) {
        vkCmdUpdateBuffer(cmd, s_hiz_buf.buffer, s_hiz_buf.offset,
                          sizeof(SceneHizHeader), &s_hiz_hdr_cpu);
        VkMemoryBarrier2 up_bar = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        };
        VkDependencyInfo up_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers    = &up_bar,
        };
        vkCmdPipelineBarrier2(cmd, &up_dep);
        s_hiz_header_dirty = false;
    }

    // --- Phase 1 Meshlet Cull (Normal Cone + Frustum + Optional Previous-Frame Reprojected Hi-Z) ---
    SceneCullPush cull_p1 = {
        .meshlets_bda  = s_geom_slab.address + k_slab_off_meshlets,
        .cull_slot_bda = cull_slot_bda(s_frame_slot),
        .hiz_bda       = use_hiz ? s_hiz_buf.address : 0ull,
        .qx            = s_cur_view.qx,
        .qy            = s_cur_view.qy,
        .qz            = s_cur_view.qz,
        .qw            = s_cur_view.qw,
        .cam_x         = s_cur_view.cam_x,
        .cam_y         = s_cur_view.cam_y,
        .cam_z         = s_cur_view.cam_z,
        .zoom          = s_cur_view.zoom,
        .width         = width,
        .height        = height,
        .work_count    = s_cur_work_items,
        .phase         = 0u,
        .enable_hiz    = use_hiz ? 1u : 0u,
        .pad0          = 0u,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cull_pipe);
    vulkan_cmd_push(cmd, s_cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof cull_p1, &cull_p1);
    vkCmdDispatch(cmd, 1u, 1u, 1u);

    VkMemoryBarrier2 cull1_bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    VkDependencyInfo cull1_dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &cull1_bar,
    };
    vkCmdPipelineBarrier2(cmd, &cull1_dep);

    if (!use_hiz && !use_visbuf) {
        s_prepared_this_frame = true;
        s_frame_counter++;
        return;
    }

    // --- Phase 1 Offscreen 1x Depth / R32_UINT VisBuf Rasterization ---
    VkImageMemoryBarrier2 pre_p1_imgs[2] = {0};
    uint32_t n_pre_p1 = 0u;
    pre_p1_imgs[n_pre_p1++] = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask       = 0u,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = s_vis_depth.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u },
    };
    if (use_visbuf) {
        pre_p1_imgs[n_pre_p1++] = (VkImageMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = 0u,
            .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = s_vis_color.image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
        };
    }
    VkDependencyInfo pre_p1_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = n_pre_p1,
        .pImageMemoryBarriers    = pre_p1_imgs,
    };
    vkCmdPipelineBarrier2(cmd, &pre_p1_dep);

    record_offscreen_pass(cmd, width, height, 0u, true);

    if (!use_hiz) {
        // Single-pass 1x R32_UINT VisBuf mode (ENGINE_VISBUF=1 ENGINE_HIZ=0)
        VkImageMemoryBarrier2 vis_to_read = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = s_vis_color.image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
        };
        VkDependencyInfo vis_read_dep = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers    = &vis_to_read,
        };
        vkCmdPipelineBarrier2(cmd, &vis_read_dep);
        s_prepared_this_frame = true;
        s_frame_counter++;
        return;
    }

    // --- Subgroup Hi-Z Pyramid Build from Phase-1 Depth ---
    VkImageMemoryBarrier2 d_to_read = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = s_vis_depth.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u },
    };
    VkDependencyInfo d_read_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1u,
        .pImageMemoryBarriers    = &d_to_read,
    };
    vkCmdPipelineBarrier2(cmd, &d_read_dep);

    record_hiz_pyramid(cmd, false);

    // --- Phase 2 Disocclusion Cull against Fresh Frame-N Hi-Z Pyramid ---
    SceneCullPush cull_p2 = cull_p1;
    cull_p2.phase = 1u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_cull_pipe);
    vulkan_cmd_push(cmd, s_cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof cull_p2, &cull_p2);
    vkCmdDispatch(cmd, 1u, 1u, 1u);
    vkCmdPipelineBarrier2(cmd, &cull1_dep);

    // --- Phase 2 Offscreen Rasterization (Disoccluded Meshlets) + Predicated Hi-Z Commit ---
    VkImageMemoryBarrier2 d_to_att = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = s_vis_depth.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u },
    };
    VkDependencyInfo d_att_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1u,
        .pImageMemoryBarriers    = &d_to_att,
    };
    vkCmdPipelineBarrier2(cmd, &d_att_dep);

    record_offscreen_pass(cmd, width, height, 1u, false);

    VkImageMemoryBarrier2 post_p2_imgs[2] = {0};
    uint32_t n_post_p2 = 0u;
    post_p2_imgs[n_post_p2++] = d_to_read;
    if (use_visbuf) {
        post_p2_imgs[n_post_p2++] = (VkImageMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = s_vis_color.image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
        };
    }
    VkDependencyInfo post_p2_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = n_post_p2,
        .pImageMemoryBarriers    = post_p2_imgs,
    };
    vkCmdPipelineBarrier2(cmd, &post_p2_dep);

    record_hiz_pyramid(cmd, true);
    lighting_set_scene_hiz(s_hiz_buf.address);

    s_used_hiz_this_frame = true;
    s_prepared_this_frame = true;
    s_frame_counter++;
}

void scene_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!s_ready || !s_prepared_this_frame || cmd == VK_NULL_HANDLE ||
        width == 0u || height == 0u || s_cur_work_items == 0u) {
        return;
    }

    if (s_visbuf_enabled && s_vis_color.view != VK_NULL_HANDLE) {
        VkPipeline resolve_pipe = vulkan_gfx_use(s_vk, &s_resolve_gfx);
        if (resolve_pipe == VK_NULL_HANDLE) {
            return;
        }
        SceneDrawPush push = {
            .meshlets_bda   = s_geom_slab.address + k_slab_off_meshlets,
            .vertices_bda   = s_geom_slab.address + k_slab_off_vertices,
            .triangles_bda  = s_geom_slab.address + k_slab_off_triangles,
            .cull_slot_bda  = cull_slot_bda(s_frame_slot),
            .width          = width,
            .height         = height,
            .phase_cmd_mode = 0u,
            .pad0           = 0u,
            .view           = s_cur_view,
        };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipe);
        cmd_push_sampled_image(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resolve_layout, s_vis_color.view);
        vulkan_cmd_push(cmd, s_resolve_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        sizeof push, &push);
        vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
        return;
    }

    VkPipeline meshlet_pipe = vulkan_gfx_use(s_vk, &s_meshlet_gfx);
    if (meshlet_pipe == VK_NULL_HANDLE) {
        return;
    }
    emit_indirect_draws(cmd, meshlet_pipe, 0u, width, height);
    if (s_used_hiz_this_frame) {
        emit_indirect_draws(cmd, meshlet_pipe, 1u, width, height);
    }
}

void scene_get_stats(SceneBenchStats *out_stats) {
    if (out_stats == nullptr) {
        return;
    }
    *out_stats = (SceneBenchStats){0};
    if (!s_ready) {
        return;
    }

    out_stats->registered_meshes = s_mesh_count;
    out_stats->slab_meshlets     = s_slab_ml_cursor;
    out_stats->slab_bytes        = (uint64_t)s_slab_ml_cursor * sizeof(SceneMeshlet) +
                                   (uint64_t)s_slab_vx_cursor * sizeof(ScenePackedVertex) +
                                   (uint64_t)s_slab_tr_cursor * sizeof(uint32_t);
    out_stats->hiz_bytes         = s_hiz_buf.bytes;
    out_stats->active_instances  = s_cur_instances;
    out_stats->hiz_mips          = s_hiz_hdr_cpu.mip_count;
    out_stats->visbuf_enabled    = s_visbuf_enabled;
    out_stats->hiz_enabled       = s_used_hiz_this_frame;

    if (s_cur_work_items > 0u && s_cull_buf.map != nullptr && s_frame_counter > 0u) {
        // Read a guaranteed-retired GPU telemetry slot with zero GPU stall.
        uint64_t retired_seq = s_frame_counter >= 3u ? (s_frame_counter - 3u) : 0u;
        const SceneCullSlot *ret = cull_slot_cpu((uint32_t)(retired_seq % k_cull_ring_slots));
        out_stats->active_meshlets  = ret->active_meshlets;
        out_stats->phase1_draws     = ret->phase1_draw_count;
        out_stats->phase2_draws     = ret->phase2_draw_count;
        out_stats->visible_meshlets = ret->phase1_draw_count + ret->phase2_draw_count;
        out_stats->cone_culled      = ret->cone_culled;
        out_stats->frustum_culled   = ret->frustum_culled;
        out_stats->hiz_culled       = ret->hiz_culled;
    }
}
