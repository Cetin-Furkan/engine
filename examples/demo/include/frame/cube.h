#ifndef ENGINE_FRAME_CUBE_H
#define ENGINE_FRAME_CUBE_H

#include "vulkan/types.h"

// One posed cube in the presented buffer.
// The GPU keeps a face only after the camera projection, and shades it with
// a directional sun. The cube is 240 pixels on a side. Object 0 stays centered.
// Later objects pass a pixel offset from the center. Width and height change
// the camera, not the cube.
// A compute dispatch writes the fillet vertices and an index buffer.
// The vertex shader reads those vertices by device address.
// That dispatch does not read the color attachment.
// A second dispatch steps one scalar temperature per vertex on a live frame.
// The draw mixes that temperature into the baked color.
// Each face is a 16 by 16 grid so a click lands on a sample. The fillet
// edges and corners stay on the original 888 vertices.
// Depth and MSAA targets live in frame/pass.h. This file is the fillet.
// The mesh is one buffer. Temperatures are one field per slot, so a click
// paints the fillet that was hit. The 128^3 tint stays one mesh-local sample.

enum : uint32_t { k_cube_heat_slots = 24u };

[[nodiscard]] int cube_init(VulkanContext *vk);
void cube_shutdown(VulkanContext *vk);

// yaw_deg is degrees about Y. 38 with a 28 degree pitch is the still pose.
// origin_x is pixels right of center. origin_y is pixels up from center.
// depth_bias pulls a later object closer. Object 0 passes 0.
// phase is turns of hue, one full turn per 5000 ms of frame-clock time.
// selected lightens the cube the same way a selected solid is lightened.
// mat is that fillet's look. The first uses slot 0. The rest use 40..62.
// heat_slot picks that fillet's temperature field. It is the pool index.
void cube_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, float yaw_deg,
               float origin_x, float origin_y, float depth_bias, float phase, bool selected,
               uint32_t mat, uint32_t heat_slot);

// Fold one wl_surface.frame stamp for this slot. The returned interval is
// the heat step for a frame that actually submits. Zero is a baseline, a
// repeated stamp, or a gap longer than a second. cube_heat_commit_stamp
// moves that slot's baseline after the submit lands. A held slot stays at 0.
uint32_t cube_heat_interval(uint32_t slot, uint32_t time_ms);
uint32_t cube_heat_step_dt(uint32_t slot);
void cube_heat_commit_stamp(uint32_t slot, uint32_t time_ms);

// Record this slot's step into the command buffer. The draw in that buffer
// reads the temperatures the step writes. A slot with no interval and no
// click records nothing. yaw_deg is the pose the vertex shader uses for
// this frame. record_ok keeps the written field and remembers signal_point,
// the acquire value that submit signals at ALL_COMMANDS. record_abort leaves
// the previous field and keeps the click. cube_heat_published is 0 until the
// first step lands; the bake idles the queue, so the seed needs no wait.
// A later submit waits that value before it reads any slot.
// True when this call wrote a field.
bool cube_heat_record(VkCommandBuffer cmd, uint32_t slot, float yaw_deg);
void cube_heat_record_ok(uint32_t slot, uint64_t signal_point);
void cube_heat_record_abort(uint32_t slot);
uint64_t cube_heat_published(void);
bool cube_heat_step_open(void);
bool cube_heat_slot_open(uint32_t slot);

// A click inside this fillet. The next live frame plants a hot spot on its
// vertices and diffuses for five seconds of callback time. Other slots are
// left as they are. The cube's yaw is not moved. A click during an open
// pulse restarts those five seconds.
bool cube_heat_poke(uint32_t slot, float origin_x, float origin_y);
bool cube_heat_poke_pending(void);
bool cube_heat_slot_pending(uint32_t slot);

// True while that slot's five-second window still wants frames. ended is
// set on the stamp that closes it. A stamp with no poke leaves both false.
bool cube_heat_wants_frame(uint32_t slot, uint32_t time_ms, bool *ended);

// The interval ended, or the opening turn idled with no pulse still open.
// Later presents keep this slot's last temperatures until the next poke.
void cube_heat_hold(uint32_t slot);
bool cube_heat_held(uint32_t slot);

// A new fillet takes the slot. The bake's upper-half seed is kept until
// this slot has already been stepped. A reused slot is seeded again.
void cube_heat_claim(uint32_t slot);
// The fillet left. The slot holds, and the next claim seeds it again.
void cube_heat_retire(uint32_t slot);

uint32_t cube_vertex_count(void);
[[nodiscard]] uint64_t cube_mesh_addr(void);

#endif /* ENGINE_FRAME_CUBE_H */
