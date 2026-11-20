#include "gltf/gltf_int.h"
#include "ecs/ecs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gltf_yup_to_zup_quat(float out_q[4]) {
    if (out_q == nullptr) {
        return;
    }
    // +90 degrees around +X maps (x, y, z)_gltf -> (x, -z, y)_engine (Right-Handed Z-Up).
    static constexpr float k_s = 0.7071067811865475244f;
    out_q[0] = k_s;
    out_q[1] = 0.0f;
    out_q[2] = 0.0f;
    out_q[3] = k_s;
}

void gltf_generate_tangents(ScenePbrCorner *corners, uint32_t corner_count) {
    if (corners == nullptr || corner_count < 3u) {
        return;
    }
    const uint32_t tri_count = corner_count / 3u;
    for (uint32_t t = 0u; t < tri_count; ++t) {
        ScenePbrCorner *c0 = &corners[t * 3u + 0u];
        ScenePbrCorner *c1 = &corners[t * 3u + 1u];
        ScenePbrCorner *c2 = &corners[t * 3u + 2u];

        const float x1 = c1->x - c0->x, y1 = c1->y - c0->y, z1 = c1->z - c0->z;
        const float x2 = c2->x - c0->x, y2 = c2->y - c0->y, z2 = c2->z - c0->z;
        const float s1 = c1->u - c0->u, t1 = c1->v - c0->v;
        const float s2 = c2->u - c0->u, t2 = c2->v - c0->v;

        const float denom = s1 * t2 - s2 * t1;
        float tx = 1.0f, ty = 0.0f, tz = 0.0f;
        float bx = 0.0f, by = 1.0f, bz = 0.0f;
        const bool valid_uv = fabsf(denom) > 1.0e-10f;
        if (valid_uv) {
            const float r = 1.0f / denom;
            tx = (t2 * x1 - t1 * x2) * r;
            ty = (t2 * y1 - t1 * y2) * r;
            tz = (t2 * z1 - t1 * z2) * r;
            bx = (s1 * x2 - s2 * x1) * r;
            by = (s1 * y2 - s2 * y1) * r;
            bz = (s1 * z2 - s2 * z1) * r;
        }

        ScenePbrCorner *tri[3] = { c0, c1, c2 };
        for (int k = 0; k < 3; ++k) {
            ScenePbrCorner *v = tri[k];
            const float nx = v->nx, ny = v->ny, nz = v->nz;
            float ox, oy, oz;
            if (valid_uv) {
                const float ndt = nx * tx + ny * ty + nz * tz;
                ox = tx - nx * ndt;
                oy = ty - ny * ndt;
                oz = tz - nz * ndt;
            } else {
                const float ax = fabsf(nx) < 0.9f ? 1.0f : 0.0f;
                const float ay = fabsf(nx) < 0.9f ? 0.0f : 1.0f;
                const float az = 0.0f;
                ox = ny * az - nz * ay;
                oy = nz * ax - nx * az;
                oz = nx * ay - ny * ax;
            }
            const float l2 = ox * ox + oy * oy + oz * oz;
            if (l2 > 1.0e-12f) {
                const float inv = 1.0f / sqrtf(l2);
                ox *= inv;
                oy *= inv;
                oz *= inv;
            } else {
                ox = 1.0f;
                oy = 0.0f;
                oz = 0.0f;
            }
            const float cx = ny * oz - nz * oy;
            const float cy = nz * ox - nx * oz;
            const float cz = nx * oy - ny * ox;
            v->tx = ox;
            v->ty = oy;
            v->tz = oz;
            v->tw = valid_uv ? ((cx * bx + cy * by + cz * bz) < 0.0f ? -1.0f : 1.0f) : 1.0f;
        }
    }
}

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint8_t *gltf_decode_base64(const char *src, size_t len, size_t *out_len) {
    if (src == nullptr || out_len == nullptr) {
        return nullptr;
    }
    uint8_t *dst = malloc((len * 3u) / 4u + 4u);
    if (dst == nullptr) {
        return nullptr;
    }
    uint32_t acc  = 0u;
    int      bits = 0;
    size_t   out  = 0u;
    for (size_t i = 0u; i < len; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c == '=') {
            break;
        }
        const int v = b64_val(c);
        if (v < 0) {
            continue;
        }
        acc = (acc << 6u) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    *out_len = out;
    return dst;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode_path(const char *uri, char *out, size_t cap) {
    size_t w = 0u;
    for (size_t i = 0u; uri[i] != '\0' && w + 1u < cap; ++i) {
        if (uri[i] == '%' && uri[i + 1u] != '\0' && uri[i + 2u] != '\0') {
            int hi = hex_digit(uri[i + 1u]);
            int lo = hex_digit(uri[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                out[w++] = (char)((hi << 4) | lo);
                i += 2u;
                continue;
            }
        }
        out[w++] = uri[i];
    }
    out[w] = '\0';
}

void gltf_resolve_uri_path(const char *base_dir, const char *uri, char *out, size_t cap) {
    char decoded[512];
    url_decode_path(uri, decoded, sizeof decoded);
    if (decoded[0] == '/' || base_dir == nullptr || base_dir[0] == '\0') {
        snprintf(out, cap, "%s", decoded);
    } else {
        snprintf(out, cap, "%s/%s", base_dir, decoded);
    }
}

static uint32_t comp_size(uint32_t ctype) {
    switch (ctype) {
    case K_GLTF_COMP_I8:
    case K_GLTF_COMP_U8:  return 1u;
    case K_GLTF_COMP_I16:
    case K_GLTF_COMP_U16: return 2u;
    case K_GLTF_COMP_U32:
    case K_GLTF_COMP_F32: return 4u;
    default:              return 0u;
    }
}

uint32_t gltf_type_comps(const char *type_str) {
    if (type_str == nullptr) return 0u;
    if (strcmp(type_str, "SCALAR") == 0) return 1u;
    if (strcmp(type_str, "VEC2")   == 0) return 2u;
    if (strcmp(type_str, "VEC3")   == 0) return 3u;
    if (strcmp(type_str, "VEC4")   == 0) return 4u;
    if (strcmp(type_str, "MAT2")   == 0) return 4u;
    if (strcmp(type_str, "MAT3")   == 0) return 9u;
    if (strcmp(type_str, "MAT4")   == 0) return 16u;
    return 0u;
}

static float read_comp_f32(const uint8_t *p, uint32_t ctype, bool normalized) {
    switch (ctype) {
    case K_GLTF_COMP_F32: {
        float v = 0.0f;
        memcpy(&v, p, sizeof v);
        return isfinite(v) ? v : 0.0f;
    }
    case K_GLTF_COMP_I8: {
        int8_t v = (int8_t)p[0];
        if (!normalized) return (float)v;
        float f = (float)v / 127.0f;
        return f < -1.0f ? -1.0f : f;
    }
    case K_GLTF_COMP_U8:
        return normalized ? ((float)p[0] / 255.0f) : (float)p[0];
    case K_GLTF_COMP_I16: {
        int16_t v = 0;
        memcpy(&v, p, sizeof v);
        if (!normalized) return (float)v;
        float f = (float)v / 32767.0f;
        return f < -1.0f ? -1.0f : f;
    }
    case K_GLTF_COMP_U16: {
        uint16_t v = 0;
        memcpy(&v, p, sizeof v);
        return normalized ? ((float)v / 65535.0f) : (float)v;
    }
    case K_GLTF_COMP_U32: {
        uint32_t v = 0;
        memcpy(&v, p, sizeof v);
        return (float)v;
    }
    default:
        return 0.0f;
    }
}

static uint32_t read_comp_u32(const uint8_t *p, uint32_t ctype) {
    switch (ctype) {
    case K_GLTF_COMP_U8:  return (uint32_t)p[0];
    case K_GLTF_COMP_U16: {
        uint16_t v = 0;
        memcpy(&v, p, sizeof v);
        return (uint32_t)v;
    }
    case K_GLTF_COMP_U32: {
        uint32_t v = 0;
        memcpy(&v, p, sizeof v);
        return v;
    }
    default:
        return 0u;
    }
}

float *gltf_decode_accessor_f32(const GltfAccessor *acc,
                                const GltfBufView *bvs, uint32_t bv_count,
                                const GltfBufData *bufs, uint32_t buf_count) {
    if (acc == nullptr || acc->count == 0u || acc->num_comps == 0u) {
        return nullptr;
    }
    const uint32_t csz = comp_size(acc->component_type);
    if (csz == 0u) {
        return nullptr;
    }
    const size_t total_floats = (size_t)acc->count * (size_t)acc->num_comps;
    float *out = calloc(total_floats, sizeof(float));
    if (out == nullptr) {
        return nullptr;
    }

    if (acc->buffer_view >= 0 && (uint32_t)acc->buffer_view < bv_count) {
        const GltfBufView *bv = &bvs[acc->buffer_view];
        if (bv->buffer < buf_count && bufs[bv->buffer].bytes != nullptr) {
            const uint8_t *base   = bufs[bv->buffer].bytes;
            const size_t   blen   = bufs[bv->buffer].len;
            const size_t   elem_b = (size_t)csz * (size_t)acc->num_comps;
            const size_t   stride = bv->byte_stride >= elem_b ? bv->byte_stride : elem_b;
            const size_t   start  = bv->byte_offset + acc->byte_offset;
            for (uint32_t i = 0u; i < acc->count; ++i) {
                const size_t off = start + (size_t)i * stride;
                if (off + elem_b > blen) {
                    break;
                }
                for (uint32_t c = 0u; c < acc->num_comps; ++c) {
                    out[(size_t)i * acc->num_comps + c] =
                        read_comp_f32(base + off + (size_t)c * csz, acc->component_type, acc->normalized);
                }
            }
        }
    }

    if (acc->has_sparse && acc->sparse_count > 0u &&
        acc->sparse_idx_bv >= 0 && (uint32_t)acc->sparse_idx_bv < bv_count &&
        acc->sparse_val_bv >= 0 && (uint32_t)acc->sparse_val_bv < bv_count) {
        const GltfBufView *ibv  = &bvs[acc->sparse_idx_bv];
        const GltfBufView *vbv  = &bvs[acc->sparse_val_bv];
        const uint32_t     icsz = comp_size(acc->sparse_idx_ctype);
        if (icsz > 0u && ibv->buffer < buf_count && vbv->buffer < buf_count &&
            bufs[ibv->buffer].bytes != nullptr && bufs[vbv->buffer].bytes != nullptr) {
            const uint8_t *ibase  = bufs[ibv->buffer].bytes;
            const size_t   ilen   = bufs[ibv->buffer].len;
            const uint8_t *vbase  = bufs[vbv->buffer].bytes;
            const size_t   vlen   = bufs[vbv->buffer].len;
            const size_t   velem  = (size_t)csz * (size_t)acc->num_comps;
            const size_t   istart = ibv->byte_offset + acc->sparse_idx_off;
            const size_t   vstart = vbv->byte_offset + acc->sparse_val_off;
            for (uint32_t s = 0u; s < acc->sparse_count; ++s) {
                const size_t ioff = istart + (size_t)s * icsz;
                const size_t voff = vstart + (size_t)s * velem;
                if (ioff + icsz > ilen || voff + velem > vlen) {
                    break;
                }
                const uint32_t dst_i = read_comp_u32(ibase + ioff, acc->sparse_idx_ctype);
                if (dst_i >= acc->count) {
                    continue;
                }
                for (uint32_t c = 0u; c < acc->num_comps; ++c) {
                    out[(size_t)dst_i * acc->num_comps + c] =
                        read_comp_f32(vbase + voff + (size_t)c * csz, acc->component_type, acc->normalized);
                }
            }
        }
    }

    return out;
}

uint32_t *gltf_decode_accessor_indices(const GltfAccessor *acc,
                                       const GltfBufView *bvs, uint32_t bv_count,
                                       const GltfBufData *bufs, uint32_t buf_count) {
    if (acc == nullptr || acc->count == 0u || acc->num_comps != 1u ||
        acc->buffer_view < 0 || (uint32_t)acc->buffer_view >= bv_count) {
        return nullptr;
    }
    const uint32_t csz = comp_size(acc->component_type);
    if (csz == 0u) {
        return nullptr;
    }
    const GltfBufView *bv = &bvs[acc->buffer_view];
    if (bv->buffer >= buf_count || bufs[bv->buffer].bytes == nullptr) {
        return nullptr;
    }
    uint32_t *out = calloc(acc->count, sizeof(uint32_t));
    if (out == nullptr) {
        return nullptr;
    }
    const uint8_t *base   = bufs[bv->buffer].bytes;
    const size_t   blen   = bufs[bv->buffer].len;
    const size_t   stride = bv->byte_stride >= csz ? bv->byte_stride : csz;
    const size_t   start  = bv->byte_offset + acc->byte_offset;
    for (uint32_t i = 0u; i < acc->count; ++i) {
        const size_t off = start + (size_t)i * stride;
        if (off + csz > blen) {
            break;
        }
        out[i] = read_comp_u32(base + off, acc->component_type);
    }
    return out;
}

void gltf_decompose_colmajor_mat4(const float cm[16],
                                  float out_t[3], float out_q[4], float out_s[3],
                                  bool *out_neg_det) {
    out_t[0] = cm[12];
    out_t[1] = cm[13];
    out_t[2] = cm[14];

    const float c0[3] = { cm[0], cm[1], cm[2] };
    const float c1[3] = { cm[4], cm[5], cm[6] };
    const float c2[3] = { cm[8], cm[9], cm[10] };

    float sx = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]);
    float sy = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    float sz = sqrtf(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]);

    const float crx = c1[1] * c2[2] - c1[2] * c2[1];
    const float cry = c1[2] * c2[0] - c1[0] * c2[2];
    const float crz = c1[0] * c2[1] - c1[1] * c2[0];
    const float det = c0[0] * crx + c0[1] * cry + c0[2] * crz;
    const bool  neg = det < 0.0f;
    if (neg) {
        sx = -sx;
    }
    out_s[0] = sx;
    out_s[1] = sy;
    out_s[2] = sz;
    if (out_neg_det != nullptr) {
        *out_neg_det = neg;
    }

    float r[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
    if (fabsf(sx) > 1.0e-7f) { r[0] = c0[0] / sx; r[3] = c0[1] / sx; r[6] = c0[2] / sx; }
    if (fabsf(sy) > 1.0e-7f) { r[1] = c1[0] / sy; r[4] = c1[1] / sy; r[7] = c1[2] / sy; }
    if (fabsf(sz) > 1.0e-7f) { r[2] = c2[0] / sz; r[5] = c2[1] / sz; r[8] = c2[2] / sz; }
    ecs_mat3_to_quat(r, out_q);
    ecs_quat_norm(out_q);
}

void gltf_compute_scene_zup_bounds(GltfAsset *asset) {
    asset->bounds_min[0] =  1.0e30f;
    asset->bounds_min[1] =  1.0e30f;
    asset->bounds_min[2] =  1.0e30f;
    asset->bounds_max[0] = -1.0e30f;
    asset->bounds_max[1] = -1.0e30f;
    asset->bounds_max[2] = -1.0e30f;

    if (asset->node_count == 0u) {
        asset->bounds_min[0] = asset->bounds_min[1] = asset->bounds_min[2] = 0.0f;
        asset->bounds_max[0] = asset->bounds_max[1] = asset->bounds_max[2] = 0.0f;
        return;
    }

    float (*world_m)[12] = calloc(asset->node_count, sizeof(*world_m));
    uint8_t *done = calloc(asset->node_count, sizeof(*done));
    if (world_m == nullptr || done == nullptr) {
        free(world_m);
        free(done);
        return;
    }

    float q_root[4];
    gltf_yup_to_zup_quat(q_root);
    const float z_pos[3] = { 0.0f, 0.0f, 0.0f };
    const float u_scl[3] = { 1.0f, 1.0f, 1.0f };
    float root_m[12];
    ecs_mat3x4_from_trs(z_pos, q_root, u_scl, root_m);

    for (uint32_t pass = 0u; pass < asset->node_count; ++pass) {
        bool any = false;
        for (uint32_t i = 0u; i < asset->node_count; ++i) {
            if (done[i] != 0u) {
                continue;
            }
            const GltfNode *n = &asset->nodes[i];
            int32_t p = n->parent_index;
            if (p >= 0 && (uint32_t)p < asset->node_count && done[p] == 0u) {
                continue;
            }
            float local_m[12];
            ecs_mat3x4_from_trs(n->translation, n->rotation, n->scale, local_m);
            if (p >= 0 && (uint32_t)p < asset->node_count) {
                ecs_mat3x4_mul(world_m[p], local_m, world_m[i]);
            } else {
                ecs_mat3x4_mul(root_m, local_m, world_m[i]);
            }
            done[i] = 1u;
            any = true;
        }
        if (!any) {
            break;
        }
    }

    bool any_pt = false;
    for (uint32_t i = 0u; i < asset->node_count; ++i) {
        if (done[i] == 0u) {
            continue;
        }
        const int32_t mid = asset->nodes[i].mesh_index;
        if (mid < 0 || (uint32_t)mid >= asset->mesh_count) {
            continue;
        }
        const GltfMesh *mesh = &asset->meshes[mid];
        for (uint32_t p = 0u; p < mesh->prim_count; ++p) {
            const GltfPrimitive *pr = &asset->prims[mesh->first_prim + p];
            if (pr->corner_count == 0u) {
                continue;
            }
            for (int cx = 0; cx < 2; ++cx) {
                for (int cy = 0; cy < 2; ++cy) {
                    for (int cz = 0; cz < 2; ++cz) {
                        const float lp[3] = {
                            cx ? pr->local_max[0] : pr->local_min[0],
                            cy ? pr->local_max[1] : pr->local_min[1],
                            cz ? pr->local_max[2] : pr->local_min[2],
                        };
                        float wp[3];
                        ecs_mat3x4_transform_point(world_m[i], lp, wp);
                        for (int a = 0; a < 3; ++a) {
                            if (wp[a] < asset->bounds_min[a]) asset->bounds_min[a] = wp[a];
                            if (wp[a] > asset->bounds_max[a]) asset->bounds_max[a] = wp[a];
                        }
                        any_pt = true;
                    }
                }
            }
        }
    }

    if (!any_pt) {
        asset->bounds_min[0] = asset->bounds_min[1] = asset->bounds_min[2] = 0.0f;
        asset->bounds_max[0] = asset->bounds_max[1] = asset->bounds_max[2] = 0.0f;
    }

    free(world_m);
    free(done);
}
