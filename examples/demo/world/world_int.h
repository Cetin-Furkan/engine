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
    void (*prepare_scene)(const WorldObj *o, bool live);
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
    // time_ms is the compositor stamp. sim_dt_ms is the issued interval.
    // True while this object wants another frame. heat_dt is the part of
    // that interval this object will record. hold freezes the field.
    bool (*simulate)(WorldObj *o, uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt);
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

enum : int { k_table_cap = 64 };

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
void table_prepare_scene(bool live);
void table_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);
void table_draw_ops(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live, const WorldOps *ops);
bool table_advance(uint32_t time_ms);
bool table_advance_ops(uint32_t time_ms, const WorldOps *ops);
void table_commit(void);
bool table_busy(void);
bool table_busy_ops(const WorldOps *ops);
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
bool table_simulate(uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt);
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
void ground_set_visible(bool visible);
bool ground_is_visible(void);
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
bool cube_kind_spawn_pose(Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow);
bool cube_kind_spawn_elastic(Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow, float restitution);
float cube_kind_restitution(const WorldObj *o);
void world_step_motion(uint32_t time_ms, uint32_t sim_dt_ms);
const WorldOps *cube_ops(void);

struct MotionBody;
void solid_kind_reset(void);
bool solid_kind_raise(uint32_t kind);
int solid_kind_index(const char *name);
const char *solid_kind_name(uint32_t kind);
bool solid_kind_place(uint32_t kind, Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow);
bool solid_kind_place_elastic(uint32_t kind, Coord at, int32_t yaw_milli, bool spinning, bool infinite, bool glow, float restitution);
float solid_kind_restitution(const WorldObj *o);
uint32_t solid_kind_gather_motion(struct MotionBody *out, WorldObj **objs, uint32_t cap);
bool solid_kind_apply_motion(const struct MotionBody *bodies, WorldObj *const *objs, uint32_t n, float dt_s, bool *any_awake);
const WorldOps *solid_ops(void);
bool solid_kind_toggle_smooth(void);
bool solid_kind_smooth(void);
void solid_kind_set_smooth(bool curved);
bool solid_kind_smooth_dirty(void);
void solid_kind_commit_smooth(void);

void mesh_kind_reset(void);
void mesh_kind_set_wanted(bool wanted);
void mesh_kind_abandon(void);
bool mesh_kind_wanted(void);
bool mesh_kind_raise(void);
bool mesh_kind_pose(int32_t yaw_milli, bool spinning, bool infinite, bool glow);
const WorldOps *mesh_ops(void);
bool mesh_kind_apply(bool has, bool bytes);

void terrain_kind_reset(void);
bool terrain_kind_spawn(void);
const WorldOps *terrain_ops(void);

// A removed cube, solid, or mesh. Newest is the last entry. Faces are not stored.
enum : int {
    k_removed_cube  = 0,
    k_removed_solid = 1,
    k_removed_mesh  = 2,
};

typedef struct WorldRemoved {
    int     kind;
    int     solid;
    Coord   at;
    int32_t yaw_milli;
    float   restitution;
    bool    spinning;
    bool    infinite;
    bool    glow;
} WorldRemoved;

void world_removed_clear(void);
void world_removed_push(const WorldRemoved *item);
uint32_t world_removed_count(void);
const WorldRemoved *world_removed_at(uint32_t index);

void overlay_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);
bool overlay_busy(void);
void overlay_commit(void);

// Programmatic C23 WorldBuilder API: constructs complex multi-object worlds
// directly in C23 code or via composite scene-file directives (ringcube,
// gridcube, tower, arcsolid, solidring, helix) without requiring individual
// per-object text lines.
typedef struct WorldBuilder {
    uint32_t cubes_placed;
    uint32_t solids_placed;
    uint32_t blueprints_placed;
    bool     smooth_solids;
    bool     failed;
} WorldBuilder;

void world_builder_begin(WorldBuilder *wb, bool smooth_solids);
void world_builder_set_ground(WorldBuilder *wb, bool visible);
void world_builder_set_gimbal(WorldBuilder *wb, bool visible);
void world_builder_set_hud(WorldBuilder *wb, bool visible);
void world_builder_set_plot(WorldBuilder *wb, bool visible);
void world_builder_set_text(WorldBuilder *wb, bool visible);
void world_builder_set_clear(WorldBuilder *wb, float r, float g, float b);
void world_builder_set_camera(WorldBuilder *wb, float x, float y, float z,
                              float yaw, float pitch, float zoom, bool turntable);
void world_builder_set_mode(WorldBuilder *wb, uint8_t mode);
void world_builder_set_draw(WorldBuilder *wb, bool enabled);
void world_builder_set_tab(WorldBuilder *wb, bool enabled);
void world_builder_set_contours(WorldBuilder *wb, bool enabled);
bool world_builder_add_blueprint(WorldBuilder *wb, WorldBlueprint bp);
bool world_builder_add_cube(WorldBuilder *wb, Coord at, int32_t yaw_milli,
                            bool spinning, bool infinite, bool glow, float restitution);
bool world_builder_add_solid(WorldBuilder *wb, uint32_t kind, Coord at, int32_t yaw_milli,
                             bool spinning, bool infinite, bool glow, float restitution);
bool world_builder_add_cube_ring(WorldBuilder *wb, Coord center, float radius,
                                 uint32_t count, float restitution, bool glow);
bool world_builder_add_cube_grid(WorldBuilder *wb, Coord center, float step_x, float step_y,
                                 uint32_t nx, uint32_t ny, float restitution, bool glow);
bool world_builder_add_cube_tower(WorldBuilder *wb, Coord base, float step_z,
                                  uint32_t count, float restitution);
bool world_builder_add_solid_arc(WorldBuilder *wb, Coord center, float radius,
                                 float dz, float bounce_min, float bounce_max);
bool world_builder_add_solid_ring(WorldBuilder *wb, Coord center, float radius,
                                  float bounce_min, float bounce_max);
bool world_builder_add_helix(WorldBuilder *wb, Coord center, float radius,
                             float step_z, uint32_t count, float restitution);
[[nodiscard]] int world_builder_commit(const WorldBuilder *wb);
[[nodiscard]] int world_build_procedural_cathedral(void);
[[nodiscard]] int world_build_besparmak_terrain(void);

#endif /* ENGINE_WORLD_INT_H */
