#ifndef ENGINE_BACKEND_INTERNAL_H
#define ENGINE_BACKEND_INTERNAL_H

#include "backend/backend.h"
#include "vulkan/bda.h"
#include "vulkan/memory.h"
#include "vulkan/pipe.h"

enum : uint32_t {
    k_backend_cell_ring_cap = 16u,
};

// Total GPU BDA buffer size:
//   8,192 B (GpuMathArchiveHeader: 256B PULSE dir + 85*64B quadtree nodes + pad)
// + 32 * 24,576 B (32 resident 4KB-aligned tile slots = 786,432 B)
// = 794,624 bytes
enum : uint32_t {
    k_backend_bda_bytes = k_gmpack_header_bytes + k_gmpack_gpu_slot_cap * k_gmpack_tile_bytes,
};

typedef struct BackendState {
    VulkanContext        *vk;
    DaBuffer              bda_buf;
    GpuMathArchiveHeader *gpu_hdr;
    GpuMathTileBlock     *gpu_tiles;
    VkPipelineLayout      comp_layout;
    VkPipeline            comp_pipe;
    uint64_t              dirty_slots_mask;
    uint32_t              slot_to_node[k_gmpack_gpu_slot_cap];
    uint32_t              slot_lru_clock[k_gmpack_gpu_slot_cap];
    uint32_t              lru_tick;
    float                 last_focus_x;
    float                 last_focus_y;
    float                 last_zoom;
    bool                  initialized;
    bool                  have_focus;
    char                  pack_path[1024];
    alignas(64) GpuMathStreamCell cells[k_backend_cell_ring_cap];
} BackendState;

extern BackendState g_backend;

// math/gpu/directory.c
int  backend_gpu_directory_init(VulkanContext *vk);
void backend_gpu_directory_shutdown(VulkanContext *vk);
void backend_gpu_directory_sync(void);
void backend_gpu_record_filter(VkCommandBuffer cmd);

// terrain/storage/synth_besparmak.c
int  backend_synth_besparmak_archive(const char *pack_path);

#endif /* ENGINE_BACKEND_INTERNAL_H */
