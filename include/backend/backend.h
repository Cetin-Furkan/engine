#ifndef ENGINE_BACKEND_H
#define ENGINE_BACKEND_H

#include "backend/math/relptr2d.h"
#include "backend/math/cell64.h"
#include "backend/terrain/pack.h"
#include "backend/terrain/stream.h"
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

// Records the Slang 2026 GPU-Math compute pass (terrain_filter.comp.spv) for
// any newly streamed SSD tiles to compute bicubic surface gradients & AO.
void               backend_record_compute(VkCommandBuffer cmd);

// Returns the Vulkan 1.4 Buffer Device Address (BDA) of the 256-byte PULSE
// Active GPU Window (GpuMathPageDirectory) at the base of the terrain arena.
[[nodiscard]] uint64_t backend_directory_bda(void);

// Returns the number of active quadtree leaf tiles in the current zoom cut.
[[nodiscard]] uint32_t backend_active_leaf_count(void);

// Samples exact measured elevation (meters), world Z, slope, and WGS84 lat/lon
// at world coordinates (wx, wy), or intersects a 3D camera click ray with the
// Baklan & Beşparmak Dağı heightfield.
bool               backend_sample_world_xy(float wx, float wy, TerrainSampleInfo *out);
float              backend_sample_elevation_world_z(float wx, float wy);
bool               backend_ray_pick_terrain(float ox, float oy, float oz,
                                            float dx, float dy, float dz,
                                            TerrainSampleInfo *out);

// Saves any live state or reloads the .gmpack archive from SSD on demand (L key).
[[nodiscard]] bool backend_reload_pack(void);

#endif /* ENGINE_BACKEND_H */
