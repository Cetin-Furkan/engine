#include "world/world_int.h"
#include "gltf/gltf.h"
#include "scene/scene.h"
#include "texture/texture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum : uint32_t {
    k_model_node_cap = 384u,
    k_model_prim_cap = 512u,
};

typedef struct ModelPrimSlot {
    uint32_t scene_mesh_id;
    uint32_t pbr_mat_id;
    uint32_t inst_flags;
    uint32_t vertex_count;
} ModelPrimSlot;

typedef struct ModelNodeRec {
    bool     used;
    bool     is_root;
    char     name[64];
    uint32_t first_prim;
    uint32_t prim_count;
    uint32_t total_verts;
} ModelNodeRec;

static ModelNodeRec  s_nodes[k_model_node_cap];
static uint32_t      s_node_count = 0u;
static ModelPrimSlot s_prims[k_model_prim_cap];
static uint32_t      s_prim_count = 0u;
static SceneSlabMark s_slab_mark  = {0};
static bool          s_has_mark   = false;
static const WorldOps s_ops;

static bool ray_aabb_hit(const CameraRay *ray, const EcsAabb *box, float *out_t) {
    float tmin = 0.01f;
    float tmax = 1.0e9f;
    const float ro[3] = { ray->ox, ray->oy, ray->oz };
    const float rd[3] = { ray->dx, ray->dy, ray->dz };
    for (int a = 0; a < 3; ++a) {
        if (fabsf(rd[a]) < 1.0e-7f) {
            if (ro[a] < box->min_v[a] || ro[a] > box->max_v[a]) {
                return false;
            }
        } else {
            float inv = 1.0f / rd[a];
            float t0  = (box->min_v[a] - ro[a]) * inv;
            float t1  = (box->max_v[a] - ro[a]) * inv;
            if (t0 > t1) {
                float tmp = t0; t0 = t1; t1 = tmp;
            }
            if (t0 > tmin) tmin = t0;
            if (t1 < tmax) tmax = t1;
            if (tmin > tmax) {
                return false;
            }
        }
    }
    if (out_t != nullptr) {
        *out_t = tmin;
    }
    return true;
}

static bool model_hit(const WorldObj *o, const CameraRay *ray, float *t, Coord *at) {
    if (o == nullptr || o->data == nullptr || ray == nullptr) {
        return false;
    }
    const ModelNodeRec *n = (const ModelNodeRec *)o->data;
    if (n->prim_count == 0u) {
        return false;
    }
    EcsAabb box = {0};
    if (!ecs_get_world_aabb(world_ecs(), o->entity, &box)) {
        return false;
    }
    float hit_t = 0.0f;
    if (!ray_aabb_hit(ray, &box, &hit_t)) {
        return false;
    }
    if (t != nullptr) {
        *t = hit_t;
    }
    if (at != nullptr) {
        *at = (Coord){
            ray->ox + ray->dx * hit_t,
            ray->oy + ray->dy * hit_t,
            ray->oz + ray->dz * hit_t,
        };
    }
    return true;
}

static void model_prepare_scene(const WorldObj *o, bool live) {
    (void)live;
    if (o == nullptr || o->data == nullptr) {
        return;
    }
    const ModelNodeRec *n = (const ModelNodeRec *)o->data;
    if (n->prim_count == 0u) {
        return;
    }
    float world_m[12];
    if (!ecs_get_world_matrix(world_ecs(), o->entity, world_m)) {
        return;
    }
    for (uint32_t i = 0u; i < n->prim_count; ++i) {
        const ModelPrimSlot *ps = &s_prims[n->first_prim + i];
        if (ps->scene_mesh_id != K_SCENE_INVALID_MESH) {
            scene_submit_instance_pbr(ps->scene_mesh_id, world_m,
                                      ps->pbr_mat_id, ps->inst_flags, o->entity);
        }
    }
}

static bool model_remove(WorldObj *o) {
    if (o == nullptr || o->data == nullptr) {
        return false;
    }
    ModelNodeRec *n = (ModelNodeRec *)o->data;
    o->alive = false;
    printf("[Model] Removed '%s'.\n", n->name);
    fflush(stdout);
    return true;
}

static uint32_t model_vertices(const WorldObj *o) {
    if (o == nullptr || o->data == nullptr) {
        return 0u;
    }
    const ModelNodeRec *n = (const ModelNodeRec *)o->data;
    return n->total_verts;
}

static const char *model_name(const WorldObj *o) {
    if (o == nullptr || o->data == nullptr) {
        return "Model";
    }
    const ModelNodeRec *n = (const ModelNodeRec *)o->data;
    return n->name[0] != '\0' ? n->name : "Model";
}

static const WorldOps s_ops = {
    .hit           = model_hit,
    .prepare_scene = model_prepare_scene,
    .remove        = model_remove,
    .vertices      = model_vertices,
    .name          = model_name,
};

void model_kind_reset(void) {
    if (s_has_mark) {
        scene_slab_restore(s_slab_mark);
        texture_reset_scene_assets();
        s_has_mark = false;
    }
    memset(s_nodes, 0, sizeof s_nodes);
    memset(s_prims, 0, sizeof s_prims);
    s_node_count = 0u;
    s_prim_count = 0u;
}

static uint32_t upload_gltf_image_cached(const GltfAsset *asset, int32_t img_idx, bool srgb,
                                         uint32_t *cache_srgb, uint32_t *cache_lin) {
    if (img_idx < 0 || (uint32_t)img_idx >= asset->image_count) {
        return srgb ? K_TEX_WHITE_SRGB : K_TEX_WHITE_LINEAR;
    }
    uint32_t *cache = srgb ? cache_srgb : cache_lin;
    if (cache[img_idx] != K_TEX_INVALID) {
        return cache[img_idx];
    }
    const GltfImage *im = &asset->images[img_idx];
    uint32_t tex_id = K_TEX_INVALID;
    if (im->encoded != nullptr && im->encoded_len > 0u) {
        tex_id = texture_create_from_memory(im->encoded, im->encoded_len, srgb);
    } else if (im->uri_path[0] != '\0') {
        tex_id = texture_create_from_file(im->uri_path, srgb);
    }
    if (tex_id == K_TEX_INVALID) {
        tex_id = srgb ? K_TEX_WHITE_SRGB : K_TEX_WHITE_LINEAR;
    }
    cache[img_idx] = tex_id;
    return tex_id;
}

static const char *path_basename(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return "Model";
    }
    const char *slash = strrchr(path, '/');
    return (slash != nullptr && slash[1] != '\0') ? (slash + 1) : path;
}

bool model_kind_spawn(const char *path, Coord at, float scale, bool auto_fit) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    GltfAsset asset = {0};
    if (gltf_load_file(path, &asset) != 0) {
        return false;
    }
    if (asset.prim_count == 0u || asset.node_count == 0u) {
        fprintf(stderr, "[Model] '%s' contains no renderable mesh primitives.\n", path);
        gltf_asset_free(&asset);
        return false;
    }

    if (!s_has_mark) {
        s_slab_mark = scene_slab_mark();
        s_has_mark  = true;
    }
    (void)texture_ensure_ready();

    // 1. Deduplicated texture upload cache per (image_index, srgb)
    uint32_t *tex_srgb = asset.image_count > 0u ? malloc(asset.image_count * sizeof(uint32_t)) : nullptr;
    uint32_t *tex_lin  = asset.image_count > 0u ? malloc(asset.image_count * sizeof(uint32_t)) : nullptr;
    for (uint32_t i = 0u; i < asset.image_count; ++i) {
        if (tex_srgb != nullptr) tex_srgb[i] = K_TEX_INVALID;
        if (tex_lin  != nullptr) tex_lin[i]  = K_TEX_INVALID;
    }

    // 2. Register default PBR material + all glTF materials in the BDA PBR material table
    PbrMaterialGpu def_mat = texture_pbr_material_default();
    uint32_t def_mat_id = texture_register_material(&def_mat);

    uint32_t *mat_ids   = asset.material_count > 0u ? calloc(asset.material_count, sizeof(uint32_t)) : nullptr;
    uint32_t *mat_flags = asset.material_count > 0u ? calloc(asset.material_count, sizeof(uint32_t)) : nullptr;
    for (uint32_t m = 0u; m < asset.material_count; ++m) {
        const GltfMaterial *gm = &asset.materials[m];
        PbrMaterialGpu gpu = texture_pbr_material_default();
        memcpy(gpu.base_color_factor, gm->base_color, sizeof gpu.base_color_factor);
        memcpy(gpu.emissive_factor, gm->emissive, sizeof gpu.emissive_factor);
        gpu.metallic_factor    = gm->metallic;
        gpu.roughness_factor   = gm->roughness;
        gpu.normal_scale       = gm->normal_scale;
        gpu.occlusion_strength = gm->occlusion_strength;
        gpu.alpha_cutoff       = gm->alpha_cutoff;
        gpu.ior                = gm->ior;
        gpu.flags              = gm->flags;

        if (gm->base_tex.image_index >= 0) {
            gpu.tex_base  = upload_gltf_image_cached(&asset, gm->base_tex.image_index, true, tex_srgb, tex_lin);
            gpu.samp_base = texture_sampler_index(gm->base_tex.wrap_s, gm->base_tex.wrap_t, gm->base_tex.filter);
        }
        if (gm->mr_tex.image_index >= 0) {
            gpu.tex_mr  = upload_gltf_image_cached(&asset, gm->mr_tex.image_index, false, tex_srgb, tex_lin);
            gpu.samp_mr = texture_sampler_index(gm->mr_tex.wrap_s, gm->mr_tex.wrap_t, gm->mr_tex.filter);
        }
        if (gm->normal_tex.image_index >= 0) {
            gpu.tex_normal  = upload_gltf_image_cached(&asset, gm->normal_tex.image_index, false, tex_srgb, tex_lin);
            gpu.samp_normal = texture_sampler_index(gm->normal_tex.wrap_s, gm->normal_tex.wrap_t, gm->normal_tex.filter);
        }
        if (gm->occ_tex.image_index >= 0) {
            gpu.tex_occ  = upload_gltf_image_cached(&asset, gm->occ_tex.image_index, false, tex_srgb, tex_lin);
            gpu.samp_occ = texture_sampler_index(gm->occ_tex.wrap_s, gm->occ_tex.wrap_t, gm->occ_tex.filter);
        }
        if (gm->emissive_tex.image_index >= 0) {
            gpu.tex_emissive  = upload_gltf_image_cached(&asset, gm->emissive_tex.image_index, true, tex_srgb, tex_lin);
            gpu.samp_emissive = texture_sampler_index(gm->emissive_tex.wrap_s, gm->emissive_tex.wrap_t, gm->emissive_tex.filter);
        }

        // Apply KHR_texture_transform (Blender Mapping node) from primary textured slot
        const GltfTextureInfo *xf_src = nullptr;
        if (gm->base_tex.has_transform)          xf_src = &gm->base_tex;
        else if (gm->normal_tex.has_transform)   xf_src = &gm->normal_tex;
        else if (gm->mr_tex.has_transform)       xf_src = &gm->mr_tex;
        else if (gm->occ_tex.has_transform)      xf_src = &gm->occ_tex;
        else if (gm->emissive_tex.has_transform) xf_src = &gm->emissive_tex;
        if (xf_src != nullptr) {
            gpu.uv_offset[0] = xf_src->uv_offset[0];
            gpu.uv_offset[1] = xf_src->uv_offset[1];
            gpu.uv_scale[0]  = xf_src->uv_scale[0];
            gpu.uv_scale[1]  = xf_src->uv_scale[1];
            gpu.uv_cos       = cosf(xf_src->uv_rotation);
            gpu.uv_sin       = sinf(xf_src->uv_rotation);
        }

        mat_ids[m] = texture_register_material(&gpu);
        uint32_t ifl = SCENE_INST_FLAG_PBR;
        if ((gm->flags & PBR_MAT_FLAG_DOUBLE_SIDED) != 0u) ifl |= SCENE_INST_FLAG_TWO_SIDED;
        if ((gm->flags & PBR_MAT_FLAG_ALPHA_MASK)   != 0u) ifl |= SCENE_INST_FLAG_ALPHA_MASK;
        if ((gm->flags & PBR_MAT_FLAG_ALPHA_BLEND)  != 0u) ifl |= SCENE_INST_FLAG_ALPHA_BLEND;
        mat_flags[m] = ifl;
    }

    // 3. Register every glTF primitive in the unified 32B/vert BDA meshlet slab
    uint32_t *prim_mesh_ids = calloc(asset.prim_count, sizeof(uint32_t));
    if (prim_mesh_ids == nullptr) {
        free(tex_srgb); free(tex_lin); free(mat_ids); free(mat_flags);
        gltf_asset_free(&asset);
        return false;
    }
    uint32_t total_tris = 0u;
    for (uint32_t p = 0u; p < asset.prim_count; ++p) {
        const GltfPrimitive *pr = &asset.prims[p];
        prim_mesh_ids[p] = scene_register_mesh_pbr(pr->corners, pr->corner_count,
                                                   pr->has_uvs, pr->has_tangents);
        total_tris += pr->corner_count / 3u;
    }

    // 4. Compute studio auto-fit / grounding transform (exact glTF Y-up -> Engine Z-up root)
    float dx = asset.bounds_max[0] - asset.bounds_min[0];
    float dy = asset.bounds_max[1] - asset.bounds_min[1];
    float dz = asset.bounds_max[2] - asset.bounds_min[2];
    float max_ext = fmaxf(dx, fmaxf(dy, dz));

    float root_s = scale;
    float root_pos[3] = { at.x, at.y, at.z };
    if (auto_fit || root_s <= 0.0f) {
        root_s = max_ext > 1.0e-5f ? (280.0f / max_ext) : 100.0f;
        float cx = 0.5f * (asset.bounds_min[0] + asset.bounds_max[0]);
        float cy = 0.5f * (asset.bounds_min[1] + asset.bounds_max[1]);
        root_pos[0] = at.x - cx * root_s;
        root_pos[1] = at.y - cy * root_s;
        root_pos[2] = at.z - asset.bounds_min[2] * root_s;
    }

    // 5. Instantiate Root + Full Blender / glTF Scene Hierarchy into SoA ECS & WorldObj table
    EcsWorld *ecs = world_ecs();
    EcsEntity *node_ents = calloc(asset.node_count, sizeof(EcsEntity));
    int root_tbl_idx = -1;
    EcsEntity root_ent = ECS_NULL_ENTITY;

    if (s_node_count < k_model_node_cap) {
        ModelNodeRec *rrec = &s_nodes[s_node_count++];
        *rrec = (ModelNodeRec){ .used = true, .is_root = true };
        snprintf(rrec->name, sizeof rrec->name, "%s", path_basename(path));
        root_tbl_idx = table_add(&s_ops, rrec, (Coord){ root_pos[0], root_pos[1], root_pos[2] }, nullptr);
        if (root_tbl_idx >= 0) {
            WorldObj *ro = table_at(root_tbl_idx);
            root_ent = ro != nullptr ? ro->entity : ECS_NULL_ENTITY;
            if (root_ent != ECS_NULL_ENTITY) {
                float q_yup[4];
                gltf_yup_to_zup_quat(q_yup);
                const float scl[3] = { root_s, root_s, root_s };
                const float ext[3] = {
                    fmaxf(dx * 0.5f, 0.25f),
                    fmaxf(dy * 0.5f, 0.25f),
                    fmaxf(dz * 0.5f, 0.25f),
                };
                float rad = sqrtf(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
                ecs_set_local_trs(ecs, root_ent, root_pos, q_yup, scl);
                ecs_set_local_bounds(ecs, root_ent, ext, rad);
            }
        }
    }

    uint32_t spawned_nodes = 0u;
    for (uint32_t i = 0u; i < asset.node_count && s_node_count < k_model_node_cap; ++i) {
        const GltfNode *gn = &asset.nodes[i];
        ModelNodeRec *nrec = &s_nodes[s_node_count++];
        *nrec = (ModelNodeRec){
            .used       = true,
            .is_root    = false,
            .first_prim = s_prim_count,
            .prim_count = 0u,
        };
        snprintf(nrec->name, sizeof nrec->name, "%s", gn->name);

        float lmin[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
        float lmax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
        bool  have_b  = false;

        if (gn->mesh_index >= 0 && (uint32_t)gn->mesh_index < asset.mesh_count) {
            const GltfMesh *mesh = &asset.meshes[gn->mesh_index];
            for (uint32_t p = 0u; p < mesh->prim_count && s_prim_count < k_model_prim_cap; ++p) {
                uint32_t pid = mesh->first_prim + p;
                const GltfPrimitive *pr = &asset.prims[pid];
                uint32_t mid = def_mat_id;
                uint32_t mfl = SCENE_INST_FLAG_PBR;
                if (pr->material_index >= 0 && (uint32_t)pr->material_index < asset.material_count) {
                    mid = mat_ids[pr->material_index];
                    mfl = mat_flags[pr->material_index];
                }
                s_prims[s_prim_count++] = (ModelPrimSlot){
                    .scene_mesh_id = prim_mesh_ids[pid],
                    .pbr_mat_id    = mid,
                    .inst_flags    = mfl,
                    .vertex_count  = pr->corner_count,
                };
                nrec->prim_count++;
                nrec->total_verts += pr->corner_count;
                for (int a = 0; a < 3; ++a) {
                    if (pr->local_min[a] < lmin[a]) lmin[a] = pr->local_min[a];
                    if (pr->local_max[a] > lmax[a]) lmax[a] = pr->local_max[a];
                }
                have_b = true;
            }
        }

        int tidx = table_add(&s_ops, nrec, (Coord){ 0.0f, 0.0f, 0.0f }, nullptr);
        if (tidx < 0) {
            break;
        }
        WorldObj *wo = table_at(tidx);
        EcsEntity ent = wo != nullptr ? wo->entity : ECS_NULL_ENTITY;
        if (node_ents != nullptr) {
            node_ents[i] = ent;
        }
        if (ent != ECS_NULL_ENTITY) {
            uint32_t d = ecs_dense_index(ecs, ent);
            if (d != ECS_INVALID_DENSE) {
                ecs->kind[d] = (uint8_t)(nrec->prim_count > 0u ? ECS_KIND_MESH : ECS_KIND_EMPTY);
            }
            ecs_set_local_trs(ecs, ent, gn->translation, gn->rotation, gn->scale);
            float ext[3] = { 0.25f, 0.25f, 0.25f };
            if (have_b) {
                ext[0] = fmaxf(0.5f * (lmax[0] - lmin[0]), 0.05f);
                ext[1] = fmaxf(0.5f * (lmax[1] - lmin[1]), 0.05f);
                ext[2] = fmaxf(0.5f * (lmax[2] - lmin[2]), 0.05f);
            }
            float rad = sqrtf(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
            ecs_set_local_bounds(ecs, ent, ext, rad);
        }
        spawned_nodes++;
    }

    // 6. Wire exact parent-child ECS hierarchy links across all Blender / glTF nodes
    uint32_t hier_links = 0u;
    if (node_ents != nullptr) {
        for (uint32_t i = 0u; i < asset.node_count; ++i) {
            if (node_ents[i] == ECS_NULL_ENTITY) {
                continue;
            }
            int32_t p = asset.nodes[i].parent_index;
            EcsEntity parent_ent = root_ent;
            if (p >= 0 && (uint32_t)p < asset.node_count && node_ents[p] != ECS_NULL_ENTITY) {
                parent_ent = node_ents[p];
            }
            if (parent_ent != ECS_NULL_ENTITY && ecs_set_parent(ecs, node_ents[i], parent_ent, false)) {
                hier_links++;
            }
        }
    }
    ecs_propagate_transforms(ecs);
    table_sync_ecs(true);
    if (root_tbl_idx >= 0) {
        table_select_quiet(root_tbl_idx);
    }

    TextureBenchStats tstat = {0};
    texture_get_stats(&tstat);
    printf("[Model] Loaded '%s'%s: %u nodes (%u hierarchy links), %u prims, %u tris, %u PBR mats, %u bindless textures (scale=%.1f)\n",
           path,
           asset.from_blend ? " [Blender .blend -> cached GLB]" : "",
           spawned_nodes, hier_links, asset.prim_count, total_tris,
           asset.material_count, tstat.texture_count, (double)root_s);
    fflush(stdout);

    free(node_ents);
    free(prim_mesh_ids);
    free(tex_srgb);
    free(tex_lin);
    free(mat_ids);
    free(mat_flags);
    gltf_asset_free(&asset);
    return true;
}
