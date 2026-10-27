#include "frame/hud.h"
#include "frame/pass.h"
#include "frame/world.h"
#include "ring/ring.h"
#include "vulkan/pipe.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_vert_spv[] = {
#embed "hud.vert.spv"
};
alignas(uint32_t) static const unsigned char k_frag_spv[] = {
#embed "hud.frag.spv"
};

static_assert(sizeof k_vert_spv % 4u == 0u, "hud vert spirv");
static_assert(sizeof k_frag_spv % 4u == 0u, "hud frag spirv");

#include "hud_font.inc"

static_assert(sizeof k_font == 95u * 8u, "hud font bytes");

// I915_FORMAT_MOD_Y_TILED. This machine presents that general modifier.
static const uint64_t k_intel_y_tile = 0x0100000000000002ull;

typedef struct HudPush {
    uint32_t fb_w;
    uint32_t fb_h;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t rows0;
    uint32_t rows1;
    uint32_t rgb;
    uint32_t solid;
} HudPush;

static_assert(offsetof(HudPush, rows0) == 24u, "hud rows");
static_assert(offsetof(HudPush, rgb) == 32u, "hud rgb");
static_assert(sizeof(HudPush) == 40u, "hud push");

enum { k_text_cap = 80 };

static VkPipelineLayout      s_layout = VK_NULL_HANDLE;
static VkPipeline            s_srgb = VK_NULL_HANDLE;
static VkPipeline            s_unorm = VK_NULL_HANDLE;
static VkPipeline            s_scene_srgb = VK_NULL_HANDLE;
static VkPipeline            s_scene_unorm = VK_NULL_HANDLE;
static VkFormat              s_scene_depth = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
static char             s_text[k_text_cap + 1];
static uint32_t         s_nchars = 0;
static uint32_t         s_cqe_count = 0;
static uint32_t         s_cqe_rate = 0;
static uint64_t         s_cqe_t0 = 0;
static uint32_t         s_last_ms = 0;
static uint32_t         s_shown_us = 0;
static uint64_t         s_last_mono = 0;
static bool             s_have_ms = false;
static bool             s_have_mono = false;
// One revolution of callback-paced presents. The period is learned from the
// short steps, so a 16/17 ms pair at 60 Hz is not a miss and a 33 ms step is.
static bool             s_seq_open = false;
static uint32_t         s_turn_frames = 0;
static uint32_t         s_turn_intervals = 0;
static uint32_t         s_turn_missed = 0;
static uint32_t         s_turn_sum_ms = 0;
static uint32_t         s_period_ms = 0;

static void hud_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Hud Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static int make_pair(VulkanContext *vk, VkSampleCountFlagBits samples, VkFormat depth,
                     VkPipeline *srgb, VkPipeline *unorm) {
    uint32_t flag[2] = { 1u, 0u };
    VkSpecializationMapEntry entry = {
        .constantID = 0,
        .offset     = 0,
        .size       = sizeof(uint32_t),
    };
    VkSpecializationInfo spec[2] = {
        {
            .mapEntryCount = 1,
            .pMapEntries   = &entry,
            .dataSize      = sizeof(uint32_t),
            .pData         = &flag[0],
        },
        {
            .mapEntryCount = 1,
            .pMapEntries   = &entry,
            .dataSize      = sizeof(uint32_t),
            .pData         = &flag[1],
        },
    };
    const VkSpecializationInfo *specs[2] = { &spec[0], &spec[1] };
    VkFormat formats[2] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM };
    VulkanGfxPipe info = {
        .vert          = k_vert_spv,
        .vert_bytes    = sizeof k_vert_spv,
        .frag          = k_frag_spv,
        .frag_bytes    = sizeof k_frag_spv,
        .layout        = s_layout,
        .depth_format  = depth,
        .samples       = samples,
        .cull          = VK_CULL_MODE_NONE,
        .depth_test    = VK_FALSE,
        .depth_write   = VK_FALSE,
        .depth_compare = VK_COMPARE_OP_LESS,
        .blend         = VK_FALSE,
    };
    VkPipeline out[2] = {0};
    if (vulkan_gfx_pipelines(vk, &info, formats, specs, 2, out) != 0) {
        return -1;
    }
    *srgb = out[0];
    *unorm = out[1];
    return 0;
}

void hud_shutdown(VulkanContext *vk) {
    s_scene_depth = VK_FORMAT_UNDEFINED;
    s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        s_layout = VK_NULL_HANDLE;
        s_srgb = VK_NULL_HANDLE;
        s_unorm = VK_NULL_HANDLE;
        s_scene_srgb = VK_NULL_HANDLE;
        s_scene_unorm = VK_NULL_HANDLE;
        return;
    }
    if (s_scene_srgb != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_scene_srgb, nullptr);
        s_scene_srgb = VK_NULL_HANDLE;
    }
    if (s_scene_unorm != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_scene_unorm, nullptr);
        s_scene_unorm = VK_NULL_HANDLE;
    }
    if (s_srgb != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_srgb, nullptr);
        s_srgb = VK_NULL_HANDLE;
    }
    if (s_unorm != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk->device, s_unorm, nullptr);
        s_unorm = VK_NULL_HANDLE;
    }
    if (s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
        s_layout = VK_NULL_HANDLE;
    }
}

int hud_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_layout != VK_NULL_HANDLE) {
        return 0;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(HudPush)) {
        fprintf(stderr, "[Hud Error] Push constants are %u bytes, the readout needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(HudPush));
        return -1;
    }
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(HudPush),
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push,
    };
    VkResult res = vkCreatePipelineLayout(vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        hud_fail("vkCreatePipelineLayout", res);
        hud_shutdown(vk);
        return -1;
    }
    if (make_pair(vk, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, &s_srgb, &s_unorm) != 0 ||
        make_pair(vk, pass_raster_samples(), pass_depth_format(), &s_scene_srgb, &s_scene_unorm) != 0) {
        hud_shutdown(vk);
        return -1;
    }
    s_scene_depth = pass_depth_format();
    s_scene_samples = pass_raster_samples();
    if (s_srgb == VK_NULL_HANDLE || s_unorm == VK_NULL_HANDLE) {
        hud_shutdown(vk);
        return -1;
    }
    printf("[Hud] 8x8 readout on presented frames\n");
    fflush(stdout);
    return 0;
}

void hud_note_cqe(void) {
    s_cqe_count++;
}

static uint32_t cqe_rate(void) {
    uint64_t now = ring_mono_ns();
    if (s_cqe_t0 == 0) {
        s_cqe_t0 = now;
        return s_cqe_rate;
    }
    uint64_t dt = now - s_cqe_t0;
    if (dt < 250000000ull) {
        return s_cqe_rate;
    }
    s_cqe_rate = (uint32_t)((uint64_t)s_cqe_count * 1000000000ull / dt);
    s_cqe_count = 0;
    s_cqe_t0 = now;
    return s_cqe_rate;
}

static void note_interval(uint32_t delta_ms) {
    if (delta_ms == 0) {
        return;
    }
    if (s_period_ms == 0) {
        s_period_ms = delta_ms;
    } else if (delta_ms + 1u < s_period_ms) {
        // The first step was late. This shorter one is the refresh.
        uint32_t late = s_period_ms / delta_ms;
        if (late >= 2u) {
            s_turn_missed += late - 1u;
        }
        s_period_ms = delta_ms;
    } else if (delta_ms > s_period_ms + s_period_ms / 2u) {
        uint32_t steps = (delta_ms + s_period_ms / 2u) / s_period_ms;
        if (steps < 2u) {
            steps = 2u;
        }
        s_turn_missed += steps - 1u;
    } else {
        s_period_ms = (s_period_ms * 3u + delta_ms) / 4u;
        if (s_period_ms == 0) {
            s_period_ms = delta_ms;
        }
    }
    s_turn_intervals++;
    s_turn_sum_ms += delta_ms;
}

void hud_turn_report(uint32_t *frames, uint32_t *missed) {
    if (frames != nullptr) {
        *frames = s_turn_frames;
    }
    if (missed != nullptr) {
        *missed = s_turn_missed;
    }
    s_seq_open = false;
    s_turn_frames = 0;
    s_turn_intervals = 0;
    s_turn_missed = 0;
    s_turn_sum_ms = 0;
    s_period_ms = 0;
}

void hud_prepare(uint32_t frame_ms, bool have_stamp, uint64_t modifier) {
    uint64_t now = ring_mono_ns();
    uint32_t us = s_shown_us;
    if (have_stamp) {
        if (!s_seq_open) {
            s_seq_open = true;
            s_turn_frames = 1;
            s_turn_intervals = 0;
            s_turn_missed = 0;
            s_turn_sum_ms = 0;
            s_period_ms = 0;
        } else if (s_have_ms && frame_ms != s_last_ms) {
            uint32_t delta = frame_ms - s_last_ms;
            // A dropped callback between revolutions is not a skipped refresh.
            if (delta > 1000u) {
                s_turn_frames = 1;
                s_turn_intervals = 0;
                s_turn_missed = 0;
                s_turn_sum_ms = 0;
                s_period_ms = 0;
            } else {
                s_turn_frames++;
                note_interval(delta);
            }
        }
        if (s_turn_intervals > 0 && s_turn_sum_ms > 0) {
            us = (uint32_t)(s_turn_sum_ms * 1000ull / s_turn_intervals);
        }
        s_last_ms = frame_ms;
        s_have_ms = true;
    } else if (!s_seq_open && s_have_mono) {
        uint64_t delta = (now - s_last_mono) / 1000ull;
        us = delta > 9999999ull ? 9999999u : (uint32_t)delta;
    } else if (!s_have_mono) {
        us = 0;
    }
    s_shown_us = us;
    s_last_mono = now;
    s_have_mono = true;

    uint32_t fps = 0;
    if (s_seq_open && s_turn_intervals > 0 && s_turn_sum_ms > 0) {
        fps = (uint32_t)((s_turn_intervals * 1000ull + s_turn_sum_ms / 2ull) / s_turn_sum_ms);
    } else if (us >= 1000u) {
        fps = 1000000u / us;
    }
    const char *kind = "mod";
    if (modifier == 0) {
        kind = "linear";
    } else if (modifier == k_intel_y_tile) {
        kind = "ytile";
    }
    uint32_t objects = 0;
    uint32_t verts = 0;
    world_stats(&objects, &verts);
    int n = snprintf(s_text, sizeof s_text, "%uus %ufps miss %u %s 0x%llx %ucq/s obj %u v %u",
                     us, fps, s_turn_missed, kind, (unsigned long long)modifier,
                     cqe_rate(), objects, verts);
    if (n < 0) {
        s_nchars = 0;
        s_text[0] = '\0';
        return;
    }
    s_nchars = (uint32_t)n;
    if (s_nchars > k_text_cap) {
        s_nchars = k_text_cap;
        s_text[k_text_cap] = '\0';
    }
}

static void draw_quad(VkCommandBuffer cmd, HudPush push) {
    vulkan_cmd_push(cmd, s_layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

static VkPipeline pick_pipeline(VkFormat format, VkFormat depth_format,
                                VkSampleCountFlagBits samples) {
    bool scene = depth_format != VK_FORMAT_UNDEFINED;
    if (scene && (depth_format != s_scene_depth || samples != s_scene_samples)) {
        return VK_NULL_HANDLE;
    }
    if (format == VK_FORMAT_B8G8R8A8_SRGB) {
        return scene ? s_scene_srgb : s_srgb;
    }
    if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        return scene ? s_scene_unorm : s_unorm;
    }
    return VK_NULL_HANDLE;
}

void hud_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, VkFormat format,
              VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (cmd == VK_NULL_HANDLE || s_layout == VK_NULL_HANDLE || s_nchars == 0 ||
        width < 80u || height < 48u) {
        return;
    }
    VkPipeline pipeline = pick_pipeline(format, depth_format, samples);
    if (pipeline == VK_NULL_HANDLE) {
        return;
    }
    uint32_t n = s_nchars;
    uint32_t max_chars = (width - 20u - 4u) / 8u;
    if (n > max_chars) {
        n = max_chars;
    }
    if (n == 0) {
        return;
    }
    uint32_t plate_w = n * 8u + 4u;
    uint32_t plate_h = 12u;
    uint32_t plate_x = 10u;
    uint32_t plate_y = height - 10u - plate_h;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    HudPush plate = {
        .fb_w  = width,
        .fb_h  = height,
        .x     = plate_x,
        .y     = plate_y,
        .w     = plate_w,
        .h     = plate_h,
        .solid = 1u,
    };
    draw_quad(cmd, plate);

    for (uint32_t i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s_text[i];
        if (ch < 32u || ch > 126u) {
            continue;
        }
        const uint8_t *rows = k_font[ch - 32u];
        bool blank = true;
        for (int r = 0; r < 8; ++r) {
            if (rows[r] != 0) {
                blank = false;
                break;
            }
        }
        if (blank) {
            continue;
        }
        HudPush glyph = {
            .fb_w  = width,
            .fb_h  = height,
            .x     = plate_x + 2u + i * 8u,
            .y     = plate_y + 2u,
            .w     = 8u,
            .h     = 8u,
            .rows0 = (uint32_t)rows[0] | ((uint32_t)rows[1] << 8u) |
                     ((uint32_t)rows[2] << 16u) | ((uint32_t)rows[3] << 24u),
            .rows1 = (uint32_t)rows[4] | ((uint32_t)rows[5] << 8u) |
                     ((uint32_t)rows[6] << 16u) | ((uint32_t)rows[7] << 24u),
            .rgb   = 0x00f4f1eau,
            .solid = 0u,
        };
        draw_quad(cmd, glyph);
    }
}
