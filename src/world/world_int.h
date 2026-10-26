#ifndef ENGINE_WORLD_INT_H
#define ENGINE_WORLD_INT_H

#include "frame/world.h"
#include "world/coord.h"
#include "world/spin.h"

// One placed thing. ops is the kind. data is that kind's record.
// alive is the picture being built. shown_alive is the picture on screen.
// The table walks ops. It does not name a cube, a face, or a mesh.

typedef struct WorldObj WorldObj;

typedef struct WorldOps {
    bool (*hit)(const WorldObj *o, const CameraRay *ray, float *t, Coord *at);
    void (*draw)(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
    bool (*advance)(WorldObj *o, uint32_t time_ms);
    void (*commit)(WorldObj *o);
    bool (*busy)(const WorldObj *o);
    bool (*toggle_spin)(WorldObj *o);
    bool (*toggle_glow)(WorldObj *o);
    bool (*respin)(WorldObj *o);
    bool (*remove)(WorldObj *o);
    const char *(*name)(const WorldObj *o);
} WorldOps;

struct WorldObj {
    const WorldOps *ops;
    void           *data;
    Coord           at;
    bool            alive;
    bool            shown_alive;
    Spin           *spin;
};

enum : int { k_table_cap = 48 };

void table_reset(void);
int table_add(const WorldOps *ops, void *data, Coord at, Spin *spin);
void table_publish(void);
WorldObj *table_at(int index);
int table_count(void);
WorldObj *table_find(const WorldOps *ops);
WorldObj *table_selected(void);
bool table_select(int index);
void table_select_quiet(int index);
void table_clear_selection(void);
bool table_highlight(const WorldObj *o, bool live);
int table_pick(const CameraRay *ray, Coord *at, float *t_hit);
void table_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
bool table_advance(uint32_t time_ms);
void table_commit(void);
bool table_busy(void);
void table_mark_spun(void);
bool table_spun_pending(void);
void table_clear_spun(void);
uint32_t table_alive_count(void);

void ground_reset(void);
bool ground_set(int axis, float offset);
void ground_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

void face_reset(void);
void face_self_check(void);
bool face_spawn(Coord at);
bool face_close(void);
bool face_cancel_chain(void);
bool face_pop_corner(void);
bool face_chain_busy(void);
void face_commit_chain(void);
void face_reap(void);
void face_draw_chain(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
uint32_t face_chain_count(void);
const WorldOps *face_ops(void);

void cube_kind_reset(void);
bool cube_kind_spawn(Coord at);
void cube_kind_self_check(void);
float cube_kind_yaw(bool live);
bool cube_kind_toggle_fallback(void);
bool cube_kind_glow_fallback(void);
bool cube_kind_poke(Coord world_hit);
void cube_kind_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live);
void cube_kind_record_ok(uint64_t signal_point);
void cube_kind_record_abort(void);
uint64_t cube_kind_published(void);
bool cube_kind_step_open(void);
bool cube_kind_poke_pending(void);
bool cube_kind_simulation(uint32_t time_ms, uint32_t *heat_dt);
void cube_kind_hold(void);
void cube_kind_commit_stamp(uint32_t stamp, uint32_t heat_dt);
const WorldOps *cube_ops(void);
uint32_t cube_kind_stat_vertices(void);

void solid_kind_reset(void);
bool solid_kind_raise(uint32_t kind);
bool solid_kind_toggle_smooth(void);
bool solid_kind_smooth_dirty(void);
void solid_kind_commit_smooth(void);
uint32_t solid_kind_stat_vertices(void);

void mesh_kind_reset(void);
void mesh_kind_set_wanted(bool wanted);
bool mesh_kind_wanted(void);
bool mesh_kind_raise(void);
bool mesh_kind_apply(bool has, bool bytes);
uint32_t mesh_kind_stat_vertices(void);

void overlay_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);
bool overlay_busy(void);
void overlay_commit(void);

#endif /* ENGINE_WORLD_INT_H */
