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

void frame_gpu_raw_ns(uint64_t *compute_ns, uint64_t *render_ns, uint64_t *total_ns,
                      uint64_t *retired_samples) {
    if (compute_ns != nullptr) {
        *compute_ns = frame_state.last_gpu_comp_ns;
    }
    if (render_ns != nullptr) {
        *render_ns = frame_state.last_gpu_rend_ns;
    }
    if (total_ns != nullptr) {
        *total_ns = frame_state.last_gpu_tot_ns;
    }
    if (retired_samples != nullptr) {
        *retired_samples = frame_state.gpu_samples_retired;
    }
}

void frame_set_bench_mode(bool enabled) {
    frame_state.bench_uncapped = enabled;
}

void frame_note_damage_rect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    frame_state.scene_dirty = true;
    if (engine_app()->record_poke_pending != nullptr &&
        engine_app()->record_poke_pending()) {
        frame_state.sim_hot = true;
    }
    if (width == UINT32_MAX || height == UINT32_MAX) {
        frame_state.pending_damage_full = true;
        frame_state.pending_damage_set  = true;
        return;
    }
    if (frame_state.pending_damage_full || width == 0u || height == 0u) {
        return;
    }
    if (x < 0) {
        uint32_t cut = (uint32_t)(-(int64_t)x);
        if (cut >= width) {
            return;
        }
        width -= cut;
        x = 0;
    }
    if (y < 0) {
        uint32_t cut = (uint32_t)(-(int64_t)y);
        if (cut >= height) {
            return;
        }
        height -= cut;
        y = 0;
    }
    if (!frame_state.pending_damage_set) {
        frame_state.pending_damage     = (FrameRect){ x, y, width, height };
        frame_state.pending_damage_set = true;
        return;
    }
    int64_t ax0 = frame_state.pending_damage.x;
    int64_t ay0 = frame_state.pending_damage.y;
    int64_t ax1 = ax0 + (int64_t)frame_state.pending_damage.width;
    int64_t ay1 = ay0 + (int64_t)frame_state.pending_damage.height;
    int64_t bx0 = x;
    int64_t by0 = y;
    int64_t bx1 = bx0 + (int64_t)width;
    int64_t by1 = by0 + (int64_t)height;
    int64_t ux0 = ax0 < bx0 ? ax0 : bx0;
    int64_t uy0 = ay0 < by0 ? ay0 : by0;
    int64_t ux1 = ax1 > bx1 ? ax1 : bx1;
    int64_t uy1 = ay1 > by1 ? ay1 : by1;
    frame_state.pending_damage = (FrameRect){
        .x      = (int32_t)ux0,
        .y      = (int32_t)uy0,
        .width  = (uint32_t)(ux1 - ux0),
        .height = (uint32_t)(uy1 - uy0),
    };
}
