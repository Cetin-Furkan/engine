#include "world/world_int.h"
#include "frame/plane.h"

// The ground draw is the screen-space metric grid behind the scene.
// It is on from reset. The world file may still name it (ground_set).
// That name also sets the hit plane in coord.h. The lines are computed
// from the camera each draw. No mesh is built.

static bool s_on = true;

void ground_reset(void) {
    coord_set_ground(2, 0.0f);
    s_on = true;
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
