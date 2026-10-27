#ifndef ENGINE_FRAME_HUD_H
#define ENGINE_FRAME_HUD_H

#include "vulkan/types.h"
#include <stdint.h>

// 8x8 readout drawn into frames that are already being recorded.
// It does not arm wl_surface.frame. While the window is idle the last
// numbers stay on the last buffer.

[[nodiscard]] int hud_init(VulkanContext *vk);
void hud_shutdown(VulkanContext *vk);

// One io_uring completion the idle loop has taken.
void hud_note_cqe(void);

// frame_ms is a wl_surface.frame timestamp in whole milliseconds.
// have_stamp is false on a present that was not paced by a new callback.
// The printed interval and fps are the average across this revolution.
// One millisecond step cannot land on 60: 16 ms prints as 62 and 17 ms as 58.
void hud_prepare(uint32_t frame_ms, bool have_stamp, uint64_t modifier);

// Presented frames in the revolution that just ended, and refreshes skipped
// between those frames. The callback timestamp is one millisecond, so a gap
// longer than one learned refresh counts as a miss. Clears the counters.
void hud_turn_report(uint32_t *frames, uint32_t *missed);

void hud_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, VkFormat format,
              VkFormat depth_format, VkSampleCountFlagBits samples);

#endif /* ENGINE_FRAME_HUD_H */
