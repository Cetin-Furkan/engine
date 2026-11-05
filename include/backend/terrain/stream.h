#ifndef ENGINE_BACKEND_TERRAIN_STREAM_H
#define ENGINE_BACKEND_TERRAIN_STREAM_H

#include "backend/math/cell64.h"
#include "backend/terrain/pack.h"
#include <stdint.h>

typedef struct TerrainSampleInfo {
    double   latitude_deg;
    double   longitude_deg;
    float    world_x;
    float    world_y;
    float    world_z;
    float    elevation_m;
    float    slope_deg;
    uint32_t node_index;
    uint8_t  lod;
    uint8_t  tile_gx;
    uint8_t  tile_gy;
} TerrainSampleInfo;

// Evaluates the camera zoom and focus (world_x, world_y, zoom) against the
// 85-node quadtree, updates active_leaves[], and returns true if new SSD tiles
// were paged in or the active LOD cut changed.
bool backend_stream_update_lod(GpuMathArchiveHeader *hdr,
                               float                 focus_x,
                               float                 focus_y,
                               float                 zoom,
                               uint32_t             *out_needed_nodes,
                               uint32_t             *out_needed_count);

// Executes pending 64-byte Stateful Function Cells to stream needed quadtree
// tiles from the SSD .gmpack file into the host-mapped GPU BDA resident pool.
int backend_stream_page_ssd_tiles(const char           *pack_path,
                                  GpuMathArchiveHeader *hdr,
                                  GpuMathTileBlock     *gpu_tile_pool,
                                  const uint32_t       *needed_nodes,
                                  uint32_t              needed_count,
                                  uint64_t             *out_dirty_slots_mask);

#endif /* ENGINE_BACKEND_TERRAIN_STREAM_H */
