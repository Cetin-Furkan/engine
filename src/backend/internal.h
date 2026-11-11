#ifndef ENGINE_BACKEND_INTERNAL_H
#define ENGINE_BACKEND_INTERNAL_H

#include "backend/backend.h"
#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include "vulkan/pipe.h"

enum : uint32_t {
    k_backend_cell_ring_cap       = 16u,
    k_backend_overlay_max_dim     = 2048u,
    k_backend_overlay_live_bytes  = k_backend_overlay_max_dim * k_backend_overlay_max_dim * 4u,
    k_backend_overlay_arena_bytes = 48u * 1024u * 1024u,
    k_backend_overlay_bda_bytes   = k_backend_overlay_live_bytes + k_backend_overlay_arena_bytes,
};

// Total GPU BDA buffer size:
//   8,192 B (GpuMathArchiveHeader: 256B PULSE dir + 85*64B quadtree nodes + pad)
// + 32 * 24,576 B (32 resident 4KB-aligned tile slots = 786,432 B)
// = 794,624 bytes
enum : uint32_t {
    k_backend_bda_bytes = k_gmpack_header_bytes + k_gmpack_gpu_slot_cap * k_gmpack_tile_bytes,
};

typedef struct BackendState {
    VulkanContext         *vk;
    DaBuffer               bda_buf;
    DaBuffer               indirect_buf;
    DaBuffer               overlay_buf;
    GpuMathArchiveHeader  *gpu_hdr;
    GpuMathTileBlock      *gpu_tiles;
    GpuMathIndirectBuffer *indirect_hdr;
    uint32_t              *overlay_gpu_pixels;
    uint32_t              *overlay_cpu_pixels;
    uint32_t               overlay_w;
    uint32_t               overlay_h;
    float                  overlay_center_x;
    float                  overlay_center_y;
    float                  overlay_base_half_x;
    float                  overlay_base_half_y;
    float                  overlay_scale_x;
    float                  overlay_scale_y;
    float                  overlay_opacity;
    uint32_t               overlay_committed_cnt;
    bool                   overlay_active;
    bool                   overlay_preview_hidden;
    bool                   suppress_manifest_reload;
    char                   overlay_image_path[1024];
    VkPipelineLayout       comp_layout;
    VkPipeline             comp_pipe;
    VkPipelineLayout       cull_layout;
    VkPipeline             cull_pipe;
    uint64_t               dirty_slots_mask;
    uint32_t               dirty_slot_count;
    uint8_t                slot_dirty[k_gmpack_gpu_slot_cap];
    uint32_t               slot_to_node[k_gmpack_gpu_slot_cap];
    uint32_t               slot_lru_clock[k_gmpack_gpu_slot_cap];
    uint32_t               lru_tick;
    float                  last_focus_x;
    float                  last_focus_y;
    float                  last_zoom;
    bool                   initialized;
    bool                   have_focus;
    int                    pack_fd;
    uint8_t                node_fuse_state[k_gmpack_node_count];
    GpuMathTileBlock      *fused_cache[k_gmpack_node_count];
    char                   pack_path[1024];
    char                   survey_manifest_path[1024];
    TerrainSurveyDataset   survey;
    alignas(64) GpuMathStreamCell cells[k_backend_cell_ring_cap];
} BackendState;

extern BackendState g_backend;

// math/gpu/directory.c
int  backend_gpu_directory_init(VulkanContext *vk);
void backend_gpu_directory_shutdown(VulkanContext *vk);
void backend_gpu_directory_sync(void);
void backend_gpu_record_filter(VkCommandBuffer cmd);
void backend_gpu_record_cull(VkCommandBuffer cmd, const TerrainCullParams *cull);

// terrain/storage/synth_besparmak.c
float backend_eval_besparmak_elevation_m(double lat, double lon);
int   backend_synth_besparmak_archive(const char *pack_path);

#endif /* ENGINE_BACKEND_INTERNAL_H */

