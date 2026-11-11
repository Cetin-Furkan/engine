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
// Complete 5-Level Quadtree Pyramid (256x256 samples per tile, 1.0 world unit = 1.0 meter):
//   LOD 0 (z=12):  1 x 1 =   1 tile  (node    0, 24.0 km x 24.0 km overview)
//   LOD 1 (z=13):  2 x 2 =   4 tiles (nodes   1..4,  12.0 km tiles)
//   LOD 2 (z=13):  4 x 4 =  16 tiles (nodes   5..20,  6.0 km tiles)
//   LOD 3 (z=14):  8 x 8 =  64 tiles (nodes  21..84,  3.0 km tiles)
//   LOD 4 (z=15/16): 16 x 16 = 256 tiles (nodes 85..340, 1.5 km tiles across entire 24x24 km map)
//   Total Quadtree Nodes = 341 tiles

enum : uint32_t {
    k_gmpack_magic          = 0x32504D47u, // "GMP2"
    k_gmpack_version        = 5u,
    k_gmpack_lod_count      = 5u,
    k_gmpack_node_count     = 341u,        // 1 + 4 + 16 + 64 + 256 (complete 5-level quadtree)
    k_gmpack_tile_dim       = 256u,        // 256x256 samples per tile (65,536 posts/pixels)
    k_gmpack_tile_samples   = 256u * 256u, // 65,536 samples
    k_gmpack_height_bytes   = 65536u * 2u, // 131,072 B (32 x 4KB blocks of u16 elevation)
    k_gmpack_imagery_bytes  = 65536u * 4u, // 262,144 B (64 x 4KB blocks of RGBA8 imagery)
    k_gmpack_tile_bytes     = 393216u,     // 96 x 4KB blocks per tile (384 KiB)
    k_gmpack_tile_blocks_4k = 96u,
    k_gmpack_header_blocks  = 6u,          // 6 x 4KB blocks = 24,576 B
    k_gmpack_header_bytes   = 24576u,      // 6 x 4KB blocks (256B dir + 341*64B nodes + 2496B pad)
    k_gmpack_total_bytes    = 24576u + 341u * 393216u, // 134,111,232 B (127.90 MiB)
    k_gmpack_gpu_slot_cap   = 341u,        // Direct 1:1 resident GPU BDA slot per quadtree node (zero eviction)
    k_gmpack_active_cut_cap = 64u,         // Max leaf tiles in the active zoom LOD cut

    // v0.2 Milestone 2: .epack v1 GPU-Decoded Compressed Terrain Archive (3.84x smaller)
    k_epack_magic           = 0x314B5045u, // "EPK1"
    k_epack_version         = 1u,
    k_epack_patch_grid      = 32u,         // 32x32 planar-residual patches of 8x8 posts
    k_epack_patch_count     = 32u * 32u,   // 1,024 patches (48 B each = 49,152 B)
    k_epack_color_grid      = 64u,         // 64x64 chroma-preserving BC1 blocks of 4x4 texels
    k_epack_color_count     = 64u * 64u,   // 4,096 blocks (8 B each = 32,768 B)
    k_epack_ao_dim          = 128u,        // 128x128 8-bit horizon AO grid (16,384 B)
    k_epack_ao_samples      = 128u * 128u,
    k_epack_tile_blocks_4k  = 25u,         // 25 x 4 KiB blocks = 102,400 B (100 KiB)
    k_epack_tile_bytes      = 102400u,     // 3.84x smaller than 393,216 B .gmpack tile
    k_epack_tile_words      = 25600u,      // 25,600 uint32_t words per tile
    k_epack_total_bytes     = 24576u + 341u * 102400u, // 34,942,976 B (33.32 MiB)
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

static_assert(sizeof(GpuMathTileBlock) == k_gmpack_tile_bytes, "GpuMathTileBlock must be 393,216 bytes");

// ============================================================================
// .epack v1 GPU-Decoded Compressed Tile Block (102,400 Bytes = 25 x 4 KiB)
// ============================================================================
// Decoded directly in Slang 2026 shaders via Vulkan 1.4 Buffer Device Address:
//   [0..49151]     48 KiB: 32x32 8x8 Planar + 5-Bit Residual Elevation Patches
//   [49152..53247]  4 KiB: Lossless Perimeter Seam Ring (u16 height + RGB565 border)
//   [53248..86015] 32 KiB: 64x64 4x4 Chroma-Preserving BC1-Style Albedo Blocks
//   [86016..102399] 16 KiB: 128x128 8-Bit Horizon Ambient Occlusion Grid

typedef struct EpackElevPatch {
    uint16_t base_u16;     // base_m  = float(base_u16) * 0.125f - 2048.0f
    int16_t  slope_x_i16;  // sx_m    = float(slope_x_i16) * (1.0f / 256.0f)
    int16_t  slope_y_i16;  // sy_m    = float(slope_y_i16) * (1.0f / 256.0f)
    uint16_t step_u16;     // step_m  = float(step_u16) * (1.0f / 1024.0f)
    uint32_t lo4[8];       // 8 rows of 8 x 4-bit low residual nibbles
    uint32_t hi1[2];       // 64 x 1-bit high residual MSB (q5 = lo4 | (hi1 << 4))
} EpackElevPatch;

static_assert(sizeof(EpackElevPatch) == 48u, "EpackElevPatch must be 48 bytes");

typedef struct EpackColorBlock {
    uint16_t c0_rgb565;    // Endpoint 0 (R5 in [4:0], G6 in [10:5], B5 in [15:11])
    uint16_t c1_rgb565;    // Endpoint 1
    uint32_t selectors;    // 16 x 2-bit interpolation weights {0, 1/3, 2/3, 1}
} EpackColorBlock;

static_assert(sizeof(EpackColorBlock) == 8u, "EpackColorBlock must be 8 bytes");

typedef struct EpackTileBlock {
    alignas(4096) EpackElevPatch elev_patches[k_epack_patch_count];   // Words 0..12287     (48 KiB)
    uint16_t                     border_height_u16[4][k_gmpack_tile_dim]; // Words 12288..12799 (2 KiB)
    uint16_t                     border_rgb565[4][k_gmpack_tile_dim];     // Words 12800..13311 (2 KiB)
    EpackColorBlock              color_blocks[k_epack_color_count];   // Words 13312..21503 (32 KiB)
    uint8_t                      ao_grid[k_epack_ao_samples];         // Words 21504..25599 (16 KiB)
} EpackTileBlock;

static_assert(sizeof(EpackTileBlock) == k_epack_tile_bytes, "EpackTileBlock must be 102,400 bytes");

// 64-Byte Interactive Bird's-Eye Orthophoto Placement Descriptor (at offset 22080 of GpuMathArchiveHeader)
enum : uint32_t {
    k_gmpack_max_ortho_layers = 16u,
};

typedef struct GpuMathOrthoOverlay {
    alignas(64) uint64_t pixels_bda; // Bytes 0..7:   Vulkan BDA of RGBA8 image pixels (width_px * height_px * 4B)
    uint32_t width_px;               // Bytes 8..11:  Image width in pixels
    uint32_t height_px;              // Bytes 12..15: Image height in pixels
    float    center_x;               // Bytes 16..19: Bird's-eye center World X (meters, East = +X)
    float    center_y;               // Bytes 20..23: Bird's-eye center World Y (meters, North = +Y)
    float    half_span_x;            // Bytes 24..27: Half-width in World X (meters, stretched in 0.5% steps)
    float    half_span_y;            // Bytes 28..31: Half-height in World Y (meters, stretched in 0.5% steps)
    float    opacity;                // Bytes 32..35: 1.0 - transparency (default 0.50 = 50% transparent)
    uint32_t active;                 // Bytes 36..39: 1 when interactive bird's-eye overlay preview is shown
    float    scale_pct_x;            // Bytes 40..43: Cumulative X stretch % (starts at 100.0%, +-0.5% steps)
    float    scale_pct_y;            // Bytes 44..47: Cumulative Y stretch % (starts at 100.0%, +-0.5% steps)
    uint32_t committed_cnt;          // Bytes 48..51: Count of Enter uploads committed into tiles
    uint32_t pad[3];                 // Bytes 52..63: Pad to 64 bytes
} GpuMathOrthoOverlay;

static_assert(sizeof(GpuMathOrthoOverlay) == 64u, "GpuMathOrthoOverlay must be 64 bytes");

// On-disk & in-memory 24,576-byte Metadata Prefix (6 x 4KB blocks)
typedef struct GpuMathArchiveHeader {
    alignas(4096) GpuMathPageDirectory dir;                           // Bytes 0..255
    GpuMathQuadNode      nodes[k_gmpack_node_count];                  // Bytes 256..22079 (341 * 64B)
    GpuMathOrthoOverlay  overlay;                                     // Bytes 22080..22143 (64B live bird's-eye overlay state)
    uint32_t             ortho_layer_count;                           // Bytes 22144..22147 (Committed full-res GPU ortho layers)
    uint32_t             ortho_layer_pad[15];                         // Bytes 22148..22207 (Pad to 64B alignment)
    GpuMathOrthoOverlay  ortho_layers[k_gmpack_max_ortho_layers];     // Bytes 22208..23231 (16 * 64B full-res layers)
    uint8_t              pad_to_24k[k_gmpack_header_bytes - 23232u];  // Bytes 23232..24575 (Pad to 24,576 bytes)
} GpuMathArchiveHeader;

static_assert(offsetof(GpuMathArchiveHeader, overlay) == 22080u, "overlay offset must be 22,080");
static_assert(offsetof(GpuMathArchiveHeader, ortho_layer_count) == 22144u, "ortho_layer_count offset must be 22,144");
static_assert(offsetof(GpuMathArchiveHeader, ortho_layers) == 22208u, "ortho_layers offset must be 22,208");
static_assert(sizeof(GpuMathArchiveHeader) == k_gmpack_header_bytes, "Archive header must be 24,576 bytes");

// 16-Byte Indirect Draw Command Record (matches VkDrawIndirectCommand layout)
typedef struct GpuMathDrawCmd {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} GpuMathDrawCmd;

static_assert(sizeof(GpuMathDrawCmd) == 16u, "GpuMathDrawCmd must be 16 bytes");

// 1,344-Byte (21 x 64B cache lines) GPU-Driven Indirect Culling & Draw Buffer
// Written by shaders/terrain_cull.comp.slang and consumed by vkCmdDrawIndirectCount
// (or vkCmdDrawIndirect fallback) + shaders/terrain.vert.slang via BDA.
typedef struct GpuMathIndirectBuffer {
    alignas(64) uint32_t draw_count;                          // Offset 0:   Visible leaf draw count (countBufferOffset = 0)
    uint32_t             culled_count;                        // Offset 4:   Frustum-culled leaf count
    uint32_t             active_leaf_total;                   // Offset 8:   Total active leaves before frustum culling
    uint32_t             pad0;                                // Offset 12
    GpuMathDrawCmd       single_cmd;                          // Offset 16:  Single instanced VkDrawIndirectCommand fallback
    uint32_t             visible_nodes[k_gmpack_active_cut_cap]; // Offset 32:  Compacted visible quadtree leaf node indices
    GpuMathDrawCmd       cmds[k_gmpack_active_cut_cap];       // Offset 288: Multi-draw VkDrawIndirectCommand array (64 * 16B)
    uint32_t             pad_tail[8];                         // Offset 1312..1343: Pad to 64-byte multiple (1,344 B)
} GpuMathIndirectBuffer;

static_assert(sizeof(GpuMathIndirectBuffer) == 1344u, "GpuMathIndirectBuffer must be 1,344 bytes");

[[nodiscard]] uint32_t backend_quadtree_level_base(uint8_t lod);
[[nodiscard]] uint32_t backend_quadtree_node_index(uint8_t lod, uint8_t gx, uint8_t gy);
[[nodiscard]] int      backend_terrain_ensure_pack(const char *pack_path);
[[nodiscard]] int      backend_terrain_read_header(const char *pack_path, GpuMathArchiveHeader *out_hdr);
[[nodiscard]] int      backend_terrain_read_tile(const char *pack_path, uint32_t node_index, GpuMathTileBlock *out_tile);
[[nodiscard]] int      backend_terrain_write_tile(const char *pack_path, uint32_t node_index, const GpuMathTileBlock *in_tile);

// .epack v1 Codec & Archive I/O
void                   backend_epack_encode_tile(const GpuMathTileBlock *src, EpackTileBlock *dst);
void                   backend_epack_decode_tile(const EpackTileBlock *src, GpuMathTileBlock *dst);
[[nodiscard]] float    backend_epack_load_height_m(const EpackTileBlock *tile, uint32_t sx, uint32_t sy);
[[nodiscard]] uint32_t backend_epack_load_rgba8(const EpackTileBlock *tile, uint32_t sx, uint32_t sy);
[[nodiscard]] int      backend_terrain_ensure_epack(const char *epack_path, const char *gmpack_path);
[[nodiscard]] int      backend_terrain_read_epack_header(const char *epack_path, const char *gmpack_path, GpuMathArchiveHeader *out_hdr);
[[nodiscard]] int      backend_terrain_read_epack_tile(const char *epack_path, uint32_t node_index, EpackTileBlock *out_tile);
[[nodiscard]] int      backend_terrain_write_epack_tile(const char *epack_path, uint32_t node_index, const EpackTileBlock *in_tile);

#endif /* ENGINE_BACKEND_TERRAIN_PACK_H */
