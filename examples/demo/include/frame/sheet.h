#ifndef ENGINE_FRAME_SHEET_H
#define ENGINE_FRAME_SHEET_H

#include "physics/physics.h"
#include "vulkan/types.h"
#include <stddef.h>
#include <stdint.h>

typedef struct SheetGpuStep {
    const AvbdVertex     *verts;
    uint32_t              vert_count;
    const AvbdConstraint *cons;
    uint32_t              con_count;
    float                 dt;
    float                 gravity[3];
    float                 damp_keep;
} SheetGpuStep;

// The flat sheet: rigid boxes, soft patches, and convex shapes from a file.
// examples/demo/world/sheet.world is the document L reads in 2D and Hybrid.
// sheet.live is the rest pose, next to that file, and is not part of live.world.
// 3D does not draw or step the sheet. 2D does not step the fillets or the lattice.

void sheet_reset(void);
void sheet_select_world(uint32_t world_id);

// 0 loaded, 1 absent, -1 present but refused. A refused load keeps the
// sheet already in memory.
[[nodiscard]] int sheet_load_path(const char *path);
[[nodiscard]] int sheet_load_memory(const char *text, size_t n);

// Boot order: sheet.live when it exists, otherwise sheet.world.
// A missing pair installs the built-in sheet.
[[nodiscard]] int sheet_load(void);

// Writes sheet.live. sheet.world is not replaced.
// sheet_hold_save keeps that write off. The unit test uses it.
[[nodiscard]] int sheet_save(void);
void sheet_hold_save(bool hold);
[[nodiscard]] int sheet_save_path(const char *path);

// Reads sheet.world again. False when that file is missing or refused.
bool sheet_reload(void);

// World pixels on z = 0. True when a body was hit and shoved.
bool sheet_click(float x, float y);
void sheet_clear(void);
bool sheet_cancel(void);
bool sheet_has_selection(void);
bool sheet_delete(void);

// One issued interval, in 8 ms steps. True while any body is still awake.
bool sheet_simulate(uint32_t sim_dt_ms);
// One step, in seconds. No save. The unit test drives this directly.
bool sheet_integrate(float dt_s);
bool sheet_busy(void);
// True once after a step moved a body. The draw consumes it.
bool sheet_take_changed(void);

void sheet_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

uint32_t sheet_rigid_count(void);
uint32_t sheet_soft_count(void);
uint32_t sheet_dyn_count(void);
void sheet_rigid_pos(uint32_t index, float *x, float *y);
void sheet_dyn_local(uint32_t index, uint32_t vert, float *x, float *y);
void sheet_soft_move(uint32_t index, uint32_t point, float dx, float dy);
void sheet_soft_pos(uint32_t index, uint32_t point, float *x, float *y);
void sheet_soft_layout(uint32_t index, uint32_t *cols, uint32_t *rows, float *spacing);

void sheet_set_avbd_iterations(uint32_t iterations);
bool sheet_pop_gpu_step(SheetGpuStep *out);

#endif /* ENGINE_FRAME_SHEET_H */
