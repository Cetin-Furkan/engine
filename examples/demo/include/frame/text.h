#ifndef ENGINE_FRAME_TEXT_H
#define ENGINE_FRAME_TEXT_H

#include "text/face/face.h"
#include "vulkan/types.h"
#include <stdbool.h>
#include <stdint.h>

// Screen text. The sealed run is the embedded face and the word
// "Antigravity"; dynamic.faces cannot retarget it. User runs are boxes
// in the same click space as the engine: center origin, +X right, +Y up.
// Hold T and click twice to place one, then type. T is not a policy bind.

[[nodiscard]] int text_init(VulkanContext *vk);
void text_shutdown(VulkanContext *vk);

// text_begin claims one slice for signal_point. The slice stays until the
// acquire timeline reaches that point. text_release gives it back when the
// submit does not happen. A full ring skips the draw and keeps the last
// slice's bytes.
void text_begin(VkSemaphore acquire, uint64_t signal_point);
void text_release(uint64_t signal_point);
void text_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, VkFormat format,
               VkFormat depth_format, VkSampleCountFlagBits samples);

// id 0 is invalid. A sealed id is never issued. A freed id fails later.
[[nodiscard]] uint32_t text_add(float x0, float y0, float x1, float y1, const char *utf8);
[[nodiscard]] bool text_set(uint32_t id, const char *utf8);
[[nodiscard]] bool text_set_rect(uint32_t id, float x0, float y0, float x1, float y1);
[[nodiscard]] bool text_set_face(uint32_t id, uint32_t face);
[[nodiscard]] bool text_set_color(uint32_t id, float r, float g, float b, float a);
[[nodiscard]] bool text_remove(uint32_t id);

// True when the click was consumed. t_down is KEY_T held.
[[nodiscard]] bool text_click(float x, float y, bool t_down);

// Pointer pixels, top-left origin. Drops an anchor if T was released.
void text_arm_tick(bool t_down, bool have_xy, int32_t px, int32_t py);

// True when the key was consumed. Letters arrive through text_insert.
[[nodiscard]] bool text_on_key(uint32_t evdev_key);
[[nodiscard]] bool text_insert(const char *utf8, uint32_t bytes);
[[nodiscard]] bool text_editing(void);

[[nodiscard]] bool text_busy(void);
void text_commit(void);

#endif /* ENGINE_FRAME_TEXT_H */
