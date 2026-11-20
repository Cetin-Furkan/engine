#pragma once

#include "scene/scene.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/memory.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdint.h>

enum : uint32_t {
    K_SCENE_VERT_SLAB_CAP      = 524288u,
    K_SCENE_TRI_SLAB_CAP       = 1048576u,
    K_SCENE_CULL_RING_SLOTS    = 4u,
    K_SCENE_MAX_DRAWS          = 16384u,
    K_SCENE_MAX_BLEND_DRAWS    = 4096u,
    K_SCENE_HIZ_AUTO_UMA_MIN   = 128u,
    K_SCENE_HIZ_AUTO_DGPU_MIN  = 64u,
    K_SCENE_MAX_PENDING_UPLOADS = 128u,
    K_SCENE_RETIRE_CAP         = 8u,
};

typedef enum SceneHizPolicy : uint8_t {
    SCENE_HIZ_OFF  = 0u,
    SCENE_HIZ_ON   = 1u,
    SCENE_HIZ_AUTO = 2u,
} SceneHizPolicy;

enum : size_t {
    K_SCENE_SLAB_OFF_MESHLETS  = 0u,
    K_SCENE_SLAB_OFF_VERTICES  = K_SCENE_MAX_MESHLETS * sizeof(SceneMeshlet),
    K_SCENE_SLAB_OFF_TRIANGLES = K_SCENE_SLAB_OFF_VERTICES +
                                 (size_t)K_SCENE_VERT_SLAB_CAP * sizeof(ScenePackedVertex),
    K_SCENE_SLAB_TOTAL_BYTES   = K_SCENE_SLAB_OFF_TRIANGLES +
                                 (size_t)K_SCENE_TRI_SLAB_CAP * sizeof(uint32_t),
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
    SceneDrawIndirectCmd phase1_cmds[K_SCENE_MAX_DRAWS];
    SceneDrawIndirectCmd phase2_cmds[K_SCENE_MAX_DRAWS];
    uint32_t             rejected_pairs[K_SCENE_MAX_DRAWS];
    uint32_t             work_pairs[K_SCENE_MAX_DRAWS];
    uint32_t             blend_pairs[K_SCENE_MAX_BLEND_DRAWS];
    SceneInstance        instances[K_SCENE_MAX_INSTANCES];
} SceneCullSlot;

static_assert(offsetof(SceneCullSlot, phase1_single_cmd) == 64u, "slot single1 offset");
static_assert(offsetof(SceneCullSlot, phase2_single_cmd) == 80u, "slot single2 offset");
static_assert(offsetof(SceneCullSlot, phase1_cmds) == 128u, "slot cmds1 offset");
static_assert(offsetof(SceneCullSlot, phase2_cmds) == 262272u, "slot cmds2 offset");
static_assert(offsetof(SceneCullSlot, rejected_pairs) == 524416u, "slot rejected offset");
static_assert(offsetof(SceneCullSlot, work_pairs) == 589952u, "slot work offset");
static_assert(offsetof(SceneCullSlot, blend_pairs) == 655488u, "slot blend offset");
static_assert(offsetof(SceneCullSlot, instances) == 671872u, "slot instances offset");

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
    float    persp;
} SceneCullPush;

typedef struct SceneHizPush {
    uint64_t hiz_bda;
    uint64_t cull_slot_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    uint32_t mode;
    uint32_t commit_prev;
    float    persp;
    uint32_t pad1;
} SceneHizPush;

typedef struct SceneShadowPush {
    uint64_t meshlets_bda;
    uint64_t vertices_bda;
    uint64_t triangles_bda;
    uint64_t cull_slot_bda;
    float    light_vp[16];
} SceneShadowPush;

typedef struct SceneDrawPush {
    uint64_t        meshlets_bda;
    uint64_t        vertices_bda;
    uint64_t        triangles_bda;
    uint64_t        cull_slot_bda;
    uint64_t        pbr_mats_bda;
    uint32_t        width;
    uint32_t        height;
    uint32_t        phase_cmd_mode;
    uint32_t        draw_flags;
    SceneViewParams view;
} SceneDrawPush;

static_assert(sizeof(SceneShadowPush) == 96u, "SceneShadowPush size");
static_assert(sizeof(SceneCullPush) == 80u, "SceneCullPush size");
static_assert(sizeof(SceneHizPush) == 64u, "SceneHizPush size");
static_assert(offsetof(SceneDrawPush, view) == 56u, "SceneDrawPush view offset");
static_assert(sizeof(SceneDrawPush) == 104u, "SceneDrawPush size");

typedef struct SceneMeshRecord {
    bool     used;
    uint32_t first_meshlet;
    uint32_t meshlet_count;
    uint32_t vert_offset;
    uint32_t vert_count;
    uint32_t tri_offset;
    uint32_t tri_count;
    uint64_t last_used_point;
    float    local_center[3];
    float    half_extents[3];
    float    avg_albedo[3];
    float    avg_roughness;
} SceneMeshRecord;

typedef struct ScenePendingUpload {
    DaBuffer     stage;
    VkDeviceSize dst_off;
    VkDeviceSize bytes;
    uint64_t     signal_point;
    bool         recorded;
    bool         in_use;
} ScenePendingUpload;

typedef struct SceneTargetImage {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint32_t       width;
    uint32_t       height;
} SceneTargetImage;

typedef struct SceneRetiredRes {
    SceneTargetImage img;
    DaBuffer         buf;
    uint64_t         retire_point;
    bool             active;
} SceneRetiredRes;

typedef struct SceneBlendEntry {
    uint32_t inst_idx;
    uint32_t first_ml;
    uint32_t ml_count;
    float    view_z;
} SceneBlendEntry;

typedef struct SceneGpuState {
    VulkanContext        *vk;
    VkFormat              pass_color_fmt;
    VkFormat              pass_depth_fmt;
    VkSampleCountFlagBits pass_samples;
    bool                  ready;
    bool                  gpu_ready;
    bool                  visbuf_enabled;
    bool                  can_hiz_push;
    SceneHizPolicy        hiz_policy;
    bool                  used_hiz_this_frame;

    DaBuffer              geom_slab;
    DaBuffer              cull_buf;
    DaBuffer              hiz_buf;
    SceneRetiredRes       retired_res[K_SCENE_RETIRE_CAP];
    uint64_t              screen_last_pt;

    SceneMeshRecord       meshes[K_SCENE_MAX_MESHES];
    uint32_t              mesh_count;
    uint32_t              slab_ml_cursor;
    uint32_t              slab_vx_cursor;
    uint32_t              slab_tr_cursor;

    SceneTargetImage      vis_depth;
    SceneTargetImage      vis_color;
    SceneHizHeader        hiz_hdr_cpu;
    bool                  hiz_header_dirty;

    VkDescriptorSetLayout tex_set_layout;
    VkDescriptorSetLayout resolve_set_layout;
    VkPipelineLayout      cull_layout;
    VkPipeline            cull_pipe;
    VkPipelineLayout      hiz_layout;
    VkPipeline            hiz_pipe;
    VkPipelineLayout      draw_layout;
    VkPipelineLayout      meshlet_layout;
    VkPipelineLayout      shadow_layout;
    VulkanGfxLive         depth_only_gfx;
    VulkanGfxLive         visbuf_gfx;
    VkPipelineLayout      resolve_layout;
    VulkanGfxLive         resolve_gfx;
    VulkanGfxLive         meshlet_gfx;
    VulkanGfxLive         meshlet_blend_gfx;
    VulkanGfxLive         shadow_gfx;

    uint32_t              frame_slot;
    uint64_t              frame_counter;
    uint32_t              cur_instances;
    uint32_t              cur_work_items;
    uint32_t              cur_blend_items;
    SceneBlendEntry       blend_entries[K_SCENE_MAX_INSTANCES];
    uint32_t              blend_entry_cnt;
    bool                  any_two_sided;
    float                 inst_bounds[K_SCENE_MAX_INSTANCES][4];
    SceneViewParams       cur_view;
    bool                  prepared_this_frame;
    ScenePendingUpload    pending_uploads[K_SCENE_MAX_PENDING_UPLOADS];
    VkSemaphore           upload_sem;
    uint64_t              cur_signal_point;
} SceneGpuState;

extern SceneGpuState g_scene;

static inline SceneCullSlot *scene_cull_slot_cpu(uint32_t slot) {
    return (SceneCullSlot *)g_scene.cull_buf.map + (slot % K_SCENE_CULL_RING_SLOTS);
}

static inline uint64_t scene_cull_slot_bda(uint32_t slot) {
    return g_scene.cull_buf.address +
           (uint64_t)(slot % K_SCENE_CULL_RING_SLOTS) * sizeof(SceneCullSlot);
}

static inline VkDeviceSize scene_cull_slot_buf_offset(uint32_t slot) {
    return g_scene.cull_buf.offset +
           (VkDeviceSize)(slot % K_SCENE_CULL_RING_SLOTS) * sizeof(SceneCullSlot);
}

// pipelines.c
bool scene_ensure_gpu_ready(void);

// targets.c
void scene_targets_reap(VulkanContext *vk, bool force_all);
void scene_targets_destroy(VulkanContext *vk);
int  scene_targets_ensure(uint32_t width, uint32_t height,
                          bool need_vis_color, bool need_hiz_buf);

// slab.c
bool scene_point_retired(uint64_t point);
void scene_uploads_reap(VulkanContext *dev_vk, bool force_all);
void scene_uploads_flush(VkCommandBuffer cmd);
