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
    // A click that hit this object. True when the picture changed.
    bool (*on_click)(WorldObj *o, Coord hit);
    void (*draw)(const WorldObj *o, VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
    bool (*advance)(WorldObj *o, uint32_t time_ms);
    void (*commit)(WorldObj *o);
    bool (*busy)(const WorldObj *o);
    bool (*toggle_spin)(WorldObj *o);
    bool (*toggle_glow)(WorldObj *o);
    bool (*respin)(WorldObj *o);
    bool (*remove)(WorldObj *o);
    // Vertices this live object draws. Stats query through here, so a
    // kind with nothing on screen contributes zero.
    uint32_t (*vertices)(const WorldObj *o);
    const char *(*name)(const WorldObj *o);
    // GPU compute in the frame command buffer, before the draw. Null
    // skips the object. record_ok keeps the written field and stores
    // the acquire signal. record_abort restores the previous field.
    // published is that signal. step_open is true while this command
    // buffer holds an unsubmitted write. poke_pending is a click waiting
    // to be planted.
    void (*record_compute)(WorldObj *o, VkCommandBuffer cmd, uint32_t heat_dt, bool live);
    void (*record_ok)(WorldObj *o, uint64_t signal_point);
    void (*record_abort)(WorldObj *o);
    uint64_t (*record_published)(const WorldObj *o);
    bool (*record_step_open)(const WorldObj *o);
    bool (*record_poke_pending)(const WorldObj *o);
    // Compositor stamp. True while this object wants another frame.
    // heat_dt is the interval a submit records. hold freezes the field.
    // commit moves the stamp baseline after that submit lands.
    bool (*simulate)(WorldObj *o, uint32_t time_ms, uint32_t *heat_dt);
    void (*simulate_hold)(WorldObj *o);
    void (*simulate_commit)(WorldObj *o, uint32_t stamp, uint32_t heat_dt);
    // R and G reach this object when the selection does not handle them.
    bool fallback;
} WorldOps;

// Compile-time placement: an ops table plus its state, no text files.
// init_fn, when set, spawns through the kind's own entry point instead.
typedef struct WorldBlueprint {
    const WorldOps *ops;
    void           *data;
    Coord           at;
    Spin           *spin;
    bool          (*init_fn)(Coord at, void *data);
} WorldBlueprint;

struct WorldObj {
    const WorldOps *ops;
    void           *data;
    Coord           at;
    // Bumped on every (re)use of this entry. The selection remembers the
    // generation it picked; a mismatch means the entry was reset/reused
    // and the old selection is poison, not a pointer to the new object.
    uint32_t        generation;
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
uint32_t table_alive_vertices(void);
void table_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live);
void table_record_ok(uint64_t signal_point);
void table_record_abort(void);
uint64_t table_record_published(void);
bool table_record_step_open(void);
bool table_record_poke_pending(void);
bool table_simulate(uint32_t time_ms, uint32_t *heat_dt);
void table_simulate_hold(void);
void table_simulate_commit(uint32_t stamp, uint32_t heat_dt);
WorldObj *table_fallback(void);
// A kind opts in at startup. world_reset runs reset, then self_check
// after the camera exists. The world file does not name the kind for that.
void table_kind_listen(void (*reset)(void), void (*self_check)(void));
void table_kinds_reset(void);
void table_kinds_check(void);

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

bool cube_kind_spawn(Coord at);
const WorldOps *cube_ops(void);

void solid_kind_reset(void);
bool solid_kind_raise(uint32_t kind);
bool solid_kind_toggle_smooth(void);
bool solid_kind_smooth_dirty(void);
void solid_kind_commit_smooth(void);

void mesh_kind_reset(void);
void mesh_kind_set_wanted(bool wanted);
bool mesh_kind_wanted(void);
bool mesh_kind_raise(void);
bool mesh_kind_apply(bool has, bool bytes);

void overlay_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);
bool overlay_busy(void);
void overlay_commit(void);

#endif /* ENGINE_WORLD_INT_H */
