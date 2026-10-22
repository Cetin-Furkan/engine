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

// Transitions depth for this draw. The fillet mesh was written once in cube_init.
void cube_prepare_draw(VkCommandBuffer cmd, VkImage depth);
// yaw_deg is degrees about Y. 38 with a 28 degree pitch is the still pose.
// origin_x is pixels right of center. origin_y is pixels up from center.
// depth_bias pulls a later object closer. Object 0 passes 0.
void cube_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, float yaw_deg,
               float origin_x, float origin_y, float depth_bias);

// The filled-quad pipeline is drawn in the same pass, so it uses these.
VkFormat cube_depth_format(void);
VkSampleCountFlagBits cube_raster_samples(void);
uint32_t cube_vertex_count(void);

#endif /* ENGINE_FRAME_CUBE_H */
