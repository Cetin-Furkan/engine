#ifndef ENGINE_FRAME_WORLD_H
#define ENGINE_FRAME_WORLD_H

#include "vulkan/types.h"
#include <stdbool.h>
#include <stdint.h>

// Object 0 is the centered fillet cube. It turns once about Y, then holds.
// Empty-space clicks collect corners. The fourth click closes one filled
// quad, ordered around the centroid, and that quad turns once in its plane.
// A later click starts the next quad. The window idles when nothing is turning.

void world_reset(void);

// origin_x is pixels right of center. origin_y is pixels up from center.
// True when the click changed the picture: a corner was stored, or a quad closed.
// False on the cube, inside a finished quad, on a corner already placed,
// a degenerate fourth click, or a full list.
bool world_spawn(float origin_x, float origin_y);

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
