#include "scene/scene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum : uint32_t {
    k_morton_window = 192u,
};

typedef struct TriMeta {
    uint32_t v[3];
    float    cx, cy, cz;
    float    nx, ny, nz;
    uint32_t morton;
    bool     degenerate;
} TriMeta;

typedef struct HashEntry {
    uint32_t vert_idx;
    bool     occupied;
} HashEntry;

static uint32_t expand_bits_10(uint32_t v) {
    v &= 0x3FFu;
    v = (v | (v << 16u)) & 0x030000FFu;
    v = (v | (v <<  8u)) & 0x0300F00Fu;
    v = (v | (v <<  4u)) & 0x030C30C3u;
    v = (v | (v <<  2u)) & 0x09249249u;
    return v;
}

static uint32_t morton3d_unit(float ux, float uy, float uz) {
    if (ux < 0.0f) ux = 0.0f; else if (ux > 1.0f) ux = 1.0f;
    if (uy < 0.0f) uy = 0.0f; else if (uy > 1.0f) uy = 1.0f;
    if (uz < 0.0f) uz = 0.0f; else if (uz > 1.0f) uz = 1.0f;
    uint32_t ix = (uint32_t)(ux * 1023.0f + 0.5f);
    uint32_t iy = (uint32_t)(uy * 1023.0f + 0.5f);
    uint32_t iz = (uint32_t)(uz * 1023.0f + 0.5f);
    return (expand_bits_10(ix) << 2u) | (expand_bits_10(iy) << 1u) | expand_bits_10(iz);
}

static uint64_t hash_corner_bits(const SceneCorner *c) {
    uint32_t words[9];
    memcpy(words, c, sizeof words);
    // Normalize -0.0f to +0.0f so bitwise-identical zero coordinates weld cleanly.
    for (uint32_t i = 0u; i < 9u; ++i) {
        if (words[i] == 0x80000000u) {
            words[i] = 0u;
        }
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0u; i < 9u; ++i) {
        h ^= (uint64_t)words[i];
        h *= 1099511628211ull;
    }
    return h;
}

static bool corners_equal(const SceneCorner *a, const SceneCorner *b) {
    return fabsf(a->x - b->x) <= 1.0e-6f &&
           fabsf(a->y - b->y) <= 1.0e-6f &&
           fabsf(a->z - b->z) <= 1.0e-6f &&
           fabsf(a->nx - b->nx) <= 1.0e-5f &&
           fabsf(a->ny - b->ny) <= 1.0e-5f &&
           fabsf(a->nz - b->nz) <= 1.0e-5f &&
           fabsf(a->r - b->r) <= 1.0e-4f &&
           fabsf(a->g - b->g) <= 1.0e-4f &&
           fabsf(a->b - b->b) <= 1.0e-4f;
}

static int cmp_tri_morton(const void *pa, const void *pb) {
    const TriMeta *a = (const TriMeta *)pa;
    const TriMeta *b = (const TriMeta *)pb;
    if (a->morton < b->morton) {
        return -1;
    }
    if (a->morton > b->morton) {
        return 1;
    }
    return 0;
}

static void compute_face_normal(const float p0[3], const float p1[3], const float p2[3],
                                float out_n[3], bool *out_degen) {
    float e1x = p1[0] - p0[0], e1y = p1[1] - p0[1], e1z = p1[2] - p0[2];
    float e2x = p2[0] - p0[0], e2y = p2[1] - p0[1], e2z = p2[2] - p0[2];
    float cx = e1y * e2z - e1z * e2y;
    float cy = e1z * e2x - e1x * e2z;
    float cz = e1x * e2y - e1y * e2x;
    float len2 = cx * cx + cy * cy + cz * cz;
    if (len2 <= 1.0e-18f) {
        out_n[0] = 0.0f;
        out_n[1] = 1.0f;
        out_n[2] = 0.0f;
        *out_degen = true;
        return;
    }
    float inv = 1.0f / sqrtf(len2);
    out_n[0] = cx * inv;
    out_n[1] = cy * inv;
    out_n[2] = cz * inv;
    *out_degen = false;
}

void scene_meshlet_build_free(SceneMeshletBuild *build) {
    if (build == nullptr) {
        return;
    }
    free(build->meshlets);
    free(build->vertices);
    free(build->triangles);
    *build = (SceneMeshletBuild){0};
}

int scene_build_meshlets(const SceneCorner *corners, uint32_t corner_count,
                         float default_roughness, SceneMeshletBuild *out_build) {
    if (out_build == nullptr) {
        return -1;
    }
    *out_build = (SceneMeshletBuild){0};
    uint32_t tri_total = corner_count / 3u;
    if (corners == nullptr || tri_total == 0u) {
        return 0;
    }

    // 1. Weld shared corners into unique vertices via open-addressed power-of-two hash table.
    uint32_t hash_cap = 64u;
    while (hash_cap < corner_count * 2u) {
        hash_cap <<= 1u;
    }
    HashEntry   *htab      = calloc(hash_cap, sizeof(*htab));
    SceneCorner *uniq_v    = malloc((size_t)corner_count * sizeof(*uniq_v));
    uint32_t    *tri_idx   = malloc((size_t)tri_total * 3u * sizeof(*tri_idx));
    TriMeta     *tris      = malloc((size_t)tri_total * sizeof(*tris));
    if (htab == nullptr || uniq_v == nullptr || tri_idx == nullptr || tris == nullptr) {
        free(htab);
        free(uniq_v);
        free(tri_idx);
        free(tris);
        return -1;
    }

    uint32_t uniq_count = 0u;
    uint32_t mask = hash_cap - 1u;
    for (uint32_t i = 0u; i < tri_total * 3u; ++i) {
        const SceneCorner *c = &corners[i];
        uint32_t slot = (uint32_t)hash_corner_bits(c) & mask;
        uint32_t found = UINT32_MAX;
        while (htab[slot].occupied) {
            uint32_t cand = htab[slot].vert_idx;
            if (corners_equal(&uniq_v[cand], c)) {
                found = cand;
                break;
            }
            slot = (slot + 1u) & mask;
        }
        if (found == UINT32_MAX) {
            found = uniq_count++;
            uniq_v[found] = *c;
            htab[slot].vert_idx = found;
            htab[slot].occupied = true;
        }
        tri_idx[i] = found;
    }
    free(htab);

    // 2. Compute triangle centroids, normals, and 30-bit 3D Morton codes.
    float cmin[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
    float cmax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
    for (uint32_t t = 0u; t < tri_total; ++t) {
        uint32_t i0 = tri_idx[t * 3u + 0u];
        uint32_t i1 = tri_idx[t * 3u + 1u];
        uint32_t i2 = tri_idx[t * 3u + 2u];
        const SceneCorner *v0 = &uniq_v[i0];
        const SceneCorner *v1 = &uniq_v[i1];
        const SceneCorner *v2 = &uniq_v[i2];

        tris[t].v[0] = i0;
        tris[t].v[1] = i1;
        tris[t].v[2] = i2;
        tris[t].cx = (v0->x + v1->x + v2->x) * (1.0f / 3.0f);
        tris[t].cy = (v0->y + v1->y + v2->y) * (1.0f / 3.0f);
        tris[t].cz = (v0->z + v1->z + v2->z) * (1.0f / 3.0f);

        float p0[3] = { v0->x, v0->y, v0->z };
        float p1[3] = { v1->x, v1->y, v1->z };
        float p2[3] = { v2->x, v2->y, v2->z };
        float fn[3] = {0};
        compute_face_normal(p0, p1, p2, fn, &tris[t].degenerate);
        tris[t].nx = fn[0];
        tris[t].ny = fn[1];
        tris[t].nz = fn[2];

        if (tris[t].cx < cmin[0]) cmin[0] = tris[t].cx;
        if (tris[t].cx > cmax[0]) cmax[0] = tris[t].cx;
        if (tris[t].cy < cmin[1]) cmin[1] = tris[t].cy;
        if (tris[t].cy > cmax[1]) cmax[1] = tris[t].cy;
        if (tris[t].cz < cmin[2]) cmin[2] = tris[t].cz;
        if (tris[t].cz > cmax[2]) cmax[2] = tris[t].cz;
    }
    free(tri_idx);

    float span[3] = {
        fmaxf(cmax[0] - cmin[0], 1.0e-6f),
        fmaxf(cmax[1] - cmin[1], 1.0e-6f),
        fmaxf(cmax[2] - cmin[2], 1.0e-6f),
    };
    for (uint32_t t = 0u; t < tri_total; ++t) {
        tris[t].morton = morton3d_unit((tris[t].cx - cmin[0]) / span[0],
                                       (tris[t].cy - cmin[1]) / span[1],
                                       (tris[t].cz - cmin[2]) / span[2]);
    }
    qsort(tris, tri_total, sizeof(*tris), cmp_tri_morton);

    // 3. Greedy spatial + vertex-reuse + normal-aligned 64v / 124t meshlet clusterizer.
    uint32_t max_meshlets = tri_total; // Upper bound (1 tri per meshlet worst case)
    SceneMeshlet      *out_ml = calloc(max_meshlets, sizeof(*out_ml));
    ScenePackedVertex *out_vx = malloc((size_t)tri_total * 3u * sizeof(*out_vx));
    uint32_t          *out_tr = malloc((size_t)tri_total * sizeof(*out_tr));
    uint8_t           *used_t = calloc(tri_total, sizeof(*used_t));
    uint32_t          *v_stamp = calloc(uniq_count, sizeof(*v_stamp));
    uint8_t           *v_local = calloc(uniq_count, sizeof(*v_local));
    if (out_ml == nullptr || out_vx == nullptr || out_tr == nullptr ||
        used_t == nullptr || v_stamp == nullptr || v_local == nullptr) {
        free(out_ml);
        free(out_vx);
        free(out_tr);
        free(used_t);
        free(v_stamp);
        free(v_local);
        free(uniq_v);
        free(tris);
        return -1;
    }

    uint32_t ml_count = 0u;
    uint32_t total_vx = 0u;
    uint32_t total_tr = 0u;
    uint32_t cursor   = 0u;
    uint32_t stamp    = 1u;

    while (cursor < tri_total) {
        while (cursor < tri_total && used_t[cursor] != 0u) {
            cursor++;
        }
        if (cursor >= tri_total) {
            break;
        }

        uint32_t local_gverts[K_SCENE_MESHLET_MAX_VERTS];
        uint32_t local_tris[K_SCENE_MESHLET_MAX_TRIS][3];
        uint32_t lv_count = 0u;
        uint32_t lt_count = 0u;
        float    nsum_x = 0.0f, nsum_y = 0.0f, nsum_z = 0.0f;

        // Seed meshlet with cursor triangle.
        used_t[cursor] = 1u;
        for (uint32_t k = 0u; k < 3u; ++k) {
            uint32_t gv = tris[cursor].v[k];
            if (v_stamp[gv] != stamp) {
                v_stamp[gv] = stamp;
                v_local[gv] = (uint8_t)lv_count;
                local_gverts[lv_count++] = gv;
            }
            local_tris[0][k] = v_local[gv];
        }
        lt_count = 1u;
        if (!tris[cursor].degenerate) {
            nsum_x = tris[cursor].nx;
            nsum_y = tris[cursor].ny;
            nsum_z = tris[cursor].nz;
        }
        float seed_cx = tris[cursor].cx;
        float seed_cy = tris[cursor].cy;
        float seed_cz = tris[cursor].cz;

        while (lt_count < K_SCENE_MESHLET_MAX_TRIS && lv_count <= K_SCENE_MESHLET_MAX_VERTS) {
            int32_t  best_idx   = -1;
            float    best_score = -1.0e30f;
            uint32_t inspected  = 0u;

            float nlen = sqrtf(nsum_x * nsum_x + nsum_y * nsum_y + nsum_z * nsum_z);
            float nax = nlen > 1.0e-6f ? (nsum_x / nlen) : 0.0f;
            float nay = nlen > 1.0e-6f ? (nsum_y / nlen) : 1.0f;
            float naz = nlen > 1.0e-6f ? (nsum_z / nlen) : 0.0f;

            for (uint32_t scan = cursor + 1u; scan < tri_total && inspected < k_morton_window; ++scan) {
                if (used_t[scan] != 0u) {
                    continue;
                }
                inspected++;

                uint32_t v0 = tris[scan].v[0];
                uint32_t v1 = tris[scan].v[1];
                uint32_t v2 = tris[scan].v[2];
                uint32_t add_v = 0u;
                if (v_stamp[v0] != stamp) add_v++;
                if (v1 != v0 && v_stamp[v1] != stamp) add_v++;
                if (v2 != v0 && v2 != v1 && v_stamp[v2] != stamp) add_v++;
                if (lv_count + add_v > K_SCENE_MESHLET_MAX_VERTS) {
                    continue;
                }

                uint32_t shared = 3u - add_v;
                float ndot = tris[scan].nx * nax + tris[scan].ny * nay + tris[scan].nz * naz;
                float dx = tris[scan].cx - seed_cx;
                float dy = tris[scan].cy - seed_cy;
                float dz = tris[scan].cz - seed_cz;
                float dist_pen = sqrtf(dx * dx + dy * dy + dz * dz) /
                                 fmaxf(span[0] + span[1] + span[2], 1.0e-4f);

                // Strongly reward shared vertices, aligned normal cone, and spatial locality.
                float score = (float)shared * 2.5f + ndot * 0.85f - dist_pen * 1.25f;
                if (ndot < -0.15f && shared == 0u) {
                    score -= 1.5f;
                }
                if (score > best_score) {
                    best_score = score;
                    best_idx = (int32_t)scan;
                }
            }

            if (best_idx < 0) {
                break;
            }
            uint32_t pick = (uint32_t)best_idx;
            used_t[pick] = 1u;
            for (uint32_t k = 0u; k < 3u; ++k) {
                uint32_t gv = tris[pick].v[k];
                if (v_stamp[gv] != stamp) {
                    v_stamp[gv] = stamp;
                    v_local[gv] = (uint8_t)lv_count;
                    local_gverts[lv_count++] = gv;
                }
                local_tris[lt_count][k] = v_local[gv];
            }
            if (!tris[pick].degenerate) {
                nsum_x += tris[pick].nx;
                nsum_y += tris[pick].ny;
                nsum_z += tris[pick].nz;
            }
            lt_count++;
        }

        // 4. Compute local meshlet AABB and quantize unique vertices to 16 bytes each.
        SceneMeshlet *ml = &out_ml[ml_count];
        float vmin[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
        float vmax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
        for (uint32_t i = 0u; i < lv_count; ++i) {
            const SceneCorner *cv = &uniq_v[local_gverts[i]];
            if (cv->x < vmin[0]) vmin[0] = cv->x;
            if (cv->x > vmax[0]) vmax[0] = cv->x;
            if (cv->y < vmin[1]) vmin[1] = cv->y;
            if (cv->y > vmax[1]) vmax[1] = cv->y;
            if (cv->z < vmin[2]) vmin[2] = cv->z;
            if (cv->z > vmax[2]) vmax[2] = cv->z;
        }
        ml->origin[0]   = vmin[0];
        ml->origin[1]   = vmin[1];
        ml->origin[2]   = vmin[2];
        ml->scale[0]    = vmax[0] - vmin[0];
        ml->scale[1]    = vmax[1] - vmin[1];
        ml->scale[2]    = vmax[2] - vmin[2];
        ml->vert_offset = total_vx;
        ml->tri_offset  = total_tr;
        ml->vert_count  = (uint8_t)lv_count;
        ml->tri_count   = (uint8_t)lt_count;

        float dequant_pos[K_SCENE_MESHLET_MAX_VERTS][3];
        float bcenter[3] = { 0.0f, 0.0f, 0.0f };
        for (uint32_t i = 0u; i < lv_count; ++i) {
            const SceneCorner *cv = &uniq_v[local_gverts[i]];
            float p_in[3] = { cv->x, cv->y, cv->z };
            uint16_t q_p[3] = {0};
            int16_t  oct_n[2] = {0};
            scene_quantize_pos16(p_in, ml->origin, ml->scale, q_p);
            scene_oct_encode_snorm16(cv->nx, cv->ny, cv->nz, oct_n);

            ScenePackedVertex *pv = &out_vx[total_vx + i];
            pv->px      = q_p[0];
            pv->py      = q_p[1];
            pv->pz      = q_p[2];
            pv->pad_u16 = 0u;
            pv->oct_nx  = oct_n[0];
            pv->oct_ny  = oct_n[1];
            pv->rgba8   = scene_pack_rgba8(cv->r, cv->g, cv->b, default_roughness);

            scene_dequantize_pos16(q_p, ml->origin, ml->scale, dequant_pos[i]);
            bcenter[0] += dequant_pos[i][0];
            bcenter[1] += dequant_pos[i][1];
            bcenter[2] += dequant_pos[i][2];
        }
        float inv_lv = 1.0f / (float)lv_count;
        // Blend AABB midpoint and vertex centroid for tight sphere center.
        ml->center[0] = 0.5f * ((vmin[0] + vmax[0]) * 0.5f + bcenter[0] * inv_lv);
        ml->center[1] = 0.5f * ((vmin[1] + vmax[1]) * 0.5f + bcenter[1] * inv_lv);
        ml->center[2] = 0.5f * ((vmin[2] + vmax[2]) * 0.5f + bcenter[2] * inv_lv);

        float max_r2 = 0.0f;
        for (uint32_t i = 0u; i < lv_count; ++i) {
            float dx = dequant_pos[i][0] - ml->center[0];
            float dy = dequant_pos[i][1] - ml->center[1];
            float dz = dequant_pos[i][2] - ml->center[2];
            float r2 = dx * dx + dy * dy + dz * dz;
            if (r2 > max_r2) {
                max_r2 = r2;
            }
        }
        ml->radius = sqrtf(max_r2) + 1.0e-4f;

        // 5. Pack local triangle indices and compute conservative post-quantization normal cone.
        float post_nsum[3] = { 0.0f, 0.0f, 0.0f };
        float post_fn[K_SCENE_MESHLET_MAX_TRIS][3];
        bool  post_deg[K_SCENE_MESHLET_MAX_TRIS];
        uint32_t valid_tris = 0u;
        for (uint32_t t = 0u; t < lt_count; ++t) {
            uint32_t i0 = local_tris[t][0];
            uint32_t i1 = local_tris[t][1];
            uint32_t i2 = local_tris[t][2];
            out_tr[total_tr + t] = (i0 & 0xFFu) | ((i1 & 0xFFu) << 8u) | ((i2 & 0xFFu) << 16u);

            compute_face_normal(dequant_pos[i0], dequant_pos[i1], dequant_pos[i2],
                                post_fn[t], &post_deg[t]);
            if (!post_deg[t]) {
                post_nsum[0] += post_fn[t][0];
                post_nsum[1] += post_fn[t][1];
                post_nsum[2] += post_fn[t][2];
                valid_tris++;
            }
        }

        if (valid_tris == 0u) {
            scene_oct_encode_snorm16(0.0f, 1.0f, 0.0f, ml->cone_axis_oct);
            ml->cone_cutoff_s16 = -32768;
        } else {
            scene_oct_encode_snorm16(post_nsum[0], post_nsum[1], post_nsum[2], ml->cone_axis_oct);
            float gpu_axis[3] = {0};
            scene_oct_decode_snorm16(ml->cone_axis_oct, gpu_axis);

            float min_dot = 1.0f;
            for (uint32_t t = 0u; t < lt_count; ++t) {
                if (post_deg[t]) {
                    continue;
                }
                float d = post_fn[t][0] * gpu_axis[0] +
                          post_fn[t][1] * gpu_axis[1] +
                          post_fn[t][2] * gpu_axis[2];
                if (d < min_dot) {
                    min_dot = d;
                }
            }
            if (min_dot <= 0.02f) {
                ml->cone_cutoff_s16 = -32768;
            } else {
                float sin_half = sqrtf(fmaxf(0.0f, 1.0f - min_dot * min_dot));
                float cutoff   = -sin_half - 0.005f;
                if (cutoff <= -0.999f) {
                    ml->cone_cutoff_s16 = -32768;
                } else {
                    ml->cone_cutoff_s16 = (int16_t)floorf(cutoff * 32767.0f);
                }
            }
        }

        total_vx += lv_count;
        total_tr += lt_count;
        ml_count++;
        stamp++;
        if (stamp == 0u) {
            memset(v_stamp, 0, (size_t)uniq_count * sizeof(*v_stamp));
            stamp = 1u;
        }
    }

    free(used_t);
    free(v_stamp);
    free(v_local);
    free(uniq_v);
    free(tris);

    out_build->meshlets       = out_ml;
    out_build->meshlet_count  = ml_count;
    out_build->vertices       = out_vx;
    out_build->vertex_count   = total_vx;
    out_build->triangles      = out_tr;
    out_build->triangle_count = total_tr;
    return 0;
}
