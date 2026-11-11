#include "frame/hud.h"
#include "frame/camera.h"
#include "frame/frame.h"
#include "frame/pass.h"
#include "frame/world.h"
#include "ring/ring.h"
#include "vulkan/bda.h"
#include "vulkan/pipe.h"
#include <math.h>
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

typedef struct HudInstance {
    uint32_t x, y, w, h;
    uint32_t rows0, rows1, rgb, solid;
} HudInstance;

typedef struct HudPush {
    uint64_t instances;
    uint32_t fb_w;
    uint32_t fb_h;
} HudPush;

static_assert(sizeof(HudInstance) == 32u, "hud instance");
static_assert(sizeof(HudPush) == 16u, "hud push");

static VulkanContext *s_vk = nullptr;

enum { k_hud_lines = 5, k_line_cap = 48 };

static const uint32_t k_flag_srgb = 1u;
static const uint32_t k_flag_unorm = 0u;
static const VkSpecializationMapEntry k_hud_entry = {
    .constantID = 0,
    .offset     = 0,
    .size       = sizeof(uint32_t),
};
static const VkSpecializationInfo k_spec_srgb = {
    .mapEntryCount = 1,
    .pMapEntries   = &k_hud_entry,
    .dataSize      = sizeof k_flag_srgb,
    .pData         = &k_flag_srgb,
};
static const VkSpecializationInfo k_spec_unorm = {
    .mapEntryCount = 1,
    .pMapEntries   = &k_hud_entry,
    .dataSize      = sizeof k_flag_unorm,
    .pData         = &k_flag_unorm,
};

static VkPipelineLayout s_layout = VK_NULL_HANDLE;
static VulkanGfxLive    s_overlay_srgb;
static VulkanGfxLive    s_overlay_unorm;
static VulkanGfxLive    s_scene_srgb;
static VulkanGfxLive    s_scene_unorm;
static VkFormat              s_scene_depth = VK_FORMAT_UNDEFINED;
static VkSampleCountFlagBits s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
static char             s_line[k_hud_lines][k_line_cap + 1];
static uint32_t         s_line_n[k_hud_lines];
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
static bool             s_visible = false;
static bool             s_logged = false;

void hud_set_visible(bool visible) {
    s_visible = visible;
}

bool hud_is_visible(void) {
    return s_visible;
}

static void hud_fail(const char *what, VkResult res) {
    fprintf(stderr, "[Hud Error] %s: %s (%d)\n", what, vulkan_vkresult_to_str(res), (int)res);
}

static void arm_pair(VkSampleCountFlagBits samples, VkFormat depth,
                     VulkanGfxLive *srgb, VulkanGfxLive *unorm) {
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
    vulkan_gfx_arm(srgb, &info, VK_FORMAT_B8G8R8A8_SRGB, &k_spec_srgb);
    vulkan_gfx_arm(unorm, &info, VK_FORMAT_B8G8R8A8_UNORM, &k_spec_unorm);
}

void hud_shutdown(VulkanContext *vk) {
    s_scene_depth = VK_FORMAT_UNDEFINED;
    s_scene_samples = VK_SAMPLE_COUNT_1_BIT;
    vulkan_gfx_disarm(vk, &s_scene_srgb);
    vulkan_gfx_disarm(vk, &s_scene_unorm);
    vulkan_gfx_disarm(vk, &s_overlay_srgb);
    vulkan_gfx_disarm(vk, &s_overlay_unorm);
    if (vk != nullptr && vk->device != VK_NULL_HANDLE && s_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(vk->device, s_layout, nullptr);
    }
    s_layout = VK_NULL_HANDLE;
    s_vk = nullptr;
}

int hud_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE || vk->physical_device == VK_NULL_HANDLE) {
        return -1;
    }
    s_vk = vk;
    return 0;
}

static bool hud_ensure_ready(void) {
    if (s_layout != VK_NULL_HANDLE) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE || s_vk->physical_device == VK_NULL_HANDLE) {
        return false;
    }
    VkPhysicalDeviceProperties props = {0};
    vkGetPhysicalDeviceProperties(s_vk->physical_device, &props);
    if (props.limits.maxPushConstantsSize < sizeof(HudPush)) {
        fprintf(stderr, "[Hud Error] Push constants are %u bytes, the readout needs %zu\n",
                props.limits.maxPushConstantsSize, sizeof(HudPush));
        return false;
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
    VkResult res = vkCreatePipelineLayout(s_vk->device, &layout_info, nullptr, &s_layout);
    if (res != VK_SUCCESS) {
        s_layout = VK_NULL_HANDLE;
        hud_fail("vkCreatePipelineLayout", res);
        return false;
    }
    arm_pair(VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED, &s_overlay_srgb, &s_overlay_unorm);
    arm_pair(pass_raster_samples(), pass_depth_format(), &s_scene_srgb, &s_scene_unorm);
    s_scene_depth = pass_depth_format();
    s_scene_samples = pass_raster_samples();
    if (!s_logged) {
        printf("[Hud] 8x8 readout on presented frames\n");
        fflush(stdout);
        s_logged = true;
    }
    return true;
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

    if (!s_visible) {
        return;
    }

    uint32_t fps = 0;
    if (s_seq_open && s_turn_intervals > 0 && s_turn_sum_ms > 0) {
        fps = (uint32_t)((s_turn_intervals * 1000ull + s_turn_sum_ms / 2ull) / s_turn_sum_ms);
    } else if (us >= 1000u) {
        fps = 1000000u / us;
    }
    char kind[32];
    if (modifier == 0) {
        snprintf(kind, sizeof kind, "linear");
    } else if (modifier == k_intel_y_tile) {
        snprintf(kind, sizeof kind, "ytile");
    } else {
        snprintf(kind, sizeof kind, "mod 0x%llx", (unsigned long long)modifier);
    }
    char zoom_text[16];
    float zoom = camera_zoom_shown();
    if (!isfinite(zoom) || zoom <= 0.0f) {
        snprintf(zoom_text, sizeof zoom_text, "0");
    } else if (zoom >= 100.0f) {
        snprintf(zoom_text, sizeof zoom_text, "%.0f", (double)zoom);
    } else if (zoom >= 10.0f) {
        snprintf(zoom_text, sizeof zoom_text, "%.1f", (double)zoom);
    } else if (zoom >= 0.01f) {
        snprintf(zoom_text, sizeof zoom_text, "%.2f", (double)zoom);
    } else {
        snprintf(zoom_text, sizeof zoom_text, "%.4f", (double)zoom);
    }
    uint32_t objects = 0;
    uint32_t verts = 0;
    world_stats(&objects, &verts);
    uint32_t comp_us = 0;
    uint32_t rend_us = 0;
    frame_gpu_timings(&comp_us, &rend_us, nullptr);
    BdaStats bda = {0};
    bda_get_stats(&bda);
    double bda_mib = (double)(bda.device_allocated + bda.staging_allocated) / (1024.0 * 1024.0);
    int wrote[k_hud_lines];
    wrote[0] = snprintf(s_line[0], sizeof s_line[0], "%u fps  %u us", fps, us);
    wrote[1] = snprintf(s_line[1], sizeof s_line[1], "miss %u  %u cq/s", s_turn_missed, cqe_rate());
    wrote[2] = snprintf(s_line[2], sizeof s_line[2], "obj %u  vert %u", objects, verts);
    wrote[3] = snprintf(s_line[3], sizeof s_line[3], "%s  zoom %s", kind, zoom_text);
    wrote[4] = snprintf(s_line[4], sizeof s_line[4], "gpu %u+%u us  bda %.1fM", comp_us, rend_us, bda_mib);
    for (uint32_t row = 0; row < k_hud_lines; ++row) {
        if (wrote[row] < 0) {
            s_line[row][0] = '\0';
            s_line_n[row] = 0;
            continue;
        }
        uint32_t n = (uint32_t)wrote[row];
        if (n > k_line_cap) {
            n = k_line_cap;
            s_line[row][k_line_cap] = '\0';
        }
        s_line_n[row] = n;
    }
}

static VkPipeline pick_pipeline(VkFormat format, VkFormat depth_format,
                                VkSampleCountFlagBits samples) {
    bool scene = depth_format != VK_FORMAT_UNDEFINED;
    if (scene && (depth_format != s_scene_depth || samples != s_scene_samples)) {
        return VK_NULL_HANDLE;
    }
    VulkanGfxLive *live = nullptr;
    if (format == VK_FORMAT_B8G8R8A8_SRGB) {
        live = scene ? &s_scene_srgb : &s_overlay_srgb;
    } else if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        live = scene ? &s_scene_unorm : &s_overlay_unorm;
    }
    if (live == nullptr) {
        return VK_NULL_HANDLE;
    }
    return vulkan_gfx_use(s_vk, live);
}

void hud_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, VkFormat format,
              VkFormat depth_format, VkSampleCountFlagBits samples) {
    if (!s_visible || cmd == VK_NULL_HANDLE || s_vk == nullptr ||
        width < 80u || height < 64u || !hud_ensure_ready()) {
        return;
    }
    VkPipeline pipeline = pick_pipeline(format, depth_format, samples);
    if (pipeline == VK_NULL_HANDLE) return;

    uint32_t longest = 0;
    for (uint32_t row = 0; row < k_hud_lines; ++row) {
        if (s_line_n[row] > longest) {
            longest = s_line_n[row];
        }
    }
    uint32_t max_chars = (width - 20u - 4u) / 8u;
    if (longest > max_chars) {
        longest = max_chars;
    }
    if (longest == 0) return;

    uint32_t cap = 1u + k_hud_lines * k_line_cap;
    BdaSlice slice = bda_alloc_transient(s_vk, (VkDeviceSize)cap * sizeof(HudInstance), 16u);
    if (slice.cpu == nullptr || slice.gpu == 0) return;

    HudInstance *inst = (HudInstance *)slice.cpu;
    uint32_t plate_w = longest * 8u + 4u;
    uint32_t plate_h = k_hud_lines * 8u + 4u;
    uint32_t plate_x = 10u;
    uint32_t plate_y = height - 10u - plate_h;

    uint32_t count = 0;
    inst[count++] = (HudInstance){
        .x = plate_x, .y = plate_y, .w = plate_w, .h = plate_h, .solid = 1u,
    };

    for (uint32_t row = 0; row < k_hud_lines; ++row) {
        uint32_t n = s_line_n[row];
        if (n > max_chars) {
            n = max_chars;
        }
        for (uint32_t i = 0; i < n; ++i) {
            unsigned char ch = (unsigned char)s_line[row][i];
            if (ch < 32u || ch > 126u) continue;
            const uint8_t *rows = k_font[ch - 32u];
            if ((rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7]) == 0) {
                continue;
            }
            if (count >= cap) {
                break;
            }
            inst[count++] = (HudInstance){
                .x     = plate_x + 2u + i * 8u,
                .y     = plate_y + 2u + row * 8u,
                .w     = 8u,
                .h     = 8u,
                .rows0 = (uint32_t)rows[0] | ((uint32_t)rows[1] << 8u) |
                         ((uint32_t)rows[2] << 16u) | ((uint32_t)rows[3] << 24u),
                .rows1 = (uint32_t)rows[4] | ((uint32_t)rows[5] << 8u) |
                         ((uint32_t)rows[6] << 16u) | ((uint32_t)rows[7] << 24u),
                .rgb   = 0x00f4f1eau,
                .solid = 0u,
            };
        }
    }

    HudPush push = { .instances = slice.gpu, .fb_w = width, .fb_h = height };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vulkan_cmd_push(cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);
    vkCmdDraw(cmd, 6, count, 0, 0);
}
