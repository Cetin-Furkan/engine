#include "world/world_int.h"
#include "frame/gimbal.h"
#include "frame/plot.h"
#include "frame/policy.h"

void overlay_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (policy_allows(PERM_PLOT_CYCLE)) {
        plot_draw(cmd, width, height);
    }
    if (policy_mode() != MODE_2D) {
        gimbal_draw(cmd, width, height);
    }
}

bool overlay_busy(void) {
    bool want_plot = policy_allows(PERM_PLOT_CYCLE) && plot_wants_frame();
    if (policy_mode() == MODE_2D) {
        return want_plot;
    }
    return want_plot || gimbal_busy();
}

void overlay_commit(void) {
    if (policy_allows(PERM_PLOT_CYCLE)) {
        plot_commit();
    }
    gimbal_commit();
}
