#include "world/world_int.h"
#include "frame/gimbal.h"
#include "frame/plot.h"

void overlay_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    plot_draw(cmd, width, height);
    gimbal_draw(cmd, width, height);
}

bool overlay_busy(void) {
    return plot_wants_frame() || gimbal_busy();
}

void overlay_commit(void) {
    plot_commit();
    gimbal_commit();
}
