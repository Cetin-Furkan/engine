#ifndef ENGINE_FRAME_PLOT_H
#define ENGINE_FRAME_PLOT_H

#include "ring/ring.h"
#include "vulkan/types.h"
#include <stdint.h>

// A math curve written by one compute dispatch, and a polyline loaded from
// plots/data.csv and plots/mesh.obj. A .blend file is Blender's database
// and is not parsed. M cycles the curve. L reads the files again.
// Neither key starts the cube's revolution.

[[nodiscard]] int plot_init(VulkanContext *vk, RingCqeFn on_cqe, void *on_user);
void plot_shutdown(VulkanContext *vk);

// Sin, sinc, gaussian, then off. True when the picture changed.
bool plot_cycle(void);

// Read the plot files again. on_cqe receives completions that are not the
// file read. True when the loaded points changed.
bool plot_reload(RingCqeFn on_cqe, void *on_user);

bool plot_wants_frame(void);
void plot_commit(void);

// Records the math dispatch before the color pass. The draw reads it.
void plot_prepare(VkCommandBuffer cmd);
void plot_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height);

#endif /* ENGINE_FRAME_PLOT_H */
