#include "world/world_int.h"
#include <math.h>
#include <stdio.h>

static constexpr float k_deg_to_rad = 0.017453292519943295f;

static WorldObj s_obj[k_table_cap];
static int      s_n = 0;
static uint32_t s_next_generation = 1;
static int      s_selected = -1;
static uint32_t s_selected_generation = 0;
static int      s_shown_selected = -1;
static bool     s_spun = false;
static EcsWorld s_ecs;
static bool     s_ecs_ready = false;

static void ensure_ecs(void) {
    if (!s_ecs_ready) {
        ecs_world_init(&s_ecs);
        s_ecs_ready = true;
    }
}

EcsWorld *world_ecs(void) {
    ensure_ecs();
    return &s_ecs;
}

EcsEntity world_selected_entity(void) {
    WorldObj *o = table_selected();
    if (o == nullptr || !o->alive) {
        return ECS_NULL_ENTITY;
    }
    return o->entity;
}

void table_reset(void) {
    ensure_ecs();
    ecs_world_reset(&s_ecs);
    // Poison every entry: ops nulled and generation bumped, so any
    // WorldObj* or index held across the reset fails validation instead
    // of silently naming whatever is placed next.
    for (int i = 0; i < k_table_cap; ++i) {
        s_obj[i].ops = nullptr;
        s_obj[i].data = nullptr;
        s_obj[i].alive = false;
        s_obj[i].shown_alive = false;
        s_obj[i].entity = ECS_NULL_ENTITY;
        s_obj[i].last_spin_deg = 0.0f;
        s_obj[i].generation = s_next_generation++;
        if (s_next_generation == 0) {
            s_next_generation = 1;
        }
    }
    s_n = 0;
    s_selected = -1;
    s_selected_generation = 0;
    s_shown_selected = -1;
    s_spun = false;
}

int table_add(const WorldOps *ops, void *data, Coord at, Spin *spin) {
    ensure_ecs();
    if (ops == nullptr || s_n >= k_table_cap) {
        return -1;
    }
    int index = s_n++;
    float init_yaw = spin != nullptr ? spin_deg(spin, true) : 0.0f;
    s_obj[index] = (WorldObj){
        .ops           = ops,
        .data          = data,
        .at            = at,
        .generation    = s_next_generation++,
        .alive         = true,
        .spin          = spin,
        .entity        = ECS_NULL_ENTITY,
        .last_spin_deg = init_yaw,
    };
    if (s_next_generation == 0) {
        s_next_generation = 1;
    }

    const char *ename = "Entity";
    if (ops->name != nullptr) {
        const char *n = ops->name(&s_obj[index]);
        if (n != nullptr && n[0] != '\0') {
            ename = n;
        }
    }
    uint32_t mask = ECS_COMP_TRANSFORM | ECS_COMP_HIERARCHY | ECS_COMP_BOUNDS | ECS_COMP_RENDER;
    EcsEntity e = ecs_spawn(&s_ecs, ename, ECS_KIND_EMPTY, mask, (int32_t)index);
    s_obj[index].entity = e;
    if (e != ECS_NULL_ENTITY) {
        float q[4];
        ecs_quat_from_pose(0.0f, init_yaw * k_deg_to_rad, q);
        const float pos[3] = { at.x, at.y, at.z };
        const float scl[3] = { 1.0f, 1.0f, 1.0f };
        const float ext[3] = { 60.0f, 60.0f, 60.0f };
        ecs_set_local_trs(&s_ecs, e, pos, q, scl);
        ecs_set_local_bounds(&s_ecs, e, ext, 80.0f);
        ecs_propagate_transforms(&s_ecs);
    }
    return index;
}

void table_configure_entity(int index, EcsKind kind, const float half_ext[3],
                            float radius, float pitch_deg) {
    ensure_ecs();
    WorldObj *o = table_at(index);
    if (o == nullptr || !o->alive || o->entity == ECS_NULL_ENTITY) {
        return;
    }
    uint32_t d = ecs_dense_index(&s_ecs, o->entity);
    if (d != ECS_INVALID_DENSE) {
        s_ecs.kind[d] = (uint8_t)kind;
        if (kind == ECS_KIND_CUBE || kind == ECS_KIND_SOLID) {
            s_ecs.comp_mask[d] |= ECS_COMP_PHYSICS;
        }
    }
    float init_yaw = o->spin != nullptr ? spin_deg(o->spin, true) : 0.0f;
    o->last_spin_deg = init_yaw;
    float q[4];
    ecs_quat_from_pose(pitch_deg * k_deg_to_rad, init_yaw * k_deg_to_rad, q);
    const float pos[3] = { o->at.x, o->at.y, o->at.z };
    const float scl[3] = { 1.0f, 1.0f, 1.0f };
    ecs_set_local_trs(&s_ecs, o->entity, pos, q, scl);
    if (half_ext != nullptr) {
        ecs_set_local_bounds(&s_ecs, o->entity, half_ext, radius);
    }
    ecs_propagate_transforms(&s_ecs);
}

void table_sync_entity_world_pos(WorldObj *o) {
    ensure_ecs();
    if (o == nullptr || !o->alive || o->entity == ECS_NULL_ENTITY) {
        return;
    }
    ecs_set_world_pos(&s_ecs, o->entity, o->at.x, o->at.y, o->at.z);
}

void table_sync_ecs(bool live) {
    ensure_ecs();
    if (live) {
        for (int i = 0; i < s_n; ++i) {
            WorldObj *o = &s_obj[i];
            if (!o->alive || o->entity == ECS_NULL_ENTITY || !ecs_alive(&s_ecs, o->entity)) {
                continue;
            }
            if (o->spin != nullptr) {
                float cur_yaw = spin_deg(o->spin, true);
                float dyaw = cur_yaw - o->last_spin_deg;
                if (fabsf(dyaw) > 1.0e-5f) {
                    EcsTrs local = {0};
                    if (ecs_get_local_trs(&s_ecs, o->entity, &local)) {
                        float q_delta[4];
                        float q_next[4];
                        ecs_quat_from_axis_angle((const float[3]){ 0.0f, 1.0f, 0.0f },
                                                 dyaw * k_deg_to_rad, q_delta);
                        ecs_quat_mul(q_delta, local.rot, q_next);
                        ecs_quat_norm(q_next);
                        ecs_set_local_rot(&s_ecs, o->entity, q_next);
                    }
                    o->last_spin_deg = cur_yaw;
                }
            }
        }
    }
    ecs_propagate_transforms(&s_ecs);
    // Keep legacy WorldObj::at synchronized with propagated world positions (essential for children)
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->entity == ECS_NULL_ENTITY) {
            continue;
        }
        EcsTrs wt = {0};
        if (ecs_get_world_trs(&s_ecs, o->entity, &wt)) {
            o->at.x = wt.pos[0];
            o->at.y = wt.pos[1];
            o->at.z = wt.pos[2];
        }
    }
}

void table_publish(void) {
    for (int i = 0; i < s_n; ++i) {
        if (!s_obj[i].alive) {
            continue;
        }
        s_obj[i].shown_alive = true;
        if (s_obj[i].spin != nullptr) {
            spin_commit(s_obj[i].spin);
        }
        if (s_obj[i].ops->commit != nullptr) {
            s_obj[i].ops->commit(&s_obj[i]);
        }
    }
    s_shown_selected = s_selected;
}

WorldObj *table_at(int index) {
    if (index < 0 || index >= s_n) {
        return nullptr;
    }
    return &s_obj[index];
}

int table_count(void) {
    return s_n;
}

WorldObj *table_find(const WorldOps *ops) {
    for (int i = 0; i < s_n; ++i) {
        if (s_obj[i].alive && s_obj[i].ops == ops) {
            return &s_obj[i];
        }
    }
    return nullptr;
}

WorldObj *table_selected(void) {
    WorldObj *o = table_at(s_selected);
    if (o == nullptr || o->generation != s_selected_generation || !o->alive) {
        // Stale or poisoned selection: drop it rather than acting on an
        // entry that was reset/reused under the same index.
        s_selected = -1;
        s_selected_generation = 0;
        return nullptr;
    }
    return o;
}

static const char *obj_name(const WorldObj *o) {
    if (o->ops != nullptr && o->ops->name != nullptr) {
        const char *n = o->ops->name(o);
        if (n != nullptr) {
            return n;
        }
    }
    return "Object";
}

bool table_select(int index) {
    WorldObj *o = table_at(index);
    if (o == nullptr || !o->alive) {
        return false;
    }
    bool changed = s_selected != index;
    if (changed) {
        s_selected = index;
        s_selected_generation = o->generation;
        printf("[Engine] %s selected.\n", obj_name(o));
        fflush(stdout);
    }
    return changed;
}

void table_select_quiet(int index) {
    WorldObj *o = table_at(index);
    if (o == nullptr) {
        return;
    }
    s_selected = index;
    s_selected_generation = o->generation;
}

void table_clear_selection(void) {
    s_selected = -1;
    s_selected_generation = 0;
}

bool table_highlight(const WorldObj *o, bool live) {
    if (o == nullptr) {
        return false;
    }
    int index = (int)(o - s_obj);
    if (index < 0 || index >= s_n) {
        return false;
    }
    return index == (live ? s_selected : s_shown_selected);
}

int table_pick(const CameraRay *ray, Coord *at, float *t_hit) {
    int best = -1;
    float best_t = 0.0f;
    Coord best_at = {0};
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->hit == nullptr) {
            continue;
        }
        float t = 0.0f;
        Coord hit = {0};
        if (!o->ops->hit(o, ray, &t, &hit)) {
            continue;
        }
        if (best < 0 || t < best_t) {
            best = i;
            best_t = t;
            best_at = hit;
        }
    }
    if (best < 0) {
        return -1;
    }
    if (at != nullptr) {
        *at = best_at;
    }
    if (t_hit != nullptr) {
        *t_hit = best_t;
    }
    return best;
}

void table_prepare_scene(bool live) {
    for (int i = 0; i < s_n; ++i) {
        const WorldObj *o = &s_obj[i];
        bool up = live ? o->alive : o->shown_alive;
        if (!up || o->ops == nullptr || o->ops->prepare_scene == nullptr) {
            continue;
        }
        o->ops->prepare_scene(o, live);
    }
}

void table_draw_shadow(VkCommandBuffer cmd, const float light_clip_vp[16], bool live) {
    for (int i = 0; i < s_n; ++i) {
        const WorldObj *o = &s_obj[i];
        bool up = live ? o->alive : o->shown_alive;
        if (!up || o->ops == nullptr || o->ops->draw_shadow == nullptr) {
            continue;
        }
        o->ops->draw_shadow(o, cmd, light_clip_vp, live);
    }
}

void table_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        bool up = live ? o->alive : o->shown_alive;
        if (!up || o->ops == nullptr || o->ops->draw == nullptr) {
            continue;
        }
        o->ops->draw(o, cmd, width, height, live);
    }
}

void table_draw_ops(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live, const WorldOps *ops) {
    if (ops == nullptr) {
        return;
    }
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        bool up = live ? o->alive : o->shown_alive;
        if (!up || o->ops != ops || o->ops->draw == nullptr) {
            continue;
        }
        o->ops->draw(o, cmd, width, height, live);
    }
}

bool table_advance(uint32_t time_ms) {
    bool changed = s_selected != s_shown_selected;
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->alive != o->shown_alive) {
            changed = true;
        }
        if (!o->alive || o->ops == nullptr || o->ops->advance == nullptr) {
            continue;
        }
        if (o->ops->advance(o, time_ms)) {
            changed = true;
        }
    }
    return changed;
}

bool table_advance_ops(uint32_t time_ms, const WorldOps *ops) {
    if (ops == nullptr) {
        return false;
    }
    bool changed = s_selected != s_shown_selected;
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->ops != ops) {
            continue;
        }
        if (o->alive != o->shown_alive) {
            changed = true;
        }
        if (!o->alive || o->ops->advance == nullptr) {
            continue;
        }
        if (o->ops->advance(o, time_ms)) {
            changed = true;
        }
    }
    return changed;
}

void table_commit(void) {
    ensure_ecs();
    int write = 0;
    int selected = -1;
    for (int i = 0; i < s_n; ++i) {
        if (!s_obj[i].alive) {
            if (s_obj[i].entity != ECS_NULL_ENTITY) {
                ecs_destroy(&s_ecs, s_obj[i].entity);
                s_obj[i].entity = ECS_NULL_ENTITY;
            }
            continue;
        }
        if (i == s_selected) {
            selected = write;
        }
        s_obj[i].shown_alive = true;
        if (s_obj[i].spin != nullptr) {
            spin_commit(s_obj[i].spin);
        }
        if (s_obj[i].ops->commit != nullptr) {
            s_obj[i].ops->commit(&s_obj[i]);
        }
        if (write != i) {
            s_obj[write] = s_obj[i];
        }
        if (s_obj[write].entity != ECS_NULL_ENTITY) {
            ecs_set_user_slot(&s_ecs, s_obj[write].entity, (int32_t)write);
        }
        write++;
    }
    s_n = write;
    s_selected = selected;
    s_shown_selected = selected;
    ecs_propagate_transforms(&s_ecs);
}

bool table_busy(void) {
    if (s_selected != s_shown_selected) {
        return true;
    }
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->alive != o->shown_alive) {
            return true;
        }
        if (!o->alive) {
            continue;
        }
        if (o->spin != nullptr && spin_busy(o->spin)) {
            return true;
        }
        if (o->ops != nullptr && o->ops->busy != nullptr && o->ops->busy(o)) {
            return true;
        }
    }
    return false;
}

bool table_busy_ops(const WorldOps *ops) {
    if (ops == nullptr) {
        return false;
    }
    if (s_selected != s_shown_selected) {
        return true;
    }
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->ops != ops) {
            continue;
        }
        if (o->alive != o->shown_alive) {
            return true;
        }
        if (!o->alive) {
            continue;
        }
        if (o->spin != nullptr && spin_busy(o->spin)) {
            return true;
        }
        if (o->ops->busy != nullptr && o->ops->busy(o)) {
            return true;
        }
    }
    return false;
}

void table_mark_spun(void) {
    s_spun = true;
}

bool table_spun_pending(void) {
    return s_spun;
}

void table_clear_spun(void) {
    s_spun = false;
}

uint32_t table_alive_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < s_n; ++i) {
        if (s_obj[i].alive) {
            n++;
        }
    }
    return n;
}

uint32_t table_alive_vertices(void) {
    uint32_t total = 0;
    for (int i = 0; i < s_n; ++i) {
        const WorldObj *o = &s_obj[i];
        if (o->alive && o->ops != nullptr && o->ops->vertices != nullptr) {
            total += o->ops->vertices(o);
        }
    }
    return total;
}

void table_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->record_compute == nullptr) {
            continue;
        }
        o->ops->record_compute(o, cmd, heat_dt, live);
    }
}

void table_record_ok(uint64_t signal_point) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->record_ok == nullptr) {
            continue;
        }
        o->ops->record_ok(o, signal_point);
    }
}

void table_record_abort(void) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->record_abort == nullptr) {
            continue;
        }
        o->ops->record_abort(o);
    }
}

uint64_t table_record_published(void) {
    // One queue. The next submit waits the newest compute signal any
    // object published. An earlier point is already ordered behind it.
    uint64_t point = 0;
    for (int i = 0; i < s_n; ++i) {
        const WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->record_published == nullptr) {
            continue;
        }
        uint64_t one = o->ops->record_published(o);
        if (one > point) {
            point = one;
        }
    }
    return point;
}

bool table_record_step_open(void) {
    for (int i = 0; i < s_n; ++i) {
        const WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->record_step_open == nullptr) {
            continue;
        }
        if (o->ops->record_step_open(o)) {
            return true;
        }
    }
    return false;
}

bool table_record_poke_pending(void) {
    for (int i = 0; i < s_n; ++i) {
        const WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->record_poke_pending == nullptr) {
            continue;
        }
        if (o->ops->record_poke_pending(o)) {
            return true;
        }
    }
    return false;
}

bool table_simulate(uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt) {
    bool pulse = false;
    uint32_t dt = 0;
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->simulate == nullptr) {
            continue;
        }
        uint32_t one = 0;
        if (o->ops->simulate(o, time_ms, sim_dt_ms, &one)) {
            pulse = true;
        }
        if (one > dt) {
            dt = one;
        }
    }
    if (heat_dt != nullptr) {
        *heat_dt = dt;
    }
    return pulse;
}

void table_simulate_hold(void) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->simulate_hold == nullptr) {
            continue;
        }
        o->ops->simulate_hold(o);
    }
}

void table_simulate_commit(uint32_t stamp, uint32_t heat_dt) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->simulate_commit == nullptr) {
            continue;
        }
        o->ops->simulate_commit(o, stamp, heat_dt);
    }
}

WorldObj *table_fallback(void) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->alive && o->ops != nullptr && o->ops->fallback) {
            return o;
        }
    }
    return nullptr;
}

enum { k_kind_cap = 8 };

typedef struct KindHook {
    void (*reset)(void);
    void (*self_check)(void);
} KindHook;

static KindHook s_kinds[k_kind_cap];
static int      s_kind_n = 0;

void table_kind_listen(void (*reset)(void), void (*self_check)(void)) {
    if (reset == nullptr && self_check == nullptr) {
        return;
    }
    if (s_kind_n >= k_kind_cap) {
        fprintf(stderr, "[World] Kind listen list is full\n");
        return;
    }
    s_kinds[s_kind_n++] = (KindHook){
        .reset      = reset,
        .self_check = self_check,
    };
}

void table_kinds_reset(void) {
    for (int i = 0; i < s_kind_n; ++i) {
        if (s_kinds[i].reset != nullptr) {
            s_kinds[i].reset();
        }
    }
}

void table_kinds_check(void) {
    for (int i = 0; i < s_kind_n; ++i) {
        if (s_kinds[i].self_check != nullptr) {
            s_kinds[i].self_check();
        }
    }
}
