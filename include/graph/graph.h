#ifndef ENGINE_GRAPH_H
#define ENGINE_GRAPH_H

#include "vulkan/types.h"
#include <stdint.h>

// ============================================================================
// Engine v0.3.1: Zero-Allocation C23 Per-Pass Render Graph & GPU Pass Profiler
// - Fixed-capacity per-frame DAG (0 heap allocations, < 5 us CPU compile)
// - Automatic minimal Vulkan 1.4 Synchronization2 barrier batching (RAW/WAW/WAR)
// - Zero redundant barriers across consecutive read-only passes (RAR merge)
// - Automatic VK_KHR_unified_image_layouts & skipped-producer UNDEFINED safety
// - Double-buffered hardware VkQueryPool per-pass timestamps + debug labels
// ============================================================================

enum : uint32_t {
    RG_MAX_PASSES       = 24u,
    RG_MAX_RESOURCES    = 32u,
    RG_MAX_PASS_ACCESS  = 10u,
    RG_MAX_BARRIERS     = 16u,
    RG_INVALID_RES      = 0xFFFFFFFFu,
};

typedef uint32_t RgResHandle;

typedef enum : uint32_t {
    RG_RES_IMAGE  = 0u,
    RG_RES_BUFFER = 1u,
} RgResKind;

typedef enum : uint32_t {
    RG_PASS_COMPUTE = 0u,
    RG_PASS_SHADOW  = 1u,
    RG_PASS_GI      = 2u,
    RG_PASS_SCENE   = 3u,
    RG_PASS_POST    = 4u,
    RG_PASS_OVERLAY = 5u,
} RgPassType;

typedef enum : uint32_t {
    RG_USE_NONE             = 0u,
    RG_USE_COMPUTE_READ     = 1u,  // BDA / SSBO read in compute shader
    RG_USE_COMPUTE_WRITE    = 2u,  // BDA / SSBO / storage image write in compute
    RG_USE_COMPUTE_RW       = 3u,  // BDA / SSBO / storage image read+write in compute
    RG_USE_COMPUTE_SAMPLED  = 4u,  // Sampled image read in compute shader
    RG_USE_INDIRECT_READ    = 5u,  // Draw/dispatch indirect argument buffer read
    RG_USE_COLOR_WRITE      = 6u,  // Color attachment clear+write (discard prior)
    RG_USE_COLOR_RW         = 7u,  // Color attachment load+blend+write
    RG_USE_DEPTH_WRITE      = 8u,  // Depth attachment clear+write (discard prior)
    RG_USE_DEPTH_READ       = 9u,  // Depth attachment read-only test
    RG_USE_FRAG_SAMPLED     = 10u, // Sampled image read in fragment shader
    RG_USE_GRAPHICS_READ    = 11u, // BDA / vertex / fragment buffer read
    RG_USE_PRESENT          = 12u, // Present / foreign queue handoff boundary
} RgUsage;

typedef struct RgAccess {
    RgResHandle res;
    RgUsage     usage;
} RgAccess;

typedef void (*RgPassFn)(VkCommandBuffer cmd, void *user);

typedef struct RgResourceState {
    RgResKind             kind;
    const char           *name;
    VkImage               image;
    VkImageAspectFlags    aspect;
    uint32_t              mip_base;
    uint32_t              mip_count;
    VkImageLayout         layout;
    VkImageLayout        *external_layout; // Optional persistent layout synced back after execute
    bool                  unified_general;
    VkPipelineStageFlags2 write_stage;
    VkAccessFlags2        write_access;
    VkPipelineStageFlags2 read_stages;
    VkPipelineStageFlags2 visible_stages;
    bool                  dirty_write;
    bool                  has_readers;
} RgResourceState;

typedef struct RgPassDesc {
    const char *name;
    RgPassType  type;
    RgPassFn    execute;
    void       *user;
    RgAccess    accesses[RG_MAX_PASS_ACCESS];
    uint32_t    access_count;
    bool        enabled;
} RgPassDesc;

typedef struct RgGraph {
    RgResourceState resources[RG_MAX_RESOURCES];
    uint32_t        res_count;
    RgPassDesc      passes[RG_MAX_PASSES];
    uint32_t        pass_count;
    uint32_t        executed_count;
    uint32_t        recorded_ts;
    uint32_t        frame_slot;
    bool            slot_reset_done;
} RgGraph;

typedef struct RgBarrierBatch {
    VkImageMemoryBarrier2 image_barriers[RG_MAX_BARRIERS];
    uint32_t              image_count;
    VkMemoryBarrier2      mem_barrier;
    bool                  has_mem_barrier;
} RgBarrierBatch;

typedef struct RgPassTiming {
    char       name[24];
    RgPassType type;
    uint32_t   gpu_us;
} RgPassTiming;

typedef struct RgProfileSnapshot {
    RgPassTiming passes[RG_MAX_PASSES];
    uint32_t     pass_count;
    uint32_t     total_us;
    uint64_t     samples;
} RgProfileSnapshot;

// Lifecycle for per-pass hardware timestamp profiler & debug labels.
void rg_init(VulkanContext *vk);
void rg_shutdown(VulkanContext *vk);

// Pure C23 per-frame graph construction (zero heap allocations).
void rg_begin_frame(RgGraph *g, uint32_t frame_slot);

[[nodiscard]] RgResHandle rg_import_image(RgGraph *g,
                                          const char *name,
                                          VkImage image,
                                          VkImageAspectFlags aspect,
                                          uint32_t mip_base,
                                          uint32_t mip_count,
                                          VkImageLayout initial_layout,
                                          VkImageLayout *persistent_layout,
                                          bool unified_general);

[[nodiscard]] RgResHandle rg_import_buffer(RgGraph *g,
                                           const char *name,
                                           VkPipelineStageFlags2 initial_write_stage,
                                           VkAccessFlags2 initial_write_access);

// Declares prior external/cross-frame write and/or read stages on an imported resource
// so the graph emits exact Vulkan 1.4 Sync2 RAW/WAW/WAR barriers on first use.
void rg_set_prior_access(RgGraph *g,
                         RgResHandle res,
                         VkPipelineStageFlags2 write_stage,
                         VkAccessFlags2 write_access,
                         VkPipelineStageFlags2 read_stages);

void rg_add_pass(RgGraph *g,
                 const char *name,
                 RgPassType type,
                 const RgAccess *accesses,
                 uint32_t access_count,
                 RgPassFn execute,
                 void *user);

// Deterministic barrier planner for a single pass (also directly unit-tested).
void rg_plan_pass_barriers(RgGraph *g, uint32_t pass_index, RgBarrierBatch *out_batch);

// Executes any pending passes added since the last flush (batched Sync2 barriers + timestamps).
void rg_flush(RgGraph *g, VkCommandBuffer cmd);

// Brackets an externally recorded dynamic rendering pass (e.g. SceneHDR) in the same GPU profile.
void rg_begin_external_pass(RgGraph *g, VkCommandBuffer cmd, const char *name, RgPassType type);
void rg_end_external_pass(RgGraph *g, VkCommandBuffer cmd);

// Seals timestamp tracking for the frame and syncs persistent external image layouts.
void rg_end_frame(RgGraph *g);

// Latest non-blocking retired per-pass GPU timing snapshot.
void rg_get_profile(RgProfileSnapshot *out);

#endif /* ENGINE_GRAPH_H */
