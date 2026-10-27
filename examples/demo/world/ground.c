#include "world/world_int.h"
#include "frame/plane.h"

// The ground is the clipmap. Its lines are computed from the camera
// each draw. The world file names the plane. This file does not build a mesh.

static bool s_on = false;

void ground_reset(void) {
    s_on = false;
    coord_clear_ground();
}

bool ground_set(int axis, float offset) {
    coord_set_ground(axis, offset);
    s_on = true;
    return true;
}

void ground_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!s_on) {
        return;
    }
    plane_draw(cmd, width, height);
}
