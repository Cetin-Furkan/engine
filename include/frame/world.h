#ifndef ENGINE_FRAME_WORLD_H
#define ENGINE_FRAME_WORLD_H

#include "vulkan/types.h"
#include <stdbool.h>
#include <stdint.h>

// Object 0 is the centered fillet cube. It turns once about Y, then holds.
// Empty-space clicks collect corners. The fourth click closes one filled
// quad, ordered around the centroid, and that quad turns once in its plane.
// A later click starts the next quad. A click inside the cube's disk plants
// heat instead of a corner. A click on a finished quad selects it. Keys 1-6
// raise a tetrahedron, octahedron, icosahedron, cylinder, cone, and torus.
// Each turns once. R keeps the selection spinning until R again. With nothing
// selected, R spins the cube. S smooths or facets those meshes. Esc clears
// a partial build and the selection. Backspace removes the selection, or the
// last loose corner. F5 runs the selected object through one more revolution.
// The window idles when nothing is turning and no heat pulse is open.
// The cube's colors crawl one hue turn per revolution on those same stamps,
// then hold. The sweep does not arm a frame by itself.

void world_reset(void);

// origin_x is pixels right of center. origin_y is pixels up from center.
// True when the click changed the picture: a corner was stored, or a quad closed.
// False on the cube, inside a finished quad, on a corner already placed,
// a degenerate fourth click, or a full list.
bool world_spawn(float origin_x, float origin_y);

// True inside the disk that keeps quad corners off the cube.
bool world_hits_cube(float origin_x, float origin_y);

// The topmost finished quad under the point becomes the selection.
// True when that selection changed and a frame should be presented.
bool world_select_at(float origin_x, float origin_y);

// True inside a raised solid's disk.
bool world_hits_solid(float origin_x, float origin_y);

// The topmost raised solid under the point becomes the selection.
// True when that selection changed.
bool world_select_solid_at(float origin_x, float origin_y);

// Key 1 is kind 0. Raises that mesh, or selects it if it is already up.
// True when the picture changed.
bool world_raise_solid(uint32_t kind);

// R. The selected solid, else the selected quad, else the cube.
// Starts an endless spin, or holds the current angle.
bool world_toggle_spin(void);

// S. Faceted cages and rounded cages, for every raised solid.
// True when a solid is on screen and a frame should be presented.
bool world_toggle_smooth(void);

// Drop loose corners and clear the selection. True when the picture changed.
bool world_cancel(void);

// Remove the selected quad. With no selection, remove the last loose corner.
// True when the picture changed.
bool world_delete(void);

// One more five-second revolution for the selected quad. The cube yaw stays.
// True when a spin was armed.
bool world_respin_selected(void);

// Degrees about Y. live is the pose world_advance just computed.
float world_cube_yaw_deg(bool live);

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

// objects is the fillet cube plus every closed quad. vertices is the baked mesh.
void world_stats(uint32_t *objects, uint32_t *vertices);

#endif /* ENGINE_FRAME_WORLD_H */
