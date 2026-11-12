#ifndef ENGINE_SCENE_H
#define ENGINE_SCENE_H

#include "vulkan/types.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Engine v0.2 Milestone 3: Unified Meshlet Scene Kernel, Subgroup Hi-Z
// Pyramid, Two-Phase GPU Occlusion Culling & R32_UINT Visibility Buffer
// ============================================================================

enum : uint32_t {
    K_SCENE_MESHLET_MAX_VERTS = 64u,
    K_SCENE_MESHLET_MAX_TRIS  = 124u,
    K_SCENE_HIZ_MAX_MIPS      = 12u,
    K_SCENE_MAX_MESHES        = 32u,
    K_SCENE_MAX_MESHLETS      = 1024u,
    K_SCENE_MAX_INSTANCES     = 256u,
    K_SCENE_INVALID_MESH      = 0xFFFFFFFFu,
};

enum : uint32_t {
    SCENE_INST_FLAG_TWO_SIDED = 1u << 0,
};

// 16-byte quantized vertex (2.25x smaller than 36-byte float3x3 MeshVertex,
// naturally 16-byte aligned for single 128-bit BDA vector load).
typedef struct ScenePackedVertex {
    alignas(16) uint16_t px;
    uint16_t py;
    uint16_t pz;
    uint16_t pad_u16;
    int16_t  oct_nx;
    int16_t  oct_ny;
    uint32_t rgba8; // R8 | (G8 << 8) | (B8 << 16) | (roughness8 << 24)
} ScenePackedVertex;

static_assert(sizeof(ScenePackedVertex) == 16u, "ScenePackedVertex must be 16 bytes");

// 64-byte cache-line-aligned meshlet header stored in device-local BDA slab.
typedef struct SceneMeshlet {
    alignas(64) float origin[3]; // Local AABB minimum for 16-bit dequantization
    uint32_t vert_offset;      // First ScenePackedVertex in BDA vertex array
    float    scale[3];         // Local AABB extent (max - min)
    uint32_t tri_offset;       // First packed uint32_t (i0 | i1<<8 | i2<<16) in BDA index array
    float    center[3];        // Conservative post-quantization bounding sphere center
    float    radius;           // Conservative post-quantization bounding sphere radius
    int16_t  cone_axis_oct[2]; // Octahedral snorm16 average face normal axis
    int16_t  cone_cutoff_s16;  // Conservative -sin(half_angle) in snorm16 (-32768 disables cone cull)
    uint8_t  vert_count;       // 1..64
    uint8_t  tri_count;        // 1..124
    uint32_t mesh_id;          // Registered SceneMesh slot
    uint32_t flags;            // Reserved / meshlet flags
} SceneMeshlet;

static_assert(sizeof(SceneMeshlet) == 64u, "SceneMeshlet must be 64 bytes");

// 48-byte camera + material view block (matches ViewPush in shaders/camera.slang).
typedef struct SceneViewParams {
    uint64_t materials_bda;
    uint32_t mat_index;
    uint32_t mat_pad;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
} SceneViewParams;

static_assert(sizeof(SceneViewParams) == 48u, "SceneViewParams must match 48-byte ViewPush");

// 64-byte per-frame instance descriptor uploaded to transient BDA slice.
typedef struct SceneInstance {
    alignas(64) float origin_x;
    float    origin_y;
    float    origin_z;
    float    uniform_scale;
    float    pitch_rad;
    float    yaw_rad;
    float    squash;
    float    roughness;
    float    selected;
    float    depth_bias;
    uint32_t mat_index;
    uint32_t mesh_id;
    uint32_t first_meshlet;
    uint32_t meshlet_count;
    uint32_t flags;
    uint32_t pad0;
} SceneInstance;

static_assert(sizeof(SceneInstance) == 64u, "SceneInstance must be 64 bytes");

// Input unindexed corner (compatible byte-for-byte with ObjCorner and MeshVertex).
typedef struct SceneCorner {
    float x, y, z;
    float nx, ny, nz;
    float r, g, b;
} SceneCorner;

static_assert(sizeof(SceneCorner) == 36u, "SceneCorner must match 36-byte MeshVertex/ObjCorner");

// CPU-built meshlet clusterization output ready for GPU BDA upload or unit inspection.
typedef struct SceneMeshletBuild {
    SceneMeshlet      *meshlets;
    uint32_t           meshlet_count;
    ScenePackedVertex *vertices;
    uint32_t           vertex_count;
    uint32_t          *triangles; // Packed (i0) | (i1 << 8) | (i2 << 16)
    uint32_t           triangle_count;
} SceneMeshletBuild;

// Analytical 2D / perspective-correct barycentric weights and screen-space partial derivatives.
typedef struct SceneBarycentrics2D {
    float lambda[3];    // Perspective-correct weights (lambda_0, lambda_1, lambda_2)
    float dlambda_dx[3];// Analytical screen-space d(lambda)/dx (per pixel)
    float dlambda_dy[3];// Analytical screen-space d(lambda)/dy (per pixel)
    float z_ndc;        // Screen-linear post-divide NDC depth
    bool  valid;        // False on degenerate zero-area triangle
} SceneBarycentrics2D;

// Live GPU culling, Hi-Z, and Visibility Buffer telemetry.
typedef struct SceneBenchStats {
    uint32_t registered_meshes;
    uint32_t slab_meshlets;
    uint64_t slab_bytes;
    uint64_t hiz_bytes;
    uint32_t active_instances;
    uint32_t active_meshlets;
    uint32_t visible_meshlets;
    uint32_t phase1_draws;
    uint32_t phase2_draws;
    uint32_t cone_culled;
    uint32_t frustum_culled;
    uint32_t hiz_culled;
    uint32_t hiz_mips;
    bool     visbuf_enabled;
    bool     hiz_enabled;
} SceneBenchStats;

// ---------------------------------------------------------------------------
// 32-Bit Visibility Buffer ID Packing: ((instance_id + 1) : 14) | (meshlet_id : 11) | (prim_id : 7)
// 0u is reserved as the background clear sentinel.
// ---------------------------------------------------------------------------

static inline uint32_t scene_visbuf_pack(uint32_t instance_id, uint32_t meshlet_id, uint32_t prim_id) {
    uint32_t inst_p1 = (instance_id + 1u) & 0x3FFFu;
    return (inst_p1 << 18u) | ((meshlet_id & 0x7FFu) << 7u) | (prim_id & 0x7Fu);
}

static inline bool scene_visbuf_unpack(uint32_t vis_id, uint32_t *out_instance_id,
                                       uint32_t *out_meshlet_id, uint32_t *out_prim_id) {
    uint32_t inst_p1 = (vis_id >> 18u) & 0x3FFFu;
    if (inst_p1 == 0u) {
        return false;
    }
    if (out_instance_id != nullptr) {
        *out_instance_id = inst_p1 - 1u;
    }
    if (out_meshlet_id != nullptr) {
        *out_meshlet_id = (vis_id >> 7u) & 0x7FFu;
    }
    if (out_prim_id != nullptr) {
        *out_prim_id = vis_id & 0x7Fu;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Quantization, Analytical Barycentrics & Meshlet Clusterizer (CPU + Testable)
// ---------------------------------------------------------------------------

void scene_oct_encode_snorm16(float nx, float ny, float nz, int16_t out_oct[2]);
void scene_oct_decode_snorm16(const int16_t oct[2], float out_nrm[3]);

void scene_quantize_pos16(const float pos[3], const float origin[3], const float scale[3],
                          uint16_t out_q[3]);
void scene_dequantize_pos16(const uint16_t q[3], const float origin[3], const float scale[3],
                            float out_pos[3]);

uint32_t scene_pack_rgba8(float r, float g, float b, float roughness);

SceneBarycentrics2D scene_eval_barycentrics_2d(const float clip0[4], const float clip1[4],
                                               const float clip2[4], float px_center,
                                               float py_center, float viewport_w,
                                               float viewport_h);

[[nodiscard]] bool scene_self_check(void);

[[nodiscard]] int scene_build_meshlets(const SceneCorner *corners, uint32_t corner_count,
                                       float default_roughness, SceneMeshletBuild *out_build);
void scene_meshlet_build_free(SceneMeshletBuild *build);

// ---------------------------------------------------------------------------
// GPU Scene Runtime (BDA Meshlet Slab, Subgroup Hi-Z, Two-Phase Cull, VisBuf)
// ---------------------------------------------------------------------------

[[nodiscard]] int scene_init(VulkanContext *vk, VkFormat depth_format,
                             VkSampleCountFlagBits samples);
void scene_shutdown(VulkanContext *vk);

[[nodiscard]] uint32_t scene_register_mesh(const SceneCorner *corners, uint32_t corner_count,
                                           float default_roughness);
[[nodiscard]] int scene_update_mesh(uint32_t mesh_id, const SceneCorner *corners,
                                    uint32_t corner_count, float default_roughness);

void scene_claim_frame(VkSemaphore acquire, uint64_t signal_point);
void scene_abort_frame(uint64_t signal_point);
void scene_begin_frame(uint32_t width, uint32_t height, const SceneViewParams *view);
void scene_submit_instance(uint32_t mesh_id, float origin_x, float origin_y, float origin_z,
                           float uniform_scale, float pitch_rad, float yaw_rad,
                           float squash, float roughness, float selected,
                           float depth_bias, uint32_t mat_index, uint32_t flags);

void scene_prepare_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);
void scene_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

bool scene_visbuf_active(void);
bool scene_toggle_visbuf(void);
void scene_invalidate_hiz(void);
void scene_get_stats(SceneBenchStats *out_stats);

#endif /* ENGINE_SCENE_H */
