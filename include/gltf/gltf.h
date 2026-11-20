#ifndef ENGINE_GLTF_H
#define ENGINE_GLTF_H

#include "scene/scene.h"
#include "texture/texture.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Engine v0.3.3: Strict ISO C23 glTF 2.0 (.gltf / .glb) & Native Blender (.blend)
// - Full glTF 2.0 core spec + Blender Principled BSDF extensions:
//   KHR_texture_transform, KHR_materials_emissive_strength,
//   KHR_materials_ior, KHR_materials_unlit
// - Native .blend support via cached headless Blender glTF 2.0 export
//   (modifiers applied, Geometry Nodes, MikkTSpace tangents, packed textures)
// - Exact Y-up (glTF) -> Z-up (Blender / Engine) root conversion preserving
//   every local TRS, nested parent-child hierarchy, and tangent handedness
// ============================================================================

typedef struct GltfTextureInfo {
    int32_t       image_index; // -1 if none
    TextureWrap   wrap_s;
    TextureWrap   wrap_t;
    TextureFilter filter;
    float         uv_offset[2];
    float         uv_scale[2];
    float         uv_rotation;
    bool          has_transform;
} GltfTextureInfo;

typedef struct GltfMaterial {
    char            name[64];
    float           base_color[4];      // Linear RGBA
    float           emissive[3];        // Linear RGB * KHR_materials_emissive_strength
    float           metallic;
    float           roughness;
    float           normal_scale;
    float           occlusion_strength;
    float           alpha_cutoff;
    float           ior;
    uint32_t        flags;              // PbrMaterialFlags
    GltfTextureInfo base_tex;
    GltfTextureInfo mr_tex;
    GltfTextureInfo normal_tex;
    GltfTextureInfo occ_tex;
    GltfTextureInfo emissive_tex;
} GltfMaterial;

typedef struct GltfImage {
    uint8_t *encoded;      // Owned PNG/JPEG/WebP bytes (from GLB chunk or data URI), or nullptr
    size_t   encoded_len;
    char     uri_path[512]; // Resolved path for external relative .gltf textures
} GltfImage;

typedef struct GltfPrimitive {
    ScenePbrCorner *corners;
    uint32_t        corner_count;
    int32_t         material_index; // -1 => default material
    float           local_min[3];
    float           local_max[3];
    bool            has_uvs;
    bool            has_tangents;
} GltfPrimitive;

typedef struct GltfMesh {
    char     name[64];
    uint32_t first_prim;
    uint32_t prim_count;
} GltfMesh;

typedef struct GltfNode {
    char     name[64];
    int32_t  parent_index;   // -1 for scene roots
    int32_t  mesh_index;     // -1 for Blender Empty / pivot / collection root
    float    translation[3]; // Local translation (glTF space)
    float    rotation[4];    // Local unit quaternion (x, y, z, w)
    float    scale[3];       // Local scale (may have negative component for mirrored Blender objects)
    bool     negative_det;   // True if node local scale flips winding
} GltfNode;

typedef struct GltfAsset {
    char           source_path[512];
    char           resolved_glb[512]; // Cache .glb path when loaded from .blend
    bool           from_blend;
    GltfNode      *nodes;
    uint32_t       node_count;
    GltfMesh      *meshes;
    uint32_t       mesh_count;
    GltfPrimitive *prims;
    uint32_t       prim_count;
    GltfMaterial  *materials;
    uint32_t       material_count;
    GltfImage     *images;
    uint32_t       image_count;
    float          bounds_min[3]; // Whole-scene AABB in engine Z-up coordinates (1:1 meters)
    float          bounds_max[3];
} GltfAsset;

// Exact +90 deg rotation around +X: converts glTF Right-Handed Y-Up -> Engine/Blender Z-Up.
void gltf_yup_to_zup_quat(float out_q[4]);

// Generates smooth Lengyel/MikkTSpace-compatible orthogonalized tangents + bitangent sign (tw = +-1).
void gltf_generate_tangents(ScenePbrCorner *corners, uint32_t corner_count);

// Exports a Blender .blend file into a deterministic mtime/size-keyed cached .glb via headless Blender.
[[nodiscard]] int gltf_blend_export_to_glb(const char *blend_path, char *out_glb_path, size_t out_cap);

// Detects .blend, .glb, or .gltf automatically and loads the complete scene graph, meshes, materials & images.
[[nodiscard]] int gltf_load_file(const char *path, GltfAsset *out_asset);

// Loads a .glb binary blob directly from memory (base_dir optional for external URIs).
[[nodiscard]] int gltf_load_glb_memory(const uint8_t *bytes, size_t len,
                                       const char *base_dir, GltfAsset *out_asset);

void gltf_asset_free(GltfAsset *asset);

#endif /* ENGINE_GLTF_H */
