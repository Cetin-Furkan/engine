#ifndef ENGINE_GIZMO_H
#define ENGINE_GIZMO_H

#include "ecs/ecs.h"
#include "vulkan/types.h"
#include <stdint.h>

// ============================================================================
// Engine v0.3.2: Interactive 3D Transform Gizmos & 3D Debug-Draw Overlay
// - Translate (X/Y/Z shafts+cones + XY/XZ/YZ plane quads + free screen center)
// - Rotate (X/Y/Z 3D torus rings + swept angle pie wedge + screen roll ring)
// - Scale (X/Y/Z cube-tipped shafts + uniform center cube)
// - Perspective-invariant screen-space sizing, World/Local frame toggle, Ctrl snap
// - 3D Debug Overlays: Arvo World AABBs, Parent->Child Hierarchy Arrows, Local Triads
// - Rendered in crisp 1x overlay pass via Vulkan 1.4 transient BDA (0 descriptor sets)
// ============================================================================

typedef enum : uint8_t {
    GIZMO_MODE_TRANSLATE = 0u,
    GIZMO_MODE_ROTATE    = 1u,
    GIZMO_MODE_SCALE     = 2u,
    GIZMO_MODE_NONE      = 3u,
} GizmoMode;

typedef enum : uint8_t {
    GIZMO_SPACE_WORLD = 0u,
    GIZMO_SPACE_LOCAL = 1u,
} GizmoSpace;

typedef enum : uint8_t {
    GIZMO_HANDLE_NONE   = 0u,
    GIZMO_HANDLE_X      = 1u,
    GIZMO_HANDLE_Y      = 2u,
    GIZMO_HANDLE_Z      = 3u,
    GIZMO_HANDLE_XY     = 4u,
    GIZMO_HANDLE_XZ     = 5u,
    GIZMO_HANDLE_YZ     = 6u,
    GIZMO_HANDLE_CENTER = 7u,
} GizmoHandle;

typedef struct GizmoVertex {
    float    x;
    float    y;
    float    z;
    uint32_t rgba;
} GizmoVertex;

static_assert(sizeof(GizmoVertex) == 16u, "GizmoVertex must be 16 bytes");

typedef struct GizmoCamera {
    float q[4];
    float cam[3];
    float zoom;
    float persp;
    float eye_w[3];
    float fwd_w[3];
} GizmoCamera;

typedef struct GizmoRay {
    float origin[3];
    float dir[3];
} GizmoRay;

typedef struct GizmoState {
    GizmoMode   mode;
    GizmoSpace  space;
    GizmoHandle hovered;
    GizmoHandle active;
    EcsEntity   drag_entity;
    bool        dragging;
    bool        snap_active;
    bool        debug_draw;

    // Captured start-of-drag state for drift-free interactive transforms
    float start_pivot[3];
    float start_axes[3][3];
    float start_pos[3];
    float start_rot[4];
    float start_scale[3];
    float drag_plane_n[3];
    float drag_start_hit[3];
    float drag_axis_t0;
    float drag_start_angle;
    float drag_curr_angle;
    float drag_world_scale;
} GizmoState;

// ----------------------------------------------------------------------------
// Pure C23 Gizmo Math, Analytic Picking, Dragging & Geometry Builder
// ----------------------------------------------------------------------------

void gizmo_state_init(GizmoState *st);
void gizmo_set_mode(GizmoState *st, GizmoMode mode);
void gizmo_cycle_mode(GizmoState *st);
void gizmo_toggle_space(GizmoState *st);
bool gizmo_toggle_debug_draw(GizmoState *st);
const char *gizmo_mode_name(GizmoMode mode);
const char *gizmo_space_name(GizmoSpace space);

// Derives world eye position and look forward direction from camera pose.
void gizmo_camera_fill(GizmoCamera *out, const float q[4], const float cam[3],
                       float zoom, float persp);

// Perspective-invariant world size corresponding to target_px on screen at pivot_w.
[[nodiscard]] float gizmo_compute_scale(const GizmoCamera *cam, const float pivot_w[3],
                                        float target_px);

// Computes the gizmo orthonormal 3-axis basis (World XYZ or Entity Local basis).
void gizmo_basis_for_entity(const EcsWorld *w, EcsEntity e, GizmoSpace space,
                            float out_pivot[3], float out_axes[3][3]);

// Exact analytic 3D ray intersection against all active gizmo handles.
[[nodiscard]] GizmoHandle gizmo_pick_handle(const GizmoState *st, const EcsWorld *w,
                                            EcsEntity e, const GizmoCamera *cam,
                                            const GizmoRay *ray);

// Updates hover state; returns true iff the hovered handle changed (for zero-idle redraw).
[[nodiscard]] bool gizmo_hover(GizmoState *st, const EcsWorld *w, EcsEntity e,
                               const GizmoCamera *cam, const GizmoRay *ray);

// Begins a gizmo drag if the ray hits any handle of entity e.
[[nodiscard]] bool gizmo_begin_drag(GizmoState *st, const EcsWorld *w, EcsEntity e,
                                    const GizmoCamera *cam, const GizmoRay *ray);

// Projects pointer ray onto the active constraint (axis/plane/ring) and updates ECS entity.
[[nodiscard]] bool gizmo_update_drag(GizmoState *st, EcsWorld *w,
                                     const GizmoCamera *cam, const GizmoRay *ray,
                                     bool snap_enabled);

void gizmo_end_drag(GizmoState *st);

// Builds solid + ribbon 3D triangles for the active gizmo and/or ECS debug overlays.
[[nodiscard]] uint32_t gizmo_build_vertices(const GizmoState *st,
                                            const EcsWorld *w,
                                            EcsEntity selected,
                                            const GizmoCamera *cam,
                                            GizmoVertex *out_verts,
                                            uint32_t max_verts);

// ----------------------------------------------------------------------------
// Vulkan 1.4 Transient-BDA Overlay Renderer
// ----------------------------------------------------------------------------

[[nodiscard]] int gizmo_gpu_init(VulkanContext *vk);
void gizmo_gpu_shutdown(VulkanContext *vk);
void gizmo_gpu_draw(VkCommandBuffer cmd,
                    uint32_t width,
                    uint32_t height,
                    VkFormat color_format,
                    const void *view_push_ptr,
                    size_t view_push_bytes,
                    const GizmoState *st,
                    const EcsWorld *w,
                    EcsEntity selected,
                    const GizmoCamera *cam);

#endif /* ENGINE_GIZMO_H */
