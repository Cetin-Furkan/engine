#include "ecs/ecs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static inline uint32_t entity_slot(EcsEntity e) {
    return (e & ECS_INDEX_MASK) - 1u;
}

static inline uint32_t entity_gen(EcsEntity e) {
    return e >> ECS_GEN_SHIFT;
}

static inline EcsEntity make_entity(uint32_t slot, uint32_t gen) {
    return (gen << ECS_GEN_SHIFT) | ((slot + 1u) & ECS_INDEX_MASK);
}

void ecs_world_init(EcsWorld *w) {
    if (w == nullptr) {
        return;
    }
    memset(w, 0, sizeof *w);
    for (uint32_t i = 0u; i < ECS_MAX_ENTITIES; ++i) {
        w->sparse_to_dense[i] = ECS_INVALID_DENSE;
        w->slot_gen[i]        = 1u;
        // LIFO free-list: pop slot 0 first for cache-friendly low indices
        w->free_list[i]       = (ECS_MAX_ENTITIES - 1u) - i;
    }
    w->free_count = ECS_MAX_ENTITIES;
    w->topo_dirty = true;
}

void ecs_world_reset(EcsWorld *w) {
    if (w == nullptr) {
        return;
    }
    // Preserve & bump slot generations across reset so stale handles are always rejected
    uint32_t saved_gen[ECS_MAX_ENTITIES];
    for (uint32_t i = 0u; i < ECS_MAX_ENTITIES; ++i) {
        uint32_t g = w->slot_gen[i] + 1u;
        if (g == 0u || g >= (1u << (32u - ECS_GEN_SHIFT))) {
            g = 1u;
        }
        saved_gen[i] = g;
    }
    memset(w, 0, sizeof *w);
    for (uint32_t i = 0u; i < ECS_MAX_ENTITIES; ++i) {
        w->sparse_to_dense[i] = ECS_INVALID_DENSE;
        w->slot_gen[i]        = saved_gen[i];
        w->free_list[i]       = (ECS_MAX_ENTITIES - 1u) - i;
    }
    w->free_count = ECS_MAX_ENTITIES;
    w->topo_dirty = true;
}

bool ecs_alive(const EcsWorld *w, EcsEntity e) {
    if (w == nullptr || e == ECS_NULL_ENTITY) {
        return false;
    }
    uint32_t raw_idx = e & ECS_INDEX_MASK;
    if (raw_idx == 0u || raw_idx > ECS_MAX_ENTITIES) {
        return false;
    }
    uint32_t slot = raw_idx - 1u;
    if (w->slot_gen[slot] != entity_gen(e)) {
        return false;
    }
    uint32_t dense = w->sparse_to_dense[slot];
    return dense < w->dense_count && w->dense_entity[dense] == e;
}

uint32_t ecs_dense_index(const EcsWorld *w, EcsEntity e) {
    if (!ecs_alive(w, e)) {
        return ECS_INVALID_DENSE;
    }
    return w->sparse_to_dense[entity_slot(e)];
}

EcsEntity ecs_spawn(EcsWorld *w, const char *name, EcsKind kind,
                    uint32_t comp_mask, int32_t user_slot) {
    if (w == nullptr || w->free_count == 0u || w->dense_count >= ECS_MAX_ENTITIES) {
        return ECS_NULL_ENTITY;
    }
    uint32_t slot  = w->free_list[--w->free_count];
    uint32_t dense = w->dense_count++;
    EcsEntity e    = make_entity(slot, w->slot_gen[slot]);

    w->sparse_to_dense[slot] = dense;
    w->dense_entity[dense]   = e;
    w->comp_mask[dense]      = comp_mask | ECS_COMP_TRANSFORM | ECS_COMP_HIERARCHY;
    w->user_slot[dense]      = user_slot;
    w->kind[dense]           = (uint8_t)kind;

    if (name != nullptr && name[0] != '\0') {
        snprintf(w->name[dense], ECS_NAME_CAP, "%s", name);
    } else {
        snprintf(w->name[dense], ECS_NAME_CAP, "Entity_%u", slot);
    }

    w->lx[dense] = 0.0f; w->ly[dense] = 0.0f; w->lz[dense] = 0.0f;
    w->qx[dense] = 0.0f; w->qy[dense] = 0.0f; w->qz[dense] = 0.0f; w->qw[dense] = 1.0f;
    w->sx[dense] = 1.0f; w->sy[dense] = 1.0f; w->sz[dense] = 1.0f;

    ecs_mat3x4_from_trs((const float[3]){ 0.0f, 0.0f, 0.0f },
                        (const float[4]){ 0.0f, 0.0f, 0.0f, 1.0f },
                        (const float[3]){ 1.0f, 1.0f, 1.0f },
                        w->world_m[dense]);
    ecs_mat3x4_cofactor3(w->world_m[dense], w->world_nrm[dense]);
    w->world_pos[dense][0] = 0.0f; w->world_pos[dense][1] = 0.0f; w->world_pos[dense][2] = 0.0f;
    w->world_rot[dense][0] = 0.0f; w->world_rot[dense][1] = 0.0f; w->world_rot[dense][2] = 0.0f; w->world_rot[dense][3] = 1.0f;
    w->world_scl[dense][0] = 1.0f; w->world_scl[dense][1] = 1.0f; w->world_scl[dense][2] = 1.0f;
    w->bound_scale[dense]  = 1.0f;

    w->parent[dense]       = ECS_NULL_ENTITY;
    w->first_child[dense]  = ECS_NULL_ENTITY;
    w->next_sibling[dense] = ECS_NULL_ENTITY;
    w->prev_sibling[dense] = ECS_NULL_ENTITY;
    w->depth[dense]        = 0u;
    w->dirty[dense]        = (uint8_t)(ECS_DIRTY_LOCAL | ECS_DIRTY_WORLD);

    w->local_ext[dense][0] = 60.0f; w->local_ext[dense][1] = 60.0f; w->local_ext[dense][2] = 60.0f;
    w->local_rad[dense]    = 84.0f;
    w->world_aabb_min[dense][0] = -60.0f; w->world_aabb_min[dense][1] = -60.0f; w->world_aabb_min[dense][2] = -60.0f;
    w->world_aabb_max[dense][0] =  60.0f; w->world_aabb_max[dense][1] =  60.0f; w->world_aabb_max[dense][2] =  60.0f;
    w->world_sphere[dense][0]   = 0.0f; w->world_sphere[dense][1] = 0.0f;
    w->world_sphere[dense][2]   = 0.0f; w->world_sphere[dense][3] = 84.0f;

    w->topo_dirty = true;
    return e;
}

static void move_dense_slot(EcsWorld *w, uint32_t dst, uint32_t src) {
    if (dst == src) {
        return;
    }
    EcsEntity moved_e = w->dense_entity[src];
    w->dense_entity[dst]   = moved_e;
    w->comp_mask[dst]      = w->comp_mask[src];
    w->user_slot[dst]      = w->user_slot[src];
    w->kind[dst]           = w->kind[src];
    memcpy(w->name[dst], w->name[src], ECS_NAME_CAP);

    w->lx[dst] = w->lx[src]; w->ly[dst] = w->ly[src]; w->lz[dst] = w->lz[src];
    w->qx[dst] = w->qx[src]; w->qy[dst] = w->qy[src]; w->qz[dst] = w->qz[src]; w->qw[dst] = w->qw[src];
    w->sx[dst] = w->sx[src]; w->sy[dst] = w->sy[src]; w->sz[dst] = w->sz[src];

    memcpy(w->world_m[dst],   w->world_m[src],   sizeof(w->world_m[dst]));
    memcpy(w->world_nrm[dst], w->world_nrm[src], sizeof(w->world_nrm[dst]));
    memcpy(w->world_pos[dst], w->world_pos[src], sizeof(w->world_pos[dst]));
    memcpy(w->world_rot[dst], w->world_rot[src], sizeof(w->world_rot[dst]));
    memcpy(w->world_scl[dst], w->world_scl[src], sizeof(w->world_scl[dst]));
    w->bound_scale[dst] = w->bound_scale[src];

    w->parent[dst]       = w->parent[src];
    w->first_child[dst]  = w->first_child[src];
    w->next_sibling[dst] = w->next_sibling[src];
    w->prev_sibling[dst] = w->prev_sibling[src];
    w->depth[dst]        = w->depth[src];
    w->dirty[dst]        = w->dirty[src];

    memcpy(w->local_ext[dst],      w->local_ext[src],      sizeof(w->local_ext[dst]));
    w->local_rad[dst] = w->local_rad[src];
    memcpy(w->world_aabb_min[dst], w->world_aabb_min[src], sizeof(w->world_aabb_min[dst]));
    memcpy(w->world_aabb_max[dst], w->world_aabb_max[src], sizeof(w->world_aabb_max[dst]));
    memcpy(w->world_sphere[dst],   w->world_sphere[src],   sizeof(w->world_sphere[dst]));

    w->sparse_to_dense[entity_slot(moved_e)] = dst;
}

void ecs_destroy(EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE) {
        return;
    }

    // Detach from parent cleanly
    if (w->parent[d] != ECS_NULL_ENTITY) {
        (void)ecs_set_parent(w, e, ECS_NULL_ENTITY, false);
        d = ecs_dense_index(w, e);
        if (d == ECS_INVALID_DENSE) {
            return;
        }
    }

    // Reparent all direct children to root preserving their exact world pose
    while (w->first_child[d] != ECS_NULL_ENTITY) {
        EcsEntity ch = w->first_child[d];
        (void)ecs_set_parent(w, ch, ECS_NULL_ENTITY, true);
        d = ecs_dense_index(w, e);
    }

    // O(1) swap-remove in dense SoA arrays
    uint32_t slot = entity_slot(e);
    uint32_t last = w->dense_count - 1u;
    move_dense_slot(w, d, last);
    w->dense_count--;

    w->sparse_to_dense[slot] = ECS_INVALID_DENSE;
    uint32_t g = w->slot_gen[slot] + 1u;
    if (g == 0u || g >= (1u << (32u - ECS_GEN_SHIFT))) {
        g = 1u;
    }
    w->slot_gen[slot] = g;
    if (w->free_count < ECS_MAX_ENTITIES) {
        w->free_list[w->free_count++] = slot;
    }
    w->topo_dirty = true;
}

uint32_t ecs_entity_count(const EcsWorld *w) {
    return w != nullptr ? w->dense_count : 0u;
}

EcsEntity ecs_entity_at(const EcsWorld *w, uint32_t dense_idx) {
    if (w == nullptr || dense_idx >= w->dense_count) {
        return ECS_NULL_ENTITY;
    }
    return w->dense_entity[dense_idx];
}

uint32_t ecs_query_mask(const EcsWorld *w, uint32_t required_mask,
                        EcsEntity *out_entities, uint32_t cap) {
    if (w == nullptr || out_entities == nullptr || cap == 0u) {
        return 0u;
    }
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < w->dense_count && n < cap; ++i) {
        if ((w->comp_mask[i] & required_mask) == required_mask) {
            out_entities[n++] = w->dense_entity[i];
        }
    }
    return n;
}

const char *ecs_get_name(const EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    return d != ECS_INVALID_DENSE ? w->name[d] : "None";
}

EcsKind ecs_get_kind(const EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    return d != ECS_INVALID_DENSE ? (EcsKind)w->kind[d] : ECS_KIND_EMPTY;
}

int32_t ecs_get_user_slot(const EcsWorld *w, EcsEntity e) {
    uint32_t d = ecs_dense_index(w, e);
    return d != ECS_INVALID_DENSE ? w->user_slot[d] : -1;
}

void ecs_set_user_slot(EcsWorld *w, EcsEntity e, int32_t user_slot) {
    uint32_t d = ecs_dense_index(w, e);
    if (d != ECS_INVALID_DENSE) {
        w->user_slot[d] = user_slot;
    }
}

void ecs_set_local_bounds(EcsWorld *w, EcsEntity e, const float half_ext[3], float radius) {
    uint32_t d = ecs_dense_index(w, e);
    if (d == ECS_INVALID_DENSE || half_ext == nullptr) {
        return;
    }
    w->local_ext[d][0] = fmaxf(fabsf(half_ext[0]), 1.0f);
    w->local_ext[d][1] = fmaxf(fabsf(half_ext[1]), 1.0f);
    w->local_ext[d][2] = fmaxf(fabsf(half_ext[2]), 1.0f);
    w->local_rad[d]    = fmaxf(radius, 1.0f);
    w->comp_mask[d]   |= ECS_COMP_BOUNDS;
    w->dirty[d]       |= (uint8_t)ECS_DIRTY_WORLD;
}
