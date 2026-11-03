#include "frame/frame.h"
#include "frame/frame_int.h"

// Starts the present process and calls the code in this folder.

int engine_subsystem_init(void) {
    return frame_boot();
}

int engine_subsystem_run(void) {
    return frame_run();
}

int engine_subsystem_shutdown(void) {
    return frame_stop();
}

void frame_gpu_timings(uint32_t *compute_us, uint32_t *render_us, uint32_t *total_us) {
    if (compute_us != nullptr) {
        *compute_us = frame_state.clock.gpu_compute_us;
    }
    if (render_us != nullptr) {
        *render_us = frame_state.clock.gpu_render_us;
    }
    if (total_us != nullptr) {
        *total_us = frame_state.clock.gpu_total_us;
    }
}
