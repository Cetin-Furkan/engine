#include "world/world_int.h"
#include "frame/plane.h"

// The ground is the universal z = 0 metric grid: coordinate space
// itself, not an object a file opts into. It is on from reset; the
// world file may still name it (ground_set) with no effect change.
// Its lines are computed from the camera each draw. No mesh is built.

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
