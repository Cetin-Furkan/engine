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

typedef struct CubeDepth {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} CubeDepth;

// Multisample color target. The cube is drawn here and resolved into the
// dma-buf. Empty when the device only has one sample.
typedef struct CubeCoverage {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} CubeCoverage;

[[nodiscard]] int cube_init(VulkanContext *vk);
void cube_shutdown(VulkanContext *vk);

[[nodiscard]] int cube_depth_create(VulkanContext *vk, CubeDepth *depth,
                                    uint32_t width, uint32_t height);
void cube_depth_destroy(VulkanContext *vk, CubeDepth *depth);

[[nodiscard]] int cube_coverage_create(VulkanContext *vk, CubeCoverage *coverage,
                                       uint32_t width, uint32_t height);
void cube_coverage_destroy(VulkanContext *vk, CubeCoverage *coverage);

// Transitions depth for this draw. Fillet positions were written once in cube_init.
// The temperature field is separate. A live frame steps it before this draw.
void cube_prepare_draw(VkCommandBuffer cmd, VkImage depth);
// yaw_deg is degrees about Y. 38 with a 28 degree pitch is the still pose.
// origin_x is pixels right of center. origin_y is pixels up from center.
// depth_bias pulls a later object closer. Object 0 passes 0.
// phase is turns of hue, one full turn per 5000 ms of frame-clock time.
void cube_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, float yaw_deg,
               float origin_x, float origin_y, float depth_bias, float phase);

// Fold one wl_surface.frame stamp. The returned interval is the heat step
// for a frame that actually submits. Zero is a baseline, a repeated stamp,
// or a gap longer than a second. cube_heat_commit_stamp moves the baseline
// after that submit lands.
uint32_t cube_heat_interval(uint32_t time_ms);
void cube_heat_commit_stamp(uint32_t time_ms);

// Record one step into the command buffer. The draw in that buffer reads
// the temperatures the step writes. dt_ms of 0 records nothing unless a
// click is waiting to plant a spot. yaw_deg is the pose the vertex shader
// uses for this frame. record_ok keeps the written field and remembers
// signal_point, the acquire value that submit signals at ALL_COMMANDS.
// record_abort leaves the previous field and keeps the click. cube_heat_published
// is 0 until the first step lands; the bake idles the queue, so the seed
// needs no wait. A later submit waits that value before it reads the field.
void cube_heat_record(VkCommandBuffer cmd, uint32_t dt_ms, float yaw_deg);
void cube_heat_record_ok(uint64_t signal_point);
void cube_heat_record_abort(void);
uint64_t cube_heat_published(void);
bool cube_heat_step_open(void);

// A click inside the cube's disk. The next live frame plants a hot spot on
// the vertices near that point and diffuses for five seconds of callback
// time. The cube's yaw is not moved. A click during an open pulse restarts
// the five seconds.
bool cube_heat_poke(float origin_x, float origin_y);
bool cube_heat_poke_pending(void);

// True while that five-second window still wants frames. ended is set on
// the stamp that closes it. A stamp with no poke leaves both false.
bool cube_heat_wants_frame(uint32_t time_ms, bool *ended);

// The interval ended, or the opening turn idled with no pulse still open.
// Later presents keep the last temperatures until the next poke.
void cube_heat_hold(void);

// The filled-quad pipeline is drawn in the same pass, so it uses these.
VkFormat cube_depth_format(void);
VkSampleCountFlagBits cube_raster_samples(void);
uint32_t cube_vertex_count(void);

#endif /* ENGINE_FRAME_CUBE_H */
