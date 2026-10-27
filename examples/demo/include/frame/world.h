#ifndef ENGINE_FRAME_WORLD_H
#define ENGINE_FRAME_WORLD_H

#include "vulkan/types.h"
#include <stdint.h>

// world/opening.world names what is placed. Each placed thing is one object
// with a coordinate and a function table. The closest hit on the click ray
// wins. Empty ground, the plane named in that file, collects corners.
// F closes that chain into one face, in click order, and the face turns
// once in its plane. Keys 1-6 raise a tetrahedron, octahedron, icosahedron,
// cylinder, cone, and torus. The OBJ is placed when the file names mesh.
// It turns once, and L reads it again. R keeps the selection spinning until
// R again. With nothing selected, R spins the cube. S smooths or facets the
// six solids. G opens or closes glow on the selection, or on the cube.
// Esc clears a partial build and the selection. Backspace removes the
// selection, or the last loose corner. F5 runs the selected object through
// one more revolution. A click on the cube also plants heat.
// The window idles when nothing is turning and no heat pulse is open.
// The cube's colors crawl one hue turn per revolution on those same stamps,
// then hold. The sweep does not arm a frame by itself.

void world_reset(void);

// Place what world/opening.world names. Call after the mesh pipeline exists.
[[nodiscard]] int world_load(void);

// One click, in pixels from the window center (+X right, +Y up), on the
// picture currently shown. cube is set when the ray hit the fillet, and
// heat_x, heat_y are that hit in posed world pixels. changed is set when
// the selection or a corner changed and a frame should be presented.
typedef struct WorldClick {
    bool  cube;
    bool  changed;
    float heat_x;
    float heat_y;
} WorldClick;

WorldClick world_click(float screen_x, float screen_y);

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
// else the cube. Starts an endless spin, or holds the current angle.
bool world_toggle_spin(void);

// S. Faceted cages and rounded cages, for every raised solid.
// True when a solid is on screen and a frame should be presented.
bool world_toggle_smooth(void);

// G. The selected OBJ mesh, else the selected solid, else the selected face,
// else the cube. Opens or closes that object's glow record.
bool world_toggle_glow(void);

// Drop loose corners and clear the selection. True when the picture changed.
bool world_cancel(void);

// Remove the selected OBJ mesh, else the selected solid, else the selected
// face. With no selection, remove the last loose corner.
// True when the picture changed.
bool world_delete(void);

// One more five-second revolution for the selected OBJ mesh, solid, or face.
// The cube yaw stays. True when a spin was armed.
bool world_respin_selected(void);

// F. Triangulate the open chain in click order. Fewer than three points,
// a crossing outline, or a chain that will not clip leaves the dots.
bool world_close_face(void);

// Degrees about Y. live is the pose world_advance just computed.
float world_cube_yaw_deg(bool live);

// The cube's heat step and the diffusion field tied to its vertices.
// A submit calls world_record. world_record_ok keeps that step's point.
void world_record(VkCommandBuffer cmd, uint32_t heat_dt, bool live);
void world_record_ok(uint64_t signal_point);
void world_record_abort(void);
uint64_t world_record_published(void);
bool world_record_step_open(void);
bool world_record_poke_pending(void);

// One compositor stamp for the heat pulse. Sets the interval a submit records.
// True while that pulse still wants a frame.
bool world_simulation(uint32_t time_ms, uint32_t *heat_dt);
void world_simulation_hold(void);
void world_simulation_commit(uint32_t time_ms, uint32_t heat_dt);

// True while a revolution is unfinished, a pose is not yet on screen,
// or a click preview is not yet on screen.
bool world_busy(void);

// Fold one wl_surface.frame timestamp into every spinning object.
// True when the picture differs from the image already on screen.
bool world_advance(uint32_t time_ms);

// The queued frame landed. The live picture is now the shown picture.
void world_commit(void);

// True once, when a revolution has been running and every object is now still.
// A static corner preview does not count. Clears that pending notice.
bool world_consume_spin(void);

// live draws the pose world_advance just computed. Otherwise the shown pose.
void world_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live);

// objects is the fillet cube, the OBJ mesh when it is up, the raised solids,
// and every closed face. vertices adds the OBJ corners to the baked meshes.
void world_stats(uint32_t *objects, uint32_t *vertices);

#endif /* ENGINE_FRAME_WORLD_H */
