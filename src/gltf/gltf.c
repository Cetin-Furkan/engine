#include "gltf/gltf_int.h"
#include "gltf/json.h"
#include "ecs/ecs.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum : uint32_t {
    k_glb_magic      = 0x46546C67u, // "glTF"
    k_glb_chunk_json = 0x4E4F534Au, // "JSON"
    k_glb_chunk_bin  = 0x004E4942u, // "BIN\0"
    k_wrap_clamp     = 33071u,
    k_wrap_mirror    = 33648u,
    k_wrap_repeat    = 10497u,
};

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint8_t *slurp_file_bytes(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return nullptr;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return nullptr;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 512l * 1024l * 1024l || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return nullptr;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (buf == nullptr) {
        fclose(fp);
        return nullptr;
    }
    size_t got = fread(buf, 1u, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return nullptr;
    }
    if (out_len != nullptr) {
        *out_len = got;
    }
    return buf;
}

static void extract_dir(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (slash == nullptr) {
        out[0] = '\0';
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static bool has_ext_ci(const char *path, const char *ext) {
    const size_t plen = strlen(path);
    const size_t elen = strlen(ext);
    if (plen < elen) {
        return false;
    }
    const char *tail = path + (plen - elen);
    for (size_t i = 0u; i < elen; ++i) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) {
            return false;
        }
    }
    return true;
}

static TextureWrap gltf_wrap_to_engine(int32_t v) {
    if (v == (int32_t)k_wrap_clamp)  return TEX_WRAP_CLAMP;
    if (v == (int32_t)k_wrap_mirror) return TEX_WRAP_MIRROR;
    return TEX_WRAP_REPEAT;
}

static TextureFilter gltf_filter_to_engine(int32_t mag, int32_t min) {
    if (mag == 9728 || min == 9728 || min == 9984 || min == 9986) {
        return TEX_FILTER_NEAREST;
    }
    return TEX_FILTER_LINEAR;
}

static void parse_texture_info(const JsonVal *tex_obj,
                               const GltfTexRef *tex_refs, uint32_t tex_count,
                               const GltfSamplerInfo *samplers, uint32_t samp_count,
                               GltfTextureInfo *out) {
    *out = (GltfTextureInfo){
        .image_index = -1,
        .wrap_s      = TEX_WRAP_REPEAT,
        .wrap_t      = TEX_WRAP_REPEAT,
        .filter      = TEX_FILTER_LINEAR,
        .uv_offset   = { 0.0f, 0.0f },
        .uv_scale    = { 1.0f, 1.0f },
        .uv_rotation = 0.0f,
    };
    if (tex_obj == nullptr || tex_obj->type != JSON_OBJECT) {
        return;
    }
    const int32_t tidx = json_as_i32(json_obj_get(tex_obj, "index"), -1);
    if (tidx < 0 || (uint32_t)tidx >= tex_count) {
        return;
    }
    out->image_index = tex_refs[tidx].source;
    const int32_t sidx = tex_refs[tidx].sampler;
    if (sidx >= 0 && (uint32_t)sidx < samp_count) {
        out->wrap_s = samplers[sidx].wrap_s;
        out->wrap_t = samplers[sidx].wrap_t;
        out->filter = samplers[sidx].filter;
    }

    const JsonVal *exts  = json_obj_get(tex_obj, "extensions");
    const JsonVal *xform = json_obj_get(exts, "KHR_texture_transform");
    if (xform != nullptr && xform->type == JSON_OBJECT) {
        out->has_transform = true;
        (void)json_read_f32_arr(json_obj_get(xform, "offset"), out->uv_offset, 2u);
        (void)json_read_f32_arr(json_obj_get(xform, "scale"),  out->uv_scale,  2u);
        out->uv_rotation = (float)json_as_num(json_obj_get(xform, "rotation"), 0.0);
    }
}

static int parse_gltf_doc(const JsonVal *root, const uint8_t *glb_bin, size_t glb_bin_len,
                          const char *base_dir, GltfAsset *out) {
    if (root == nullptr || root->type != JSON_OBJECT || out == nullptr) {
        return -1;
    }

    // 1. Buffers
    const JsonVal *j_bufs = json_obj_get(root, "buffers");
    const uint32_t buf_count = (j_bufs != nullptr && j_bufs->type == JSON_ARRAY) ? j_bufs->child_count : 0u;
    GltfBufData *bufs = buf_count > 0u ? calloc(buf_count, sizeof(*bufs)) : nullptr;
    if (buf_count > 0u && bufs == nullptr) {
        return -1;
    }

    uint32_t bi = 0u;
    for (const JsonVal *b = j_bufs != nullptr ? j_bufs->first_child : nullptr;
         b != nullptr && bi < buf_count; b = b->next_sibling, ++bi) {
        const char *uri = json_as_str(json_obj_get(b, "uri"), nullptr);
        if (uri == nullptr) {
            if (bi == 0u && glb_bin != nullptr) {
                bufs[bi].bytes = (uint8_t *)glb_bin;
                bufs[bi].len   = glb_bin_len;
                bufs[bi].owned = false;
            }
        } else if (strncmp(uri, "data:", 5u) == 0) {
            const char *comma = strchr(uri, ',');
            if (comma != nullptr) {
                size_t dlen = 0u;
                bufs[bi].bytes = gltf_decode_base64(comma + 1, strlen(comma + 1), &dlen);
                bufs[bi].len   = dlen;
                bufs[bi].owned = true;
            }
        } else {
            char fpath[1024];
            gltf_resolve_uri_path(base_dir, uri, fpath, sizeof fpath);
            size_t flen = 0u;
            bufs[bi].bytes = slurp_file_bytes(fpath, &flen);
            bufs[bi].len   = flen;
            bufs[bi].owned = true;
        }
    }

    // 2. BufferViews
    const JsonVal *j_bvs = json_obj_get(root, "bufferViews");
    const uint32_t bv_count = (j_bvs != nullptr && j_bvs->type == JSON_ARRAY) ? j_bvs->child_count : 0u;
    GltfBufView *bvs = bv_count > 0u ? calloc(bv_count, sizeof(*bvs)) : nullptr;
    uint32_t bvi = 0u;
    for (const JsonVal *bv = j_bvs != nullptr ? j_bvs->first_child : nullptr;
         bv != nullptr && bvi < bv_count; bv = bv->next_sibling, ++bvi) {
        bvs[bvi] = (GltfBufView){
            .buffer      = (uint32_t)json_as_i32(json_obj_get(bv, "buffer"), 0),
            .byte_offset = (size_t)json_as_num(json_obj_get(bv, "byteOffset"), 0.0),
            .byte_length = (size_t)json_as_num(json_obj_get(bv, "byteLength"), 0.0),
            .byte_stride = (size_t)json_as_num(json_obj_get(bv, "byteStride"), 0.0),
        };
    }

    // 3. Accessors
    const JsonVal *j_accs = json_obj_get(root, "accessors");
    const uint32_t acc_count = (j_accs != nullptr && j_accs->type == JSON_ARRAY) ? j_accs->child_count : 0u;
    GltfAccessor *accs = acc_count > 0u ? calloc(acc_count, sizeof(*accs)) : nullptr;
    uint32_t ai = 0u;
    for (const JsonVal *a = j_accs != nullptr ? j_accs->first_child : nullptr;
         a != nullptr && ai < acc_count; a = a->next_sibling, ++ai) {
        accs[ai] = (GltfAccessor){
            .buffer_view    = json_as_i32(json_obj_get(a, "bufferView"), -1),
            .byte_offset    = (size_t)json_as_num(json_obj_get(a, "byteOffset"), 0.0),
            .component_type = (uint32_t)json_as_i32(json_obj_get(a, "componentType"), 0),
            .count          = (uint32_t)json_as_i32(json_obj_get(a, "count"), 0),
            .num_comps      = gltf_type_comps(json_as_str(json_obj_get(a, "type"), nullptr)),
            .normalized     = json_as_bool(json_obj_get(a, "normalized"), false),
            .sparse_idx_bv  = -1,
            .sparse_val_bv  = -1,
        };
        const JsonVal *sp = json_obj_get(a, "sparse");
        if (sp != nullptr && sp->type == JSON_OBJECT) {
            const JsonVal *si = json_obj_get(sp, "indices");
            const JsonVal *sv = json_obj_get(sp, "values");
            accs[ai].has_sparse       = true;
            accs[ai].sparse_count     = (uint32_t)json_as_i32(json_obj_get(sp, "count"), 0);
            accs[ai].sparse_idx_bv    = json_as_i32(json_obj_get(si, "bufferView"), -1);
            accs[ai].sparse_idx_off   = (size_t)json_as_num(json_obj_get(si, "byteOffset"), 0.0);
            accs[ai].sparse_idx_ctype = (uint32_t)json_as_i32(json_obj_get(si, "componentType"), 0);
            accs[ai].sparse_val_bv    = json_as_i32(json_obj_get(sv, "bufferView"), -1);
            accs[ai].sparse_val_off   = (size_t)json_as_num(json_obj_get(sv, "byteOffset"), 0.0);
        }
    }

    // 4. Images
    const JsonVal *j_imgs = json_obj_get(root, "images");
    const uint32_t img_count = (j_imgs != nullptr && j_imgs->type == JSON_ARRAY) ? j_imgs->child_count : 0u;
    if (img_count > 0u) {
        out->images      = calloc(img_count, sizeof(*out->images));
        out->image_count = out->images != nullptr ? img_count : 0u;
    }
    uint32_t ii = 0u;
    for (const JsonVal *im = j_imgs != nullptr ? j_imgs->first_child : nullptr;
         im != nullptr && ii < out->image_count; im = im->next_sibling, ++ii) {
        const int32_t bv_idx = json_as_i32(json_obj_get(im, "bufferView"), -1);
        const char   *uri    = json_as_str(json_obj_get(im, "uri"), nullptr);
        if (bv_idx >= 0 && (uint32_t)bv_idx < bv_count) {
            const GltfBufView *bv = &bvs[bv_idx];
            if (bv->buffer < buf_count && bufs[bv->buffer].bytes != nullptr &&
                bv->byte_offset + bv->byte_length <= bufs[bv->buffer].len &&
                bv->byte_length > 0u) {
                uint8_t *copy = malloc(bv->byte_length);
                if (copy != nullptr) {
                    memcpy(copy, bufs[bv->buffer].bytes + bv->byte_offset, bv->byte_length);
                    out->images[ii].encoded     = copy;
                    out->images[ii].encoded_len = bv->byte_length;
                }
            }
        } else if (uri != nullptr && strncmp(uri, "data:", 5u) == 0) {
            const char *comma = strchr(uri, ',');
            if (comma != nullptr) {
                size_t dlen = 0u;
                out->images[ii].encoded     = gltf_decode_base64(comma + 1, strlen(comma + 1), &dlen);
                out->images[ii].encoded_len = dlen;
            }
        } else if (uri != nullptr && uri[0] != '\0') {
            gltf_resolve_uri_path(base_dir, uri, out->images[ii].uri_path, sizeof out->images[ii].uri_path);
        }
    }

    // 5. Samplers & Textures
    const JsonVal *j_samps = json_obj_get(root, "samplers");
    const uint32_t samp_count = (j_samps != nullptr && j_samps->type == JSON_ARRAY) ? j_samps->child_count : 0u;
    GltfSamplerInfo *samplers = samp_count > 0u ? calloc(samp_count, sizeof(*samplers)) : nullptr;
    uint32_t si = 0u;
    for (const JsonVal *s = j_samps != nullptr ? j_samps->first_child : nullptr;
         s != nullptr && si < samp_count; s = s->next_sibling, ++si) {
        const int32_t ws  = json_as_i32(json_obj_get(s, "wrapS"), (int32_t)k_wrap_repeat);
        const int32_t wt  = json_as_i32(json_obj_get(s, "wrapT"), (int32_t)k_wrap_repeat);
        const int32_t mag = json_as_i32(json_obj_get(s, "magFilter"), 9729);
        const int32_t min = json_as_i32(json_obj_get(s, "minFilter"), 9987);
        samplers[si] = (GltfSamplerInfo){
            .wrap_s = gltf_wrap_to_engine(ws),
            .wrap_t = gltf_wrap_to_engine(wt),
            .filter = gltf_filter_to_engine(mag, min),
        };
    }

    const JsonVal *j_texs = json_obj_get(root, "textures");
    const uint32_t tex_count = (j_texs != nullptr && j_texs->type == JSON_ARRAY) ? j_texs->child_count : 0u;
    GltfTexRef *tex_refs = tex_count > 0u ? calloc(tex_count, sizeof(*tex_refs)) : nullptr;
    uint32_t ti = 0u;
    for (const JsonVal *t = j_texs != nullptr ? j_texs->first_child : nullptr;
         t != nullptr && ti < tex_count; t = t->next_sibling, ++ti) {
        int32_t src = json_as_i32(json_obj_get(t, "source"), -1);
        if (src < 0) {
            const JsonVal *ext_webp = json_obj_get(json_obj_get(t, "extensions"), "EXT_texture_webp");
            src = json_as_i32(json_obj_get(ext_webp, "source"), -1);
        }
        tex_refs[ti] = (GltfTexRef){
            .source  = src,
            .sampler = json_as_i32(json_obj_get(t, "sampler"), -1),
        };
    }

    // 6. Materials (Core PBR + Blender Principled BSDF extensions)
    const JsonVal *j_mats = json_obj_get(root, "materials");
    const uint32_t mat_count = (j_mats != nullptr && j_mats->type == JSON_ARRAY) ? j_mats->child_count : 0u;
    if (mat_count > 0u) {
        out->materials      = calloc(mat_count, sizeof(*out->materials));
        out->material_count = out->materials != nullptr ? mat_count : 0u;
    }
    uint32_t mi = 0u;
    for (const JsonVal *m = j_mats != nullptr ? j_mats->first_child : nullptr;
         m != nullptr && mi < out->material_count; m = m->next_sibling, ++mi) {
        GltfMaterial *gm = &out->materials[mi];
        snprintf(gm->name, sizeof gm->name, "%s", json_as_str(json_obj_get(m, "name"), "Material"));
        gm->base_color[0] = gm->base_color[1] = gm->base_color[2] = gm->base_color[3] = 1.0f;
        gm->metallic                 = 1.0f;
        gm->roughness                = 1.0f;
        gm->normal_scale             = 1.0f;
        gm->occlusion_strength       = 1.0f;
        gm->alpha_cutoff             = 0.5f;
        gm->ior                      = 1.5f;
        gm->base_tex.image_index     = -1;
        gm->mr_tex.image_index       = -1;
        gm->normal_tex.image_index   = -1;
        gm->occ_tex.image_index      = -1;
        gm->emissive_tex.image_index = -1;

        const JsonVal *pbr = json_obj_get(m, "pbrMetallicRoughness");
        if (pbr != nullptr && pbr->type == JSON_OBJECT) {
            (void)json_read_f32_arr(json_obj_get(pbr, "baseColorFactor"), gm->base_color, 4u);
            gm->metallic  = (float)json_as_num(json_obj_get(pbr, "metallicFactor"),  1.0);
            gm->roughness = (float)json_as_num(json_obj_get(pbr, "roughnessFactor"), 1.0);
            parse_texture_info(json_obj_get(pbr, "baseColorTexture"),
                               tex_refs, tex_count, samplers, samp_count, &gm->base_tex);
            parse_texture_info(json_obj_get(pbr, "metallicRoughnessTexture"),
                               tex_refs, tex_count, samplers, samp_count, &gm->mr_tex);
            if (gm->base_tex.image_index >= 0) gm->flags |= PBR_MAT_FLAG_HAS_BASE_TEX;
            if (gm->mr_tex.image_index   >= 0) gm->flags |= PBR_MAT_FLAG_HAS_MR_TEX;
        }

        const JsonVal *nt = json_obj_get(m, "normalTexture");
        if (nt != nullptr && nt->type == JSON_OBJECT) {
            parse_texture_info(nt, tex_refs, tex_count, samplers, samp_count, &gm->normal_tex);
            gm->normal_scale = (float)json_as_num(json_obj_get(nt, "scale"), 1.0);
            if (gm->normal_tex.image_index >= 0) gm->flags |= PBR_MAT_FLAG_HAS_NORMAL;
        }

        const JsonVal *ot = json_obj_get(m, "occlusionTexture");
        if (ot != nullptr && ot->type == JSON_OBJECT) {
            parse_texture_info(ot, tex_refs, tex_count, samplers, samp_count, &gm->occ_tex);
            gm->occlusion_strength = (float)json_as_num(json_obj_get(ot, "strength"), 1.0);
            if (gm->occ_tex.image_index >= 0) gm->flags |= PBR_MAT_FLAG_HAS_OCC;
        }

        const JsonVal *et = json_obj_get(m, "emissiveTexture");
        if (et != nullptr && et->type == JSON_OBJECT) {
            parse_texture_info(et, tex_refs, tex_count, samplers, samp_count, &gm->emissive_tex);
            if (gm->emissive_tex.image_index >= 0) gm->flags |= PBR_MAT_FLAG_HAS_EMI_TEX;
        }
        (void)json_read_f32_arr(json_obj_get(m, "emissiveFactor"), gm->emissive, 3u);

        if (json_as_bool(json_obj_get(m, "doubleSided"), false)) {
            gm->flags |= PBR_MAT_FLAG_DOUBLE_SIDED;
        }
        const char *amode = json_as_str(json_obj_get(m, "alphaMode"), "OPAQUE");
        if (strcmp(amode, "MASK") == 0) {
            gm->flags |= PBR_MAT_FLAG_ALPHA_MASK;
            gm->alpha_cutoff = (float)json_as_num(json_obj_get(m, "alphaCutoff"), 0.5);
        } else if (strcmp(amode, "BLEND") == 0) {
            gm->flags |= PBR_MAT_FLAG_ALPHA_BLEND;
        }

        const JsonVal *exts = json_obj_get(m, "extensions");
        if (exts != nullptr && exts->type == JSON_OBJECT) {
            const JsonVal *es = json_obj_get(exts, "KHR_materials_emissive_strength");
            if (es != nullptr && es->type == JSON_OBJECT) {
                const float strength = (float)json_as_num(json_obj_get(es, "emissiveStrength"), 1.0);
                if (strength >= 0.0f && isfinite(strength)) {
                    gm->emissive[0] *= strength;
                    gm->emissive[1] *= strength;
                    gm->emissive[2] *= strength;
                }
            }
            const JsonVal *ior_ext = json_obj_get(exts, "KHR_materials_ior");
            if (ior_ext != nullptr && ior_ext->type == JSON_OBJECT) {
                const float ior = (float)json_as_num(json_obj_get(ior_ext, "ior"), 1.5);
                if (ior >= 1.0f && ior <= 4.0f) {
                    gm->ior = ior;
                }
            }
            if (json_obj_get(exts, "KHR_materials_unlit") != nullptr) {
                gm->flags |= PBR_MAT_FLAG_UNLIT;
            }
        }
    }

    // 7. Meshes & Primitives
    const JsonVal *j_meshes = json_obj_get(root, "meshes");
    const uint32_t mesh_count = (j_meshes != nullptr && j_meshes->type == JSON_ARRAY) ? j_meshes->child_count : 0u;
    uint32_t total_prims = 0u;
    for (const JsonVal *jm = j_meshes != nullptr ? j_meshes->first_child : nullptr;
         jm != nullptr; jm = jm->next_sibling) {
        const JsonVal *jps = json_obj_get(jm, "primitives");
        if (jps != nullptr && jps->type == JSON_ARRAY) {
            total_prims += jps->child_count;
        }
    }
    if (mesh_count > 0u) {
        out->meshes     = calloc(mesh_count, sizeof(*out->meshes));
        out->mesh_count = out->meshes != nullptr ? mesh_count : 0u;
    }
    if (total_prims > 0u) {
        out->prims      = calloc(total_prims, sizeof(*out->prims));
        out->prim_count = out->prims != nullptr ? total_prims : 0u;
    }

    uint32_t m_idx = 0u;
    uint32_t p_idx = 0u;
    for (const JsonVal *jm = j_meshes != nullptr ? j_meshes->first_child : nullptr;
         jm != nullptr && m_idx < out->mesh_count; jm = jm->next_sibling, ++m_idx) {
        GltfMesh *gm = &out->meshes[m_idx];
        snprintf(gm->name, sizeof gm->name, "%s", json_as_str(json_obj_get(jm, "name"), "Mesh"));
        gm->first_prim = p_idx;
        gm->prim_count = 0u;

        const JsonVal *jps = json_obj_get(jm, "primitives");
        for (const JsonVal *jp = jps != nullptr ? jps->first_child : nullptr;
             jp != nullptr && p_idx < out->prim_count; jp = jp->next_sibling) {
            const int32_t mode = json_as_i32(json_obj_get(jp, "mode"), 4);
            if (mode != 4 && mode != 5 && mode != 6) {
                continue;
            }
            const JsonVal *attrs  = json_obj_get(jp, "attributes");
            const int32_t  pos_ai = json_as_i32(json_obj_get(attrs, "POSITION"), -1);
            if (pos_ai < 0 || (uint32_t)pos_ai >= acc_count || accs[pos_ai].num_comps != 3u) {
                continue;
            }
            const uint32_t vcount = accs[pos_ai].count;
            float *pos = gltf_decode_accessor_f32(&accs[pos_ai], bvs, bv_count, bufs, buf_count);
            if (pos == nullptr || vcount < 3u) {
                free(pos);
                continue;
            }

            const int32_t nrm_ai = json_as_i32(json_obj_get(attrs, "NORMAL"), -1);
            const int32_t tan_ai = json_as_i32(json_obj_get(attrs, "TANGENT"), -1);
            const int32_t uv_ai  = json_as_i32(json_obj_get(attrs, "TEXCOORD_0"), -1);
            const int32_t col_ai = json_as_i32(json_obj_get(attrs, "COLOR_0"), -1);
            const int32_t idx_ai = json_as_i32(json_obj_get(jp, "indices"), -1);

            float *nrm = (nrm_ai >= 0 && (uint32_t)nrm_ai < acc_count && accs[nrm_ai].num_comps == 3u)
                ? gltf_decode_accessor_f32(&accs[nrm_ai], bvs, bv_count, bufs, buf_count) : nullptr;
            float *tan = (tan_ai >= 0 && (uint32_t)tan_ai < acc_count && accs[tan_ai].num_comps == 4u)
                ? gltf_decode_accessor_f32(&accs[tan_ai], bvs, bv_count, bufs, buf_count) : nullptr;
            float *uv  = (uv_ai  >= 0 && (uint32_t)uv_ai  < acc_count && accs[uv_ai].num_comps  == 2u)
                ? gltf_decode_accessor_f32(&accs[uv_ai],  bvs, bv_count, bufs, buf_count) : nullptr;
            const uint32_t col_comps = (col_ai >= 0 && (uint32_t)col_ai < acc_count) ? accs[col_ai].num_comps : 0u;
            float *col = (col_comps == 3u || col_comps == 4u)
                ? gltf_decode_accessor_f32(&accs[col_ai], bvs, bv_count, bufs, buf_count) : nullptr;

            uint32_t  icount  = vcount;
            uint32_t *raw_idx = nullptr;
            if (idx_ai >= 0 && (uint32_t)idx_ai < acc_count) {
                raw_idx = gltf_decode_accessor_indices(&accs[idx_ai], bvs, bv_count, bufs, buf_count);
                if (raw_idx != nullptr) {
                    icount = accs[idx_ai].count;
                }
            }

            uint32_t tri_count = 0u;
            if (mode == 4) {
                tri_count = icount / 3u;
            } else if (icount >= 3u) {
                tri_count = icount - 2u;
            }
            if (tri_count == 0u) {
                free(pos); free(nrm); free(tan); free(uv); free(col); free(raw_idx);
                continue;
            }

            ScenePbrCorner *corners = calloc((size_t)tri_count * 3u, sizeof(*corners));
            if (corners == nullptr) {
                free(pos); free(nrm); free(tan); free(uv); free(col); free(raw_idx);
                continue;
            }

            float lmin[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
            float lmax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
            for (uint32_t t = 0u; t < tri_count; ++t) {
                uint32_t src_i[3];
                if (mode == 4) {
                    src_i[0] = t * 3u + 0u; src_i[1] = t * 3u + 1u; src_i[2] = t * 3u + 2u;
                } else if (mode == 5) {
                    if ((t & 1u) == 0u) {
                        src_i[0] = t; src_i[1] = t + 1u; src_i[2] = t + 2u;
                    } else {
                        src_i[0] = t + 1u; src_i[1] = t; src_i[2] = t + 2u;
                    }
                } else {
                    src_i[0] = 0u; src_i[1] = t + 1u; src_i[2] = t + 2u;
                }

                uint32_t vi[3];
                for (int k = 0; k < 3; ++k) {
                    uint32_t idx = raw_idx != nullptr ? raw_idx[src_i[k]] : src_i[k];
                    if (idx >= vcount) idx = 0u;
                    vi[k] = idx;
                }

                float fn[3] = { 0.0f, 1.0f, 0.0f };
                if (nrm == nullptr) {
                    const float *p0 = &pos[vi[0] * 3u];
                    const float *p1 = &pos[vi[1] * 3u];
                    const float *p2 = &pos[vi[2] * 3u];
                    const float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
                    const float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
                    const float cx = e1y * e2z - e1z * e2y;
                    const float cy = e1z * e2x - e1x * e2z;
                    const float cz = e1x * e2y - e1y * e2x;
                    const float l2 = cx * cx + cy * cy + cz * cz;
                    if (l2 > 1.0e-18f) {
                        const float inv = 1.0f / sqrtf(l2);
                        fn[0] = cx * inv; fn[1] = cy * inv; fn[2] = cz * inv;
                    }
                }

                for (int k = 0; k < 3; ++k) {
                    const uint32_t v = vi[k];
                    ScenePbrCorner *c = &corners[t * 3u + (uint32_t)k];
                    c->x = pos[v * 3u + 0u];
                    c->y = pos[v * 3u + 1u];
                    c->z = pos[v * 3u + 2u];
                    if (c->x < lmin[0]) lmin[0] = c->x;
                    if (c->x > lmax[0]) lmax[0] = c->x;
                    if (c->y < lmin[1]) lmin[1] = c->y;
                    if (c->y > lmax[1]) lmax[1] = c->y;
                    if (c->z < lmin[2]) lmin[2] = c->z;
                    if (c->z > lmax[2]) lmax[2] = c->z;

                    if (nrm != nullptr && v < accs[nrm_ai].count) {
                        c->nx = nrm[v * 3u + 0u];
                        c->ny = nrm[v * 3u + 1u];
                        c->nz = nrm[v * 3u + 2u];
                    } else {
                        c->nx = fn[0]; c->ny = fn[1]; c->nz = fn[2];
                    }
                    if (tan != nullptr && v < accs[tan_ai].count) {
                        c->tx = tan[v * 4u + 0u];
                        c->ty = tan[v * 4u + 1u];
                        c->tz = tan[v * 4u + 2u];
                        c->tw = tan[v * 4u + 3u] < 0.0f ? -1.0f : 1.0f;
                    }
                    if (uv != nullptr && v < accs[uv_ai].count) {
                        c->u = uv[v * 2u + 0u];
                        c->v = uv[v * 2u + 1u];
                    }
                    if (col != nullptr && v < accs[col_ai].count) {
                        c->r = col[v * col_comps + 0u];
                        c->g = col[v * col_comps + 1u];
                        c->b = col[v * col_comps + 2u];
                        c->a = (col_comps == 4u) ? col[v * col_comps + 3u] : 1.0f;
                    } else {
                        c->r = 1.0f; c->g = 1.0f; c->b = 1.0f; c->a = 1.0f;
                    }
                }
            }

            const bool has_uvs = (uv != nullptr);
            bool has_tangents  = (tan != nullptr);
            if (!has_tangents && has_uvs) {
                gltf_generate_tangents(corners, tri_count * 3u);
                has_tangents = true;
            }

            out->prims[p_idx] = (GltfPrimitive){
                .corners        = corners,
                .corner_count   = tri_count * 3u,
                .material_index = json_as_i32(json_obj_get(jp, "material"), -1),
                .local_min      = { lmin[0], lmin[1], lmin[2] },
                .local_max      = { lmax[0], lmax[1], lmax[2] },
                .has_uvs        = has_uvs,
                .has_tangents   = has_tangents,
            };
            p_idx++;
            gm->prim_count++;

            free(pos); free(nrm); free(tan); free(uv); free(col); free(raw_idx);
        }
    }
    out->prim_count = p_idx;

    // 8. Scene Graph Nodes & Parent-Child Hierarchy
    const JsonVal *j_nodes = json_obj_get(root, "nodes");
    const uint32_t node_count = (j_nodes != nullptr && j_nodes->type == JSON_ARRAY) ? j_nodes->child_count : 0u;
    if (node_count > 0u) {
        out->nodes      = calloc(node_count, sizeof(*out->nodes));
        out->node_count = out->nodes != nullptr ? node_count : 0u;
    }
    for (uint32_t i = 0u; i < out->node_count; ++i) {
        out->nodes[i].parent_index = -1;
        out->nodes[i].mesh_index   = -1;
        out->nodes[i].rotation[3]  = 1.0f;
        out->nodes[i].scale[0] = out->nodes[i].scale[1] = out->nodes[i].scale[2] = 1.0f;
    }

    uint32_t ni = 0u;
    for (const JsonVal *jn = j_nodes != nullptr ? j_nodes->first_child : nullptr;
         jn != nullptr && ni < out->node_count; jn = jn->next_sibling, ++ni) {
        GltfNode *gn = &out->nodes[ni];
        const char *nname = json_as_str(json_obj_get(jn, "name"), nullptr);
        if (nname != nullptr && nname[0] != '\0') {
            snprintf(gn->name, sizeof gn->name, "%s", nname);
        } else {
            snprintf(gn->name, sizeof gn->name, "Node_%u", ni);
        }
        gn->mesh_index = json_as_i32(json_obj_get(jn, "mesh"), -1);

        const JsonVal *jmat = json_obj_get(jn, "matrix");
        float mat16[16];
        if (json_read_f32_arr(jmat, mat16, 16u)) {
            gltf_decompose_colmajor_mat4(mat16, gn->translation, gn->rotation, gn->scale, &gn->negative_det);
        } else {
            (void)json_read_f32_arr(json_obj_get(jn, "translation"), gn->translation, 3u);
            if (json_read_f32_arr(json_obj_get(jn, "rotation"), gn->rotation, 4u)) {
                ecs_quat_norm(gn->rotation);
            }
            (void)json_read_f32_arr(json_obj_get(jn, "scale"), gn->scale, 3u);
            gn->negative_det = (gn->scale[0] * gn->scale[1] * gn->scale[2]) < 0.0f;
        }

        const JsonVal *jch = json_obj_get(jn, "children");
        for (const JsonVal *ch = jch != nullptr ? jch->first_child : nullptr;
             ch != nullptr; ch = ch->next_sibling) {
            const int32_t cid = json_as_i32(ch, -1);
            if (cid >= 0 && (uint32_t)cid < out->node_count && (uint32_t)cid != ni) {
                out->nodes[cid].parent_index = (int32_t)ni;
            }
        }
    }

    const JsonVal *j_scenes = json_obj_get(root, "scenes");
    if (j_scenes != nullptr && j_scenes->type == JSON_ARRAY && j_scenes->child_count > 1u &&
        out->node_count > 0u) {
        int32_t def_sc = json_as_i32(json_obj_get(root, "scene"), 0);
        if (def_sc < 0 || (uint32_t)def_sc >= j_scenes->child_count) {
            def_sc = 0;
        }
        const JsonVal *sc       = json_arr_at(j_scenes, (uint32_t)def_sc);
        const JsonVal *sc_nodes = json_obj_get(sc, "nodes");
        if (sc_nodes != nullptr && sc_nodes->type == JSON_ARRAY && sc_nodes->child_count > 0u) {
            uint8_t *active = calloc(out->node_count, 1u);
            if (active != nullptr) {
                for (const JsonVal *rn = sc_nodes->first_child; rn != nullptr; rn = rn->next_sibling) {
                    const int32_t ridx = json_as_i32(rn, -1);
                    if (ridx >= 0 && (uint32_t)ridx < out->node_count) {
                        active[ridx] = 1u;
                    }
                }
                for (uint32_t pass = 0u; pass < out->node_count; ++pass) {
                    bool changed = false;
                    for (uint32_t i = 0u; i < out->node_count; ++i) {
                        const int32_t p = out->nodes[i].parent_index;
                        if (active[i] == 0u && p >= 0 && active[p] != 0u) {
                            active[i] = 1u;
                            changed = true;
                        }
                    }
                    if (!changed) break;
                }
                for (uint32_t i = 0u; i < out->node_count; ++i) {
                    if (active[i] == 0u) {
                        out->nodes[i].mesh_index = -1;
                    }
                }
                free(active);
            }
        }
    }

    gltf_compute_scene_zup_bounds(out);

    free(tex_refs);
    free(samplers);
    free(accs);
    free(bvs);
    for (uint32_t i = 0u; i < buf_count; ++i) {
        if (bufs[i].owned) {
            free(bufs[i].bytes);
        }
    }
    free(bufs);
    return 0;
}

int gltf_load_glb_memory(const uint8_t *bytes, size_t len,
                         const char *base_dir, GltfAsset *out_asset) {
    if (bytes == nullptr || len < 20u || out_asset == nullptr) {
        return -1;
    }
    *out_asset = (GltfAsset){0};

    const uint32_t magic   = read_u32_le(bytes + 0u);
    const uint32_t version = read_u32_le(bytes + 4u);
    const uint32_t glb_len = read_u32_le(bytes + 8u);
    if (magic != k_glb_magic || version != 2u || glb_len > len) {
        fprintf(stderr, "[glTF] Invalid GLB header (magic=0x%08x ver=%u)\n", magic, version);
        return -1;
    }

    const char    *json_ptr = nullptr;
    size_t         json_len = 0u;
    const uint8_t *bin_ptr  = nullptr;
    size_t         bin_len  = 0u;

    size_t pos = 12u;
    while (pos + 8u <= glb_len) {
        const uint32_t chunk_len  = read_u32_le(bytes + pos);
        const uint32_t chunk_type = read_u32_le(bytes + pos + 4u);
        pos += 8u;
        if (pos + chunk_len > glb_len) {
            break;
        }
        if (chunk_type == k_glb_chunk_json && json_ptr == nullptr) {
            json_ptr = (const char *)(bytes + pos);
            json_len = chunk_len;
        } else if (chunk_type == k_glb_chunk_bin && bin_ptr == nullptr) {
            bin_ptr = bytes + pos;
            bin_len = chunk_len;
        }
        pos += chunk_len;
    }
    if (json_ptr == nullptr || json_len == 0u) {
        fprintf(stderr, "[glTF] GLB missing JSON chunk\n");
        return -1;
    }

    JsonDoc doc = {0};
    if (json_parse(json_ptr, json_len, &doc) != 0) {
        fprintf(stderr, "[glTF] Failed to parse GLB JSON\n");
        return -1;
    }
    int rc = parse_gltf_doc(doc.root, bin_ptr, bin_len, base_dir, out_asset);
    json_doc_free(&doc);
    if (rc != 0) {
        gltf_asset_free(out_asset);
    }
    return rc;
}

static bool is_blend_bytes_or_ext(const char *path, const uint8_t *bytes, size_t len) {
    if (has_ext_ci(path, ".blend") || has_ext_ci(path, ".blend1")) {
        return true;
    }
    if (len >= 7u && memcmp(bytes, "BLENDER", 7u) == 0) {
        return true;
    }
    if (len >= 4u &&
        ((bytes[0] == 0x1Fu && bytes[1] == 0x8Bu) ||
         (bytes[0] == 0x28u && bytes[1] == 0xB5u && bytes[2] == 0x2Fu && bytes[3] == 0xFDu))) {
        return true;
    }
    return false;
}

int gltf_load_file(const char *path, GltfAsset *out_asset) {
    if (path == nullptr || path[0] == '\0' || out_asset == nullptr) {
        return -1;
    }
    *out_asset = (GltfAsset){0};

    uint8_t hdr[12] = {0};
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        fprintf(stderr, "[glTF] Cannot open '%s'\n", path);
        return -1;
    }
    const size_t hdr_got = fread(hdr, 1u, sizeof hdr, fp);
    fclose(fp);

    const char *load_path = path;
    char cached_glb[512] = {0};
    bool from_blend = false;

    if (is_blend_bytes_or_ext(path, hdr, hdr_got)) {
        if (gltf_blend_export_to_glb(path, cached_glb, sizeof cached_glb) != 0) {
            return -1;
        }
        load_path  = cached_glb;
        from_blend = true;
    }

    size_t   len   = 0u;
    uint8_t *bytes = slurp_file_bytes(load_path, &len);
    if (bytes == nullptr || len < 4u) {
        free(bytes);
        fprintf(stderr, "[glTF] Failed to read '%s'\n", load_path);
        return -1;
    }

    char base_dir[512];
    extract_dir(load_path, base_dir, sizeof base_dir);

    int rc = -1;
    if (len >= 20u && read_u32_le(bytes) == k_glb_magic) {
        rc = gltf_load_glb_memory(bytes, len, base_dir, out_asset);
    } else {
        JsonDoc doc = {0};
        if (json_parse((const char *)bytes, len, &doc) == 0) {
            rc = parse_gltf_doc(doc.root, nullptr, 0u, base_dir, out_asset);
            json_doc_free(&doc);
        } else {
            fprintf(stderr, "[glTF] Failed to parse JSON from '%s'\n", load_path);
        }
    }
    free(bytes);

    if (rc != 0) {
        gltf_asset_free(out_asset);
        return -1;
    }

    snprintf(out_asset->source_path, sizeof out_asset->source_path, "%s", path);
    if (from_blend) {
        snprintf(out_asset->resolved_glb, sizeof out_asset->resolved_glb, "%s", cached_glb);
        out_asset->from_blend = true;
    }
    return 0;
}

void gltf_asset_free(GltfAsset *asset) {
    if (asset == nullptr) {
        return;
    }
    if (asset->prims != nullptr) {
        for (uint32_t i = 0u; i < asset->prim_count; ++i) {
            free(asset->prims[i].corners);
        }
        free(asset->prims);
    }
    if (asset->images != nullptr) {
        for (uint32_t i = 0u; i < asset->image_count; ++i) {
            free(asset->images[i].encoded);
        }
        free(asset->images);
    }
    free(asset->nodes);
    free(asset->meshes);
    free(asset->materials);
    *asset = (GltfAsset){0};
}
