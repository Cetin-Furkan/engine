#ifndef ENGINE_BACKEND_H
#define ENGINE_BACKEND_H

#include "backend/math/relptr2d.h"
#include "backend/math/cell64.h"
#include "backend/terrain/pack.h"
#include "backend/terrain/stream.h"
#include "backend/terrain/survey.h"
#include "ring/ring.h"
#include "vulkan/core.h"

// ============================================================================
// GPU-Math & Dynamic Map/Terrain Streaming Backend (src/backend/)
// ============================================================================

[[nodiscard]] int  backend_init(VulkanContext *vk);
void               backend_shutdown(VulkanContext *vk);

// Updates the active zoom-driven quadtree cut around (focus_x, focus_y, zoom),
// dynamically streams high-LOD tiles from the SSD (.gmpack) via 64B Stateful
// Function Cells and RelPtr2D offsets, and returns true if the GPU BDA page
// directory or resident tile pool changed.
bool               backend_update_camera_zoom(float focus_x, float focus_y, float zoom);

// Records the Slang 2026 GPU-Math compute passes:
// 1. terrain_filter.comp.spv for any newly streamed SSD tiles
// 2. terrain_cull.comp.spv for GPU-driven frustum + elevation AABB culling into BDA indirect draw commands
void               backend_record_compute(VkCommandBuffer cmd, const TerrainCullParams *cull);

// Returns the Vulkan 1.4 Buffer Device Address (BDA) of the 256-byte PULSE
// Active GPU Window (GpuMathPageDirectory) at the base of the terrain arena.
[[nodiscard]] uint64_t backend_directory_bda(void);

// Returns the Vulkan 1.4 Buffer Device Address (BDA) and VkBuffer handle of the
// GPU-driven indirect draw + visible-leaf compaction buffer (GpuMathIndirectBuffer).
[[nodiscard]] uint64_t backend_indirect_bda(void);
[[nodiscard]] VkBuffer backend_indirect_buffer(VkDeviceSize *out_base_offset);

// Returns the number of active quadtree leaf tiles in the current zoom cut,
// and the number of frustum-visible leaf tiles after culling.
[[nodiscard]] uint32_t backend_active_leaf_count(void);
[[nodiscard]] uint32_t backend_visible_leaf_count(void);

// Samples exact measured elevation (meters), world Z, slope, and WGS84 lat/lon
// at world coordinates (wx, wy), intersects a 3D camera click ray with the
// Baklan & Beşparmak Dağı heightfield, or evaluates a two-point geodesic elevation profile.
bool               backend_sample_world_xy(float wx, float wy, TerrainSampleInfo *out);
float              backend_sample_elevation_world_z(float wx, float wy);
bool               backend_ray_pick_terrain(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            TerrainSampleInfo *out);
bool               backend_compute_terrain_profile(float ax, float ay,
                                                   float bx, float by,
                                                   TerrainProfileInfo *out);

// Saves any live state or reloads the .gmpack archive and survey manifest from SSD on demand (L key).
[[nodiscard]] bool backend_reload_pack(void);

// Harvests an asynchronous io_uring tile-read CQE submitted by backend_update_camera_zoom.
// Returns true when a newly resident .epack tile flipped ready in GPU BDA memory.
[[nodiscard]] bool backend_on_cqe(const struct io_uring_cqe *cqe);

// Returns the resolved path of the active photogrammetric/geodetic survey manifest,
// and retrieves the most recent multi-view stereo & survey fusion statistics.
const char        *backend_survey_manifest_path(void);
bool               backend_survey_get_stats(TerrainSurveyStats *out_stats);

// Interactive Bird's-Eye Orthophoto Placement & Upload API:
// - Starts at 50% transparency (opacity = 0.50); 'O' increases transparency, 'P' decreases transparency
// - Moves parallel to the map plane via 'W', 'A', 'S', 'D'
// - Stretches in 2 dimensions (X width and Y height) in exact 0.5% increments
// - Pressing 'Enter' uploads/bakes the bird's-eye image directly into all overlapping terrain tiles
void               backend_overlay_set_custom_path(const char *image_path);
bool               backend_overlay_move(float dx_m, float dy_m, bool log_state);
bool               backend_overlay_adjust_transparency(float delta_transparency);
bool               backend_overlay_stretch_2d(int step_x, int step_y);
bool               backend_overlay_commit_upload(void);
bool               backend_overlay_get_center(float *out_wx, float *out_wy);

typedef struct BackendBenchStats {
    uint64_t bda_arena_bytes;
    uint64_t indirect_bytes;
    uint64_t overlay_arena_bytes;
    uint64_t fused_cache_bytes;
    uint64_t stream_uring_us_total;
    uint64_t stream_boot_uring_us;
    uint32_t slot_capacity;
    uint32_t resident_tiles;
    uint32_t active_leaves;
    uint32_t visible_leaves;
    uint32_t stream_loads_total;
    uint32_t stream_uring_turns;
    uint32_t stream_async_cqes;
    uint32_t stream_fixed_buf_reads;
    bool     initialized;
    bool     zero_copy_uma;
} BackendBenchStats;

void               backend_get_bench_stats(BackendBenchStats *out);

#endif /* ENGINE_BACKEND_H */
