#ifndef ENGINE_PHYSICS_H
#define ENGINE_PHYSICS_H

#include "vulkan/types.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Engine v0.2 Milestone 5: First-Class Unified GPU/CPU Physics Kernel (AVBD)
// - 2026 Augmented Vertex Block Descent (Giles, Diaz & Yuksel, SIGGRAPH 25/26)
// - Square-Root-Free 3x3 LDL^T Register Block Solver (Immune to fp32 k^3 Overflow)
// - Greedy Conflict Graph Coloring + CSR Adjacency + Fused SLM Compute Kernel
// - Rockafellar-Wets Unilateral Contact + SPD-Stabilized Hookean Soft/Rigid Solids
// - Dual-Architecture Profile Split:
//     * UMA_IGPU:        4 primal-dual iterations, zero-copy LLC BDA slab
//     * DISCRETE_DIRECT: 8 primal-dual iterations, ring-buffered BDA compute
// ============================================================================

enum : uint32_t {
    K_AVBD_MAGIC         = 0x44425641u, // "AVBD"
    K_AVBD_MAX_VERTS     = 256u,
    K_AVBD_MAX_CONS      = 512u,
    K_AVBD_MAX_CSR_EDGES = 1024u,
    K_AVBD_MAX_COLORS    = 16u,
    K_AVBD_WARM_CACHE    = 512u,
    K_AVBD_SLICE_BYTES   = 65536u,      // 64 KiB per frame ring slot (256 KiB total)
};

enum : uint32_t {
    AVBD_CON_SPRING_3D     = 0u, // Bilateral distance / Hookean spring between (vert_a, vert_b)
    AVBD_CON_ANCHOR_3D     = 1u, // Bilateral 3D target pin on vert_a toward normal_or_target[3]
    AVBD_CON_CONTACT_PLANE = 2u, // Unilateral Rockafellar-Wets half-space n.x >= d on vert_a
    AVBD_CON_CONTACT_PAIR  = 3u, // Unilateral Rockafellar-Wets separation n.(x_b - x_a) >= d_sep
};

enum : uint32_t {
    AVBD_EXEC_CPU_ONLY    = 0u,
    AVBD_EXEC_UMA_SLM_4   = 1u,
    AVBD_EXEC_DGPU_SLM_8  = 2u,
};

// 64-byte cache-line-aligned primal vertex / particle / body center record.
typedef struct AvbdVertex {
    alignas(64) float pos[3];
    float    inv_mass;      // 0.0f = kinematic / fixed; > 0.0f = 1 / mass
    float    vel[3];
    float    radius;
    float    prev_pos[3];
    float    restitution;
    float    inert_y[3];    // Inertial prediction y_i = x_i^t + h v_i^t + h^2 g
    uint32_t flags;
} AvbdVertex;

static_assert(sizeof(AvbdVertex) == 64u, "AvbdVertex must be 64 bytes");

// 64-byte cache-line-aligned primal-dual augmented-Lagrangian constraint record.
typedef struct AvbdConstraint {
    alignas(64) uint32_t kind;
    uint32_t vert_a;
    uint32_t vert_b;              // UINT32_MAX for unary constraints
    uint32_t hash_key;            // Deterministic key for across-frame multiplier warm-starting
    float    normal_or_target[3]; // Unit normal (PLANE / PAIR) or world target (ANCHOR_3D)
    float    rest_or_offset;      // Rest length l_0 (SPRING), plane offset d (PLANE), sep (PAIR)
    float    stiffness_min;       // Initial/minimum penalty stiffness k_min
    float    stiffness_max;       // Maximum adaptive penalty cap k_max
    float    penalty_k;           // Current augmented-Lagrangian penalty k_c
    float    lambda[3];           // Dual multiplier (lambda[0] for 1D, lambda[0..2] for ANCHOR_3D)
    float    beta_ramp;           // Penalty ramping factor beta (e.g. 4.0f)
    float    compliance_pad;
} AvbdConstraint;

static_assert(sizeof(AvbdConstraint) == 64u, "AvbdConstraint must be 64 bytes");

// 256-byte BDA dispatch header consumed by shaders/physics_avbd.comp.slang.
typedef struct AvbdStepHeader {
    alignas(64) uint32_t magic;
    uint32_t vert_count;
    uint32_t con_count;
    uint32_t num_colors;
    uint32_t iterations;
    uint32_t csr_edge_count;
    float    dt;
    float    warm_decay;
    float    gravity[3];
    float    damp_keep;
    uint64_t verts_bda;
    uint64_t cons_bda;
    uint64_t csr_row_bda;
    uint64_t csr_col_bda;
    uint64_t color_verts_bda;
    uint64_t stats_bda;
    uint32_t color_start[K_AVBD_MAX_COLORS];
    uint32_t color_count[K_AVBD_MAX_COLORS];
    uint32_t gpu_completed_iters;
    float    max_violation;
    uint32_t reserved[6];
} AvbdStepHeader;

static_assert(offsetof(AvbdStepHeader, verts_bda) == 48u, "verts_bda offset");
static_assert(offsetof(AvbdStepHeader, color_start) == 96u, "color_start offset");
static_assert(offsetof(AvbdStepHeader, gpu_completed_iters) == 224u, "gpu_completed_iters offset");
static_assert(sizeof(AvbdStepHeader) == 256u, "AvbdStepHeader must be 256 bytes");

// Persistent warm-start cache entry for dual multipliers across simulation ticks.
typedef struct AvbdWarmEntry {
    uint32_t hash_key;
    uint32_t age;
    float    penalty_k;
    float    lambda[3];
} AvbdWarmEntry;

typedef struct AvbdWarmCache {
    AvbdWarmEntry entries[K_AVBD_WARM_CACHE];
    uint32_t      count;
} AvbdWarmCache;

typedef struct PhysicsBenchStats {
    bool     gpu_enabled;
    uint32_t exec_mode;
    uint32_t iterations;
    uint32_t active_verts;
    uint32_t active_constraints;
    uint32_t num_colors;
    uint32_t dispatches_frame;
    uint64_t bda_bytes;
} PhysicsBenchStats;

// ----------------------------------------------------------------------------
// Pure C23 Mathematical & Variational AVBD Kernels (CPU + Standalone Testable)
// ----------------------------------------------------------------------------

// Solves symmetric positive-definite 3x3 linear system H * out_dx = rhs using
// square-root-free LDL^T factorization in scalar registers (immune to fp32 k^3 overflow).
// h_diag = {H00, H11, H22}, h_off = {H01, H02, H12}.
void avbd_solve_sym3x3(const float h_diag[3],
                       const float h_off[3],
                       const float rhs[3],
                       float       out_dx[3]);

// Builds compressed sparse row (CSR) vertex-to-constraint incidence tables.
// Returns total CSR incidence entries written to out_col_con (capped at col_cap).
uint32_t avbd_build_csr(uint32_t              vert_count,
                        const AvbdConstraint *cons,
                        uint32_t              con_count,
                        uint32_t             *out_row_ptr,
                        uint32_t             *out_col_con,
                        uint32_t              col_cap);

// Greedy conflict graph coloring over binary constraints so all vertices within
// color c in [0..num_colors-1] share zero constraints and solve race-free in parallel.
// Populates out_color_verts[0..vert_count-1], out_color_start[16], out_color_count[16].
uint32_t avbd_color_graph(const AvbdVertex     *verts,
                          uint32_t              vert_count,
                          const AvbdConstraint *cons,
                          uint32_t              con_count,
                          const uint32_t       *csr_row_ptr,
                          const uint32_t       *csr_col_con,
                          uint32_t             *out_color_verts,
                          uint32_t              out_color_start[K_AVBD_MAX_COLORS],
                          uint32_t              out_color_count[K_AVBD_MAX_COLORS]);

// Warm-start cache lookup & commit helpers (gamma warm-start decay in [0, 1]).
void avbd_warm_cache_reset(AvbdWarmCache *cache);
void avbd_warm_apply(const AvbdWarmCache *cache,
                     AvbdConstraint      *cons,
                     uint32_t             con_count,
                     float                warm_gamma);
void avbd_warm_store(AvbdWarmCache        *cache,
                     const AvbdConstraint *cons,
                     uint32_t              con_count);

// Executes one full primal-dual Augmented Vertex Block Descent timestep in-place,
// returning the maximum residual constraint violation (meters / world units).
float avbd_solve_step(AvbdVertex     *verts,
                      uint32_t        vert_count,
                      AvbdConstraint *cons,
                      uint32_t        con_count,
                      float           dt,
                      const float     gravity[3],
                      uint32_t        iterations);

// Solves a 2D deformable soft-body mass-spring patch (cols x rows, structural +
// shear + 2-hop flexion constraints) via graph-colored 3x3 AVBD.
// Optionally outputs generated AvbdVertex / AvbdConstraint arrays for GPU BDA submission.
void avbd_step_soft_grid_2d(float          *io_x,
                            float          *io_y,
                            float          *io_vx,
                            float          *io_vy,
                            uint32_t        cols,
                            uint32_t        rows,
                            float           spacing,
                            float           spring_stiff,
                            float           damp_rate,
                            float           gx,
                            float           gy,
                            float           dt,
                            uint32_t        iterations,
                            AvbdVertex     *opt_gpu_verts,
                            uint32_t       *opt_vert_count,
                            AvbdConstraint *opt_gpu_cons,
                            uint32_t       *opt_con_count);

[[nodiscard]] bool physics_self_check(void);

// ----------------------------------------------------------------------------
// Vulkan 1.4 BDA GPU Physics Runtime (Fused Workgroup SLM Compute Kernel)
// ----------------------------------------------------------------------------

[[nodiscard]] int physics_init(VulkanContext *vk);
void physics_shutdown(VulkanContext *vk);
void physics_reset(void);

// Returns profile-tuned primal-dual iteration count (4 on UMA_IGPU, 8 on DISCRETE/HYBRID).
uint32_t physics_recommended_iterations(void);

// Queues a pre-step AVBD system into the lazy 256 KiB BDA ring slab for GPU compute execution.
void physics_submit_step(const AvbdVertex     *verts,
                         uint32_t              vert_count,
                         const AvbdConstraint *cons,
                         uint32_t              con_count,
                         float                 dt,
                         const float           gravity[3],
                         float                 damp_keep);

// Records pending fused SLM AVBD dispatches into the frame compute command buffer.
void physics_record_compute(VkCommandBuffer cmd);

bool physics_gpu_active(void);
bool physics_toggle_gpu(void);
void physics_get_stats(PhysicsBenchStats *out_stats);

#endif /* ENGINE_PHYSICS_H */
