#ifndef ENGINE_ECS_H
#define ENGINE_ECS_H

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Engine v0.3.2: Zero-Allocation Cache-Aligned SoA ECS & 3D Transform Hierarchy
// - 32-bit generational EcsEntity handles (12-bit index + 20-bit generation)
// - O(1) sparse-to-dense indirection with 64B cache-line aligned SoA arrays
// - Zero-recursion topological depth-bucket 3D transform propagation
// - Exact row-major 3x4 world affine matrices + 3x3 cofactor normal matrices
// - Arvo tight world AABB + bounding sphere propagation in the same pass
// - Cycle-safe world-pose-preserving reparenting (keep_world_pose)
// ============================================================================

enum : uint32_t {
    ECS_NULL_ENTITY   = 0u,
    ECS_INDEX_BITS    = 12u,
    ECS_INDEX_MASK    = (1u << ECS_INDEX_BITS) - 1u,
    ECS_GEN_SHIFT     = ECS_INDEX_BITS,
    ECS_MAX_ENTITIES  = 1024u,
    ECS_MAX_DEPTH     = 16u,
    ECS_NAME_CAP      = 24u,
    ECS_INVALID_DENSE = 0xFFFFFFFFu,
};

typedef uint32_t EcsEntity;

typedef enum : uint32_t {
    ECS_COMP_NONE      = 0u,
    ECS_COMP_TRANSFORM = 1u << 0,
    ECS_COMP_HIERARCHY = 1u << 1,
    ECS_COMP_BOUNDS    = 1u << 2,
    ECS_COMP_RENDER    = 1u << 3,
    ECS_COMP_PHYSICS   = 1u << 4,
    ECS_COMP_SELECT    = 1u << 5,
} EcsCompMask;

typedef enum : uint8_t {
    ECS_KIND_EMPTY   = 0u,
    ECS_KIND_CUBE    = 1u,
    ECS_KIND_SOLID   = 2u,
    ECS_KIND_MESH    = 3u,
    ECS_KIND_FACE    = 4u,
    ECS_KIND_TERRAIN = 5u,
} EcsKind;

typedef enum : uint8_t {
    ECS_DIRTY_NONE   = 0u,
    ECS_DIRTY_LOCAL  = 1u << 0,
    ECS_DIRTY_WORLD  = 1u << 1,
} EcsDirtyFlags;

typedef struct EcsTrs {
    float pos[3];
    float rot[4]; // Unit quaternion (x, y, z, w)
    float scale[3];
} EcsTrs;

typedef struct EcsAabb {
    float min_v[3];
    float max_v[3];
} EcsAabb;

typedef struct EcsSphere {
    float center[3];
    float radius;
} EcsSphere;

// Cache-line aligned Structure-of-Arrays (SoA) ECS World (zero heap allocations).
typedef struct EcsWorld {
    // Sparse slot indirection (indexed by entity slot [0..ECS_MAX_ENTITIES-1])
    uint32_t  sparse_to_dense[ECS_MAX_ENTITIES];
    uint32_t  slot_gen[ECS_MAX_ENTITIES];
    uint32_t  free_list[ECS_MAX_ENTITIES];
    uint32_t  free_count;

    // Dense entity handle table [0..dense_count-1]
    uint32_t  dense_count;
    alignas(64) EcsEntity dense_entity[ECS_MAX_ENTITIES];
    alignas(64) uint32_t  comp_mask[ECS_MAX_ENTITIES];
    alignas(64) int32_t   user_slot[ECS_MAX_ENTITIES];
    alignas(64) uint8_t   kind[ECS_MAX_ENTITIES];
    char                  name[ECS_MAX_ENTITIES][ECS_NAME_CAP];

    // Local Transform SoA (contiguous SIMD-friendly arrays)
    alignas(64) float lx[ECS_MAX_ENTITIES];
    alignas(64) float ly[ECS_MAX_ENTITIES];
    alignas(64) float lz[ECS_MAX_ENTITIES];
    alignas(64) float qx[ECS_MAX_ENTITIES];
    alignas(64) float qy[ECS_MAX_ENTITIES];
    alignas(64) float qz[ECS_MAX_ENTITIES];
    alignas(64) float qw[ECS_MAX_ENTITIES];
    alignas(64) float sx[ECS_MAX_ENTITIES];
    alignas(64) float sy[ECS_MAX_ENTITIES];
    alignas(64) float sz[ECS_MAX_ENTITIES];

    // World Transform Cache SoA (computed by topological depth-bucket propagation)
    alignas(64) float world_m[ECS_MAX_ENTITIES][12];   // Row-major 3x4 affine world matrix
    alignas(64) float world_nrm[ECS_MAX_ENTITIES][9];  // Row-major 3x3 cofactor normal matrix
    alignas(64) float world_pos[ECS_MAX_ENTITIES][3];
    alignas(64) float world_rot[ECS_MAX_ENTITIES][4];
    alignas(64) float world_scl[ECS_MAX_ENTITIES][3];
    alignas(64) float bound_scale[ECS_MAX_ENTITIES];

    // Hierarchy SoA (intrusive linked sibling list + depth + dirty flags)
    alignas(64) EcsEntity parent[ECS_MAX_ENTITIES];
    alignas(64) EcsEntity first_child[ECS_MAX_ENTITIES];
    alignas(64) EcsEntity next_sibling[ECS_MAX_ENTITIES];
    alignas(64) EcsEntity prev_sibling[ECS_MAX_ENTITIES];
    alignas(64) uint16_t  depth[ECS_MAX_ENTITIES];
    alignas(64) uint8_t   dirty[ECS_MAX_ENTITIES];

    // Bounds SoA (local OBB half-extents + transformed Arvo world AABB + sphere)
    alignas(64) float local_ext[ECS_MAX_ENTITIES][3];
    alignas(64) float local_rad[ECS_MAX_ENTITIES];
    alignas(64) float world_aabb_min[ECS_MAX_ENTITIES][3];
    alignas(64) float world_aabb_max[ECS_MAX_ENTITIES][3];
    alignas(64) float world_sphere[ECS_MAX_ENTITIES][4]; // (cx, cy, cz, radius)

    // Cached topological depth-sorted traversal order (rebuilt only when hierarchy changes)
    uint32_t  topo_order[ECS_MAX_ENTITIES];
    uint32_t  topo_count;
    bool      topo_dirty;
    uint32_t  propagate_writes; // Telemetry: transforms recomputed in last propagation pass
} EcsWorld;

// ----------------------------------------------------------------------------
// Pure C23 Quaternion & 3x4 Affine Matrix Math Kernels
// ----------------------------------------------------------------------------

void ecs_quat_identity(float out_q[4]);
void ecs_quat_norm(float q[4]);
void ecs_quat_mul(const float a[4], const float b[4], float out_q[4]);
void ecs_quat_conj(const float q[4], float out_q[4]);
void ecs_quat_from_axis_angle(const float axis[3], float angle_rad, float out_q[4]);
// Matches legacy rotate_pose(v, pitch_x, yaw_y) = Ry(yaw_y) * Rx(pitch_x).
void ecs_quat_from_pose(float pitch_x_rad, float yaw_y_rad, float out_q[4]);
void ecs_quat_to_euler_deg(const float q[4], float out_deg[3]);
void ecs_quat_rotate_vec3(const float q[4], const float v[3], float out_v[3]);
void ecs_quat_to_mat3(const float q[4], float out_m3[9]);
void ecs_mat3_to_quat(const float m3[9], float out_q[4]);

void ecs_mat3x4_from_trs(const float pos[3], const float q[4], const float scale[3],
                         float out_m[12]);
void ecs_mat3x4_mul(const float a[12], const float b[12], float out_m[12]);
void ecs_mat3x4_cofactor3(const float m[12], float out_nrm[9]);
[[nodiscard]] bool ecs_mat3x4_invert(const float m[12], float out_inv[12]);
void ecs_mat3x4_transform_point(const float m[12], const float p[3], float out_p[3]);
void ecs_mat3x4_decompose(const float m[12], float out_pos[3], float out_rot[4], float out_scale[3]);

// ----------------------------------------------------------------------------
// SoA ECS Lifecycle, Entity Management & Queries
// ----------------------------------------------------------------------------

void ecs_world_init(EcsWorld *w);
void ecs_world_reset(EcsWorld *w);

[[nodiscard]] EcsEntity ecs_spawn(EcsWorld *w, const char *name, EcsKind kind,
                                  uint32_t comp_mask, int32_t user_slot);
void ecs_destroy(EcsWorld *w, EcsEntity e);
[[nodiscard]] bool ecs_alive(const EcsWorld *w, EcsEntity e);
[[nodiscard]] uint32_t ecs_dense_index(const EcsWorld *w, EcsEntity e);
[[nodiscard]] uint32_t ecs_entity_count(const EcsWorld *w);
[[nodiscard]] EcsEntity ecs_entity_at(const EcsWorld *w, uint32_t dense_idx);
[[nodiscard]] uint32_t ecs_query_mask(const EcsWorld *w, uint32_t required_mask,
                                      EcsEntity *out_entities, uint32_t cap);

const char *ecs_get_name(const EcsWorld *w, EcsEntity e);
EcsKind ecs_get_kind(const EcsWorld *w, EcsEntity e);
int32_t ecs_get_user_slot(const EcsWorld *w, EcsEntity e);
void ecs_set_user_slot(EcsWorld *w, EcsEntity e, int32_t user_slot);
void ecs_set_local_bounds(EcsWorld *w, EcsEntity e, const float half_ext[3], float radius);

// ----------------------------------------------------------------------------
// 3D Local/World Transforms & Topological Hierarchy Propagation
// ----------------------------------------------------------------------------

void ecs_set_local_trs(EcsWorld *w, EcsEntity e, const float pos[3],
                       const float rot_q[4], const float scale[3]);
void ecs_set_local_pos(EcsWorld *w, EcsEntity e, float x, float y, float z);
void ecs_set_local_rot(EcsWorld *w, EcsEntity e, const float rot_q[4]);
void ecs_set_local_scale(EcsWorld *w, EcsEntity e, float sx, float sy, float sz);
bool ecs_get_local_trs(const EcsWorld *w, EcsEntity e, EcsTrs *out_trs);

// World-space setters automatically convert through inv(M_parent) for nested children.
void ecs_set_world_pos(EcsWorld *w, EcsEntity e, float x, float y, float z);
void ecs_set_world_rot(EcsWorld *w, EcsEntity e, const float rot_q[4]);
void ecs_set_world_scale(EcsWorld *w, EcsEntity e, float sx, float sy, float sz);
void ecs_set_world_trs(EcsWorld *w, EcsEntity e, const float pos[3],
                       const float rot_q[4], const float scale[3]);

// Cycle-safe parenting. When keep_world_pose is true, converts world pose into
// the new parent's local frame so the entity never teleports on attach/detach.
[[nodiscard]] bool ecs_set_parent(EcsWorld *w, EcsEntity child, EcsEntity new_parent,
                                  bool keep_world_pose);
[[nodiscard]] EcsEntity ecs_get_parent(const EcsWorld *w, EcsEntity e);
[[nodiscard]] uint32_t ecs_get_depth(const EcsWorld *w, EcsEntity e);
[[nodiscard]] uint32_t ecs_count_children(const EcsWorld *w, EcsEntity e);
[[nodiscard]] bool ecs_is_descendant(const EcsWorld *w, EcsEntity maybe_desc, EcsEntity ancestor);

// Zero-recursion topological depth-bucket propagation pass.
void ecs_propagate_transforms(EcsWorld *w);

bool ecs_get_world_matrix(const EcsWorld *w, EcsEntity e, float out_m[12]);
bool ecs_get_world_trs(const EcsWorld *w, EcsEntity e, EcsTrs *out_trs);
bool ecs_get_world_aabb(const EcsWorld *w, EcsEntity e, EcsAabb *out_aabb);
bool ecs_get_world_sphere(const EcsWorld *w, EcsEntity e, EcsSphere *out_sphere);

#endif /* ENGINE_ECS_H */
