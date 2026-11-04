#ifndef ENGINE_FRAME_WORLD_H
#define ENGINE_FRAME_WORLD_H

#include "vulkan/types.h"
#include <stdint.h>

// The metric grid is the ground and is on with an empty placement list.
// examples/demo/world/opening.world names extra objects. Each placed thing
// is one object with a coordinate and a function table. The closest hit
// on the click ray wins. Empty ground collects corners while the draw
// permission is on. It is off at boot. Numpad 9 grants and revokes it.
// F closes that chain into one face, in click order, and the face stays.
// R or F5 turns it. Keys 1-6 raise a tetrahedron, octahedron, icosahedron,
// cylinder, cone, and torus. The OBJ is placed when the file names mesh.
// It turns once, and L reads it again. R keeps the selection spinning until
// R again. With nothing selected, R spins the first placed cube. S smooths
// or facets the six solids. G opens or closes glow on the selection, or on
// that cube. Esc clears a partial build and the selection. Backspace or
// Delete removes the selection, or the last loose corner, or the first
// placed cube when neither is present. F5 runs the selected object through
// one more revolution. A click on a cube also plants heat.
// Tab cycles 3D, 2D, and Hybrid. 2D draws the flat sheet and does not
// step the fillets or the lattice. Hybrid draws the 3D scene and the sheet.
// The window idles when nothing is turning and no heat pulse is open.
// Each cube's colors crawl one hue turn per revolution on those same stamps,
// then hold. The sweep does not arm a frame by itself.

enum : uint32_t { k_world_preset_max = 6u };

void world_select(uint32_t world_id);
uint32_t world_selected_id(void);
const char *world_selected_title(void);
void world_apply_policy(void);

void world_reset(void);

// Place what examples/demo/world/live.world names when that file exists,
// otherwise opening.world. Call after mesh_init. The graphics pipelines
// are created on the first draw, so this does not wait for one.
[[nodiscard]] int world_load(void);

// Write the live placement next to opening.world. The boot file is not
// replaced. Click-built faces and the heat field are not stored.
// A finite spin saved mid-turn reloads as a new turn from the saved yaw.
[[nodiscard]] int world_save(void);

// One click, in pixels from the window center (+X right, +Y up), on the
// picture currently shown. Dispatch is through WorldOps: the closest
// hit is selected, then its on_click (if any) runs. True when the
// selection or a corner changed and a frame should be presented.
bool world_click(float screen_x, float screen_y);

// Key 1 is kind 0. Raises that mesh, or selects it if it is already up.
// True when the picture changed.
bool world_raise_solid(uint32_t kind);

// The OBJ joined the opening turn when the file had faces.
void world_adopt_mesh(void);

// has is true when the reload stored a triangle. bytes is true when those
// corners changed. A mesh that appears does one revolution. A vertex change
// of a mesh already up does not.
bool world_apply_mesh(bool has, bool bytes);

// R. The selected OBJ mesh, else the selected solid, else the selected face,
// else the first placed cube. Starts an endless spin, or holds the current angle.
bool world_toggle_spin(void);

// S. Faceted cages and rounded cages, for every raised solid.
// True when a solid is on screen and a frame should be presented.
bool world_toggle_smooth(void);

// G. The selected OBJ mesh, else the selected solid, else the selected face,
// else the first placed cube. Opens or closes that object's glow record.
bool world_toggle_glow(void);

// Drop loose corners and clear the selection. True when the picture changed.
bool world_cancel(void);

// Remove the selected object. With no selection, remove the last loose
// corner. With neither, remove the first placed cube. True when the picture changed.
// A removed cube, solid, or mesh is remembered. Z puts it back.
bool world_delete(void);

// Z. Put back the last removed cube, solid, or mesh, at the place and yaw
// it had. The heat dent is not kept. Faces are not remembered.
// True when an object was placed.
bool world_restore(void);

// One more five-second revolution for the selected OBJ mesh, solid, or face.
// The cube yaw stays. True when a spin was armed.
bool world_respin_selected(void);

// F. Triangulate the open chain in click order. Fewer than three points,
// a crossing outline, or a chain that will not clip leaves the dots.
bool world_close_face(void);

// Objects that own a GPU step record it here. world_record_ok keeps
// that step's acquire point. An object with no compute is skipped.
void world_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live);
void world_record_ok(uint64_t signal_point);
void world_record_abort(void);
uint64_t world_record_published(void);
bool world_record_step_open(void);
bool world_record_poke_pending(void);

// time_ms is the compositor stamp and closes the heat pulse. sim_dt_ms is
// the interval the frame clock issued. A submit records that interval and
// no other. True while a pulse still wants a frame.
bool world_simulation(uint32_t time_ms, uint32_t sim_dt_ms, uint32_t *heat_dt);
void world_simulation_hold(void);
void world_simulation_commit(uint32_t time_ms, uint32_t heat_dt);

// True while a revolution is unfinished, a pose is not yet on screen,
// or a click preview is not yet on screen.
bool world_busy(void);

// Fold one simulation-cursor stamp into every spinning object.
// The cursor moves by the interval the frame clock issued.
// True when the picture differs from the image already on screen.
bool world_advance(uint32_t time_ms);

// The queued frame landed. The live picture is now the shown picture.
void world_commit(void);

// True once, when a revolution has been running and every object is now still.
// A static corner preview does not count. Clears that pending notice.
bool world_consume_spin(void);

// live draws the pose world_advance just computed. Otherwise the shown pose.
void world_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);

// objects is every live table entry. vertices sums the live objects'
// own WorldOps vertex counts, so unraised kinds contribute zero.
void world_stats(uint32_t *objects, uint32_t *vertices);

#endif /* ENGINE_FRAME_WORLD_H */
