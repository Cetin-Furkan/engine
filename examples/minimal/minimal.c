#include "engine/app.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Second standalone hosted program proving the EngineApp seam.
// Supplies ONLY the 7 [REQUIRED] hooks in include/engine/app.h, links
// solely against src/ (zero demo, terrain, scene, physics, lighting, or
// FreeType symbols), advances on the issued 8 ms simulation cursor,
// draws a single transient BDA vertex slice, handles click + key, and
// disarms cleanly into Loop idle.

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "minimal.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "minimal.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "minimal.vert.spv alignment");
static_assert(sizeof k_frag_spv % 4u == 0u, "minimal.frag.spv alignment");

typedef struct MinimalVertex {
    float pos_px[2];
    float pad[2];
    float rgba[4];
} MinimalVertex;

typedef struct MinimalPush {
    uint64_t vertices_bda;
    uint32_t fb_w;
    uint32_t fb_h;
} MinimalPush;

static_assert(sizeof(MinimalVertex) == 32u, "MinimalVertex must be 32 bytes");
static_assert(sizeof(MinimalPush)   == 16u, "MinimalPush must be 16 bytes");

enum : uint32_t {
    k_blade_count = 6u,
    k_vert_count  = k_blade_count * 3u,
    k_boot_ticks  = 8u,
    k_poke_ticks  = 24u,
};

static constexpr float k_tau = 6.28318530717958647692f;

static VulkanContext   *s_vk              = nullptr;
static VkPipelineLayout s_layout          = VK_NULL_HANDLE;
static VulkanGfxLive    s_pipe_srgb       = {0};
static float            s_center_x        = 0.0f;
static float            s_center_y        = 0.0f;
static float            s_angle_rad       = 0.25f;
static float            s_cursor_phase    = 0.0f;
static uint32_t         s_palette_index   = 0u;
static uint32_t         s_ticks_remaining = 0u;

static void minimal_shutdown(VulkanContext *vk) {
    VulkanContext *ctx = vk != nullptr ? vk : s_vk;
    vulkan_gfx_disarm(ctx, &s_pipe_srgb);
    if (ctx != nullptr && ctx->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx->device, s_layout, nullptr);
    }
    s_layout          = VK_NULL_HANDLE;
    s_vk              = nullptr;
    s_ticks_remaining = 0u;
}

static int minimal_init(VulkanContext *vk, RingCqeFn forward_cqe, void *forward_user) {
    (void)forward_cqe;
    (void)forward_user;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0u,
        .size       = sizeof(MinimalPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &push_range,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[Minimal Error] vkCreatePipelineLayout failed: %s (%d)\n",
                vulkan_vkresult_to_str(res), (int)res);
        return -1;
    }

    VulkanGfxPipe pipe_info = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = VK_FORMAT_UNDEFINED,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_FALSE,
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_ALWAYS,
        .blend         = VK_FALSE,
    };
    vulkan_gfx_arm(&s_pipe_srgb, &pipe_info, VK_FORMAT_B8G8R8A8_SRGB, nullptr);

    s_vk              = vk;
    s_center_x        = 0.0f;
    s_center_y        = 0.0f;
    s_angle_rad       = 0.25f;
    s_cursor_phase    = 0.0f;
    s_palette_index   = 0u;
    s_ticks_remaining = k_boot_ticks;
    return 0;
}

static void palette_accent(uint32_t idx, float phase, float out_rgb[3]) {
    static const float k_accents[4][3] = {
        { 0.14f, 0.62f, 0.96f },
        { 0.94f, 0.42f, 0.16f },
        { 0.18f, 0.82f, 0.52f },
        { 0.78f, 0.34f, 0.92f },
    };
    const float *base = k_accents[idx & 3u];
    float pulse = 0.88f + 0.12f * cosf(phase * k_tau);
    out_rgb[0] = base[0] * pulse;
    out_rgb[1] = base[1] * pulse;
    out_rgb[2] = base[2] * pulse;
}

static void minimal_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    (void)live;
    if (s_vk == nullptr || s_layout == VK_NULL_HANDLE || width == 0u || height == 0u) {
        return;
    }
    VkPipeline pipe = vulkan_gfx_use(s_vk, &s_pipe_srgb);
    if (pipe == VK_NULL_HANDLE) {
        return;
    }

    MinimalVertex verts[k_vert_count];
    float min_dim = (float)(width < height ? width : height);
    float outer_r = fmaxf(min_dim * 0.24f, 24.0f);
    float inner_r = outer_r * 0.42f;
    float accent[3] = {0};
    palette_accent(s_palette_index, s_cursor_phase, accent);

    for (uint32_t i = 0u; i < k_blade_count; ++i) {
        float a0 = s_angle_rad + ((float)i / (float)k_blade_count) * k_tau;
        float a1 = a0 + (0.58f / (float)k_blade_count) * k_tau;
        uint32_t base = i * 3u;

        verts[base + 0u] = (MinimalVertex){
            .pos_px = { s_center_x, s_center_y },
            .rgba   = { 0.96f, 0.97f, 0.99f, 1.0f },
        };
        verts[base + 1u] = (MinimalVertex){
            .pos_px = { s_center_x + cosf(a0) * outer_r, s_center_y + sinf(a0) * outer_r },
            .rgba   = { accent[0], accent[1], accent[2], 1.0f },
        };
        verts[base + 2u] = (MinimalVertex){
            .pos_px = { s_center_x + cosf(a1) * inner_r, s_center_y + sinf(a1) * inner_r },
            .rgba   = { accent[0] * 0.28f, accent[1] * 0.32f, accent[2] * 0.42f, 1.0f },
        };
    }

    BdaSlice slice = bda_alloc_transient(s_vk, (VkDeviceSize)sizeof(verts), 16u);
    if (slice.cpu == nullptr || slice.gpu == 0ull) {
        return;
    }
    memcpy(slice.cpu, verts, sizeof(verts));

    MinimalPush push = {
        .vertices_bda = slice.gpu,
        .fb_w         = width,
        .fb_h         = height,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof(push), &push);
    vkCmdDraw(cmd, k_vert_count, 1u, 0u, 0u);
}

static bool minimal_advance(uint32_t time_ms, const FrameStep *step) {
    if (step == nullptr || step->sim_dt_ms == 0u || s_ticks_remaining == 0u) {
        return false;
    }
    uint32_t steps = step->sim_dt_ms / FRAME_SIM_STEP_MS;
    if (steps == 0u) {
        steps = 1u;
    }
    uint32_t take  = steps < s_ticks_remaining ? steps : s_ticks_remaining;
    uint32_t dt_ms = take * FRAME_SIM_STEP_MS;
    if (dt_ms > step->sim_dt_ms) {
        dt_ms = step->sim_dt_ms;
    }
    s_angle_rad    = fmodf(s_angle_rad + (float)dt_ms * 0.0045f, k_tau);
    s_cursor_phase = (float)(time_ms % 4000u) * (1.0f / 4000.0f);
    s_ticks_remaining -= take;
    return true;
}

static bool minimal_busy(void) {
    return s_ticks_remaining > 0u;
}

static bool minimal_click(float origin_x, float origin_y) {
    s_center_x        = origin_x;
    s_center_y        = origin_y;
    s_palette_index   = (s_palette_index + 1u) & 3u;
    s_ticks_remaining = k_poke_ticks;
    return true;
}

static bool minimal_key(uint32_t key, EngineKeyContext *ctx) {
    if (key == KEY_ESC || key == KEY_Q) {
        if (ctx != nullptr) {
            if (ctx->run_rc != nullptr) {
                *ctx->run_rc = 0;
            }
            if (ctx->why != nullptr) {
                *ctx->why = "quit key";
            }
            if (ctx->stop != nullptr) {
                *ctx->stop = true;
            }
        }
        return false;
    }
    if (key == KEY_R || key == KEY_HOME) {
        s_center_x        = 0.0f;
        s_center_y        = 0.0f;
        s_ticks_remaining = k_poke_ticks;
        return true;
    }
    if (key == KEY_SPACE || (key >= KEY_1 && key <= KEY_4)) {
        if (key >= KEY_1 && key <= KEY_4) {
            s_palette_index = (key - KEY_1) & 3u;
        } else {
            s_palette_index = (s_palette_index + 1u) & 3u;
        }
        s_ticks_remaining = k_poke_ticks;
        return true;
    }
    return false;
}

static const EngineApp k_app = {
    .init     = minimal_init,
    .shutdown = minimal_shutdown,
    .draw     = minimal_draw,
    .advance  = minimal_advance,
    .busy     = minimal_busy,
    .click    = minimal_click,
    .key      = minimal_key,
};

const EngineApp *engine_app(void) {
    return &k_app;
}
