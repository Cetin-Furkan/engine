#ifndef ENGINE_BACKEND_TERRAIN_PACK_H
#define ENGINE_BACKEND_TERRAIN_PACK_H

#include "backend/math/relptr2d.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Baklan & Beşparmak Dağı (Denizli, Türkiye) Quadtree Map Archive (.gmpack)
// ============================================================================
// Root Web Mercator Tile: z=11, x=1192, y=790
// Bounding Box:
//   Latitude:  37.857507° N .. 37.996163° N
//   Longitude: 29.531250° E .. 29.707031° E
//   Elevation: 824.0 m (Baklan Plain) .. 1,612.0 m (Beşparmak Dağı Summit)
//
// Restricted 5-Level Focal Quadtree Pyramid (256x256 samples per tile, 1.0 world unit = 1.0 meter):
//   LOD 0 (z=12):  1 x 1 =  1 tile  (node    0, 24.0 km x 24.0 km overview)
//   LOD 1 (z=13):  2 x 2 =  4 tiles (nodes   1..4,  12.0 km tiles)
//   LOD 2 (z=13):  4 x 4 = 16 tiles (nodes   5..20,  6.0 km tiles)
//   LOD 3 (z=14):  8 x 8 = 64 tiles (nodes  21..84,  3.0 km tiles)
//   LOD 4 (z=15/16): 8 x 8 = 64 ultra-HD core tiles (nodes 85..148, 1.5 km tiles over Baklan & Beşparmak Dağı)
//   Total Quadtree Nodes = 149 tiles

enum : uint32_t {
    k_gmpack_magic          = 0x32504D47u, // "GMP2"
    k_gmpack_version        = 4u,
    k_gmpack_lod_count      = 5u,
    k_gmpack_node_count     = 149u,        // 1 + 4 + 16 + 64 + 64
    k_gmpack_tile_dim       = 256u,        // 256x256 samples per tile (65,536 posts/pixels)
    k_gmpack_tile_samples   = 256u * 256u, // 65,536 samples
    k_gmpack_height_bytes   = 65536u * 2u, // 131,072 B (32 x 4KB blocks of u16 elevation)
    k_gmpack_imagery_bytes  = 65536u * 4u, // 262,144 B (64 x 4KB blocks of RGBA8 imagery)
    k_gmpack_tile_bytes     = 393216u,     // 96 x 4KB blocks per tile (384 KiB)
    k_gmpack_tile_blocks_4k = 96u,
    k_gmpack_header_blocks  = 3u,          // 3 x 4KB blocks = 12,288 B
    k_gmpack_header_bytes   = 12288u,      // 3 x 4KB blocks (256B dir + 149*64B nodes + pad)
    k_gmpack_total_bytes    = 12288u + 149u * 393216u, // 58,601,472 B (55.89 MiB)
    k_gmpack_gpu_slot_cap   = 64u,         // Max simultaneously resident tiles in GPU BDA pool
    k_gmpack_active_cut_cap = 64u,         // Max leaf tiles in the active zoom LOD cut
};

static constexpr double k_besparmak_lat_min  = 37.857507156252030;
static constexpr double k_besparmak_lat_max  = 38.065392351332490;
static constexpr double k_besparmak_lon_min  = 29.487304687500000;
static constexpr double k_besparmak_lon_max  = 29.750976562500000;
static constexpr double k_besparmak_peak_lat = 37.960556000000000;
static constexpr double k_besparmak_peak_lon = 29.636667000000000;
static constexpr double k_baklan_center_lat  = 37.976748000000000;
static constexpr double k_baklan_center_lon  = 29.606909000000000;
static constexpr float  k_besparmak_elev_min = 810.0f;
static constexpr float  k_besparmak_elev_max = 1691.0f;
static constexpr float  k_terrain_world_half = 12000.0f; // 1.0 world unit = 1.0 real meter (24,000 m x 24,000 m)

// 64-Byte Quadtree Node Descriptor (1 L1 cache line per node)
typedef struct GpuMathQuadNode {
    alignas(64) RelPtr2D rel_ssd_payload; // Bytes 0..7:   Self-relative 2D pointer to SSD tile block
    RelPtr2D          rel_gpu_payload; // Bytes 8..15:  Self-relative 2D pointer to resident GPU BDA block
    RelPtr2D          rel_children;    // Bytes 16..23: Self-relative pointer to first of 4 child nodes
    GpuMathTileHeader header;          // Bytes 24..31: Lilliput 64-bit unified block/Morton/elevation header
    float             world_min_x;     // Bytes 32..35: World-space西/left X bound
    float             world_min_y;     // Bytes 36..39: World-space南/bottom Y bound
    float             world_max_x;     // Bytes 40..43: World-space東/right X bound
    float             world_max_y;     // Bytes 44..47: World-space北/top Y bound
    uint16_t          node_index;      // Bytes 48..49: 0..84
    uint16_t          parent_index;    // Bytes 50..51: Parent node index (0 for root)
    uint16_t          gpu_slot;        // Bytes 52..53: Resident GPU slot (0..31, 0 = root overview)
    uint8_t           lod;             // Byte  54:     0..3
    uint8_t           grid_x;          // Byte  55:     0..(2^lod - 1)
    uint8_t           grid_y;          // Byte  56:     0..(2^lod - 1)
    uint8_t           resident;        // Byte  57:     1 when loaded into GPU BDA slot
    uint16_t          pad_tail;        // Bytes 58..59
    float             elev_mean_m;     // Bytes 60..63: Mean measured elevation (meters)
} GpuMathQuadNode;

static_assert(sizeof(GpuMathQuadNode) == 64u, "GpuMathQuadNode must be 64 bytes");

// 256-Byte PULSE Active GPU Window (at offset 0 of .gmpack and GPU BDA arena)
typedef struct GpuMathPageDirectory {
    alignas(256) uint32_t magic; // 0x32504D47 ("GMP2")
    uint32_t version;            // 2
    uint32_t node_count;         // 85
    uint32_t tile_dim;           // 64
    float    world_half_extent;  // 720.0f
    float    elev_min_m;         // 824.0f (Baklan Ovası floor)
    float    elev_max_m;         // 1612.0f (Beşparmak Dağı peak)
    float    vertical_scale;     // World Z units per meter of elevation
    double   lat_min_deg;        // 37.857507
    double   lat_max_deg;        // 37.996163
    double   lon_min_deg;        // 29.531250
    double   lon_max_deg;        // 29.707031
    RelPtr2D root_node_rel;      // Self-relative pointer to node 0
    RelPtr2D tile_pool_rel;      // Self-relative pointer to 4KB-aligned tile pool base
    uint32_t active_leaf_count;  // Number of active quadtree leaves in current zoom cut
    uint32_t resident_tile_count;// Number of tiles currently resident in GPU BDA
    uint32_t stream_loads_total; // Total SSD -> GPU tile loads completed
    uint32_t deepest_lod_active; // Highest LOD (0..3) currently active under camera zoom
    float    focus_world_x;      // Current camera focus X on terrain
    float    focus_world_y;      // Current camera focus Y on terrain
    float    focus_zoom;         // Current camera zoom factor
    uint32_t frame_epoch;        // Monotonic frame counter
    // Active leaf node indices (up to 64 uint16_t = 128 bytes, bytes 112..239)
    uint16_t active_leaves[k_gmpack_active_cut_cap];
    uint64_t reserved_tail[2];   // Bytes 240..255
} GpuMathPageDirectory;

static_assert(sizeof(GpuMathPageDirectory) == 256u, "GpuMathPageDirectory must be 256 bytes");

// 24,576-Byte (6 x 4KB) Tile Payload Block
typedef struct GpuMathTileBlock {
    // Elevation in 1/16th meter units: meters = height_u16[i] * 0.0625f
    alignas(4096) uint16_t height_u16[k_gmpack_tile_samples];
    // Satellite + Topographic imagery packed as 0xAABBGGRR (R in bits 0..7, G in 8..15, B in 16..23, slope/AO in 24..31)
    uint32_t imagery_rgba8[k_gmpack_tile_samples];
} GpuMathTileBlock;

static_assert(sizeof(GpuMathTileBlock) == k_gmpack_tile_bytes, "GpuMathTileBlock must be 24,576 bytes");

// On-disk & in-memory 12,288-byte Metadata Prefix (3 x 4KB blocks)
typedef struct GpuMathArchiveHeader {
    alignas(4096) GpuMathPageDirectory dir;          // Bytes 0..255
    GpuMathQuadNode      nodes[k_gmpack_node_count]; // Bytes 256..9791 (149 * 64B)
    uint8_t              pad_to_12k[k_gmpack_header_bytes - 256u - k_gmpack_node_count * 64u];
} GpuMathArchiveHeader;

static_assert(sizeof(GpuMathArchiveHeader) == k_gmpack_header_bytes, "Archive header must be 12,288 bytes");

[[nodiscard]] uint32_t backend_quadtree_level_base(uint8_t lod);
[[nodiscard]] uint32_t backend_quadtree_node_index(uint8_t lod, uint8_t gx, uint8_t gy);
[[nodiscard]] int      backend_terrain_ensure_pack(const char *pack_path);
[[nodiscard]] int      backend_terrain_read_header(const char *pack_path, GpuMathArchiveHeader *out_hdr);
[[nodiscard]] int      backend_terrain_read_tile(const char *pack_path, uint32_t node_index, GpuMathTileBlock *out_tile);

#endif /* ENGINE_BACKEND_TERRAIN_PACK_H */
