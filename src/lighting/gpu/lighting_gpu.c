#include "lighting/lighting.h"
#include "vulkan/bda.h"
#include "vulkan/core.h"
#include "vulkan/pipe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

alignas(uint32_t) static const unsigned char k_gbuf_comp_spv[] = {
#embed "lighting_gbuf.comp.spv"
};
alignas(uint32_t) static const unsigned char k_hrc_comp_spv[] = {
#embed "lighting_hrc.comp.spv"
};
alignas(uint32_t) static const unsigned char k_ssilvb_comp_spv[] = {
#embed "lighting_ssilvb.comp.spv"
};

static_assert(sizeof k_gbuf_comp_spv % 4u == 0u, "lighting_gbuf.comp.spv align");
static_assert(sizeof k_hrc_comp_spv % 4u == 0u, "lighting_hrc.comp.spv align");
static_assert(sizeof k_ssilvb_comp_spv % 4u == 0u, "lighting_ssilvb.comp.spv align");

enum : uint32_t {
    k_env_ring_slots = 4u,
};

typedef struct LightingComputePush {
    uint64_t env_bda;
    uint32_t cascade_idx;
    uint32_t flags;
} LightingComputePush;

static_assert(sizeof(LightingComputePush) == 16u, "LightingComputePush must be 16 bytes");

static VulkanContext   *s_vk             = nullptr;
static VkPipelineLayout s_comp_layout    = VK_NULL_HANDLE;
static VkPipeline       s_pipe_gbuf      = VK_NULL_HANDLE;
static VkPipeline       s_pipe_hrc       = VK_NULL_HANDLE;
static VkPipeline       s_pipe_ssilvb    = VK_NULL_HANDLE;

static DaBuffer         s_env_buf        = {0};
static DaBuffer         s_gi_slab        = {0};
static uint32_t         s_env_slot       = 0u;
static bool             s_env_ready      = false;

static LightingMatSlot  s_mat_cpu[K_LIGHTING_MAT_SLOTS];
static LightingProxy    s_proxies[K_LIGHTING_MAX_PROXIES];
static uint32_t         s_proxy_count    = 0u;

static bool             s_gi_user_on     = true;
static bool             s_gi_frame_live  = false;
static uint32_t         s_oracle_mode    = LIGHTING_ORACLE_ANALYTICAL_UMA;
static uint32_t         s_frame_index    = 0u;
static uint32_t         s_ping_index     = 0u;
static bool             s_history_valid  = false;
static bool             s_have_prev_cam  = false;
static bool             s_logged_init    = false;
static bool             s_logged_gi      = false;

static uint32_t         s_full_w         = 0u;
static uint32_t         s_full_h         = 0u;
static uint32_t         s_gi_w           = 0u;
static uint32_t         s_gi_h           = 0u;
static uint32_t         s_hrc_w0         = 0u;
static uint32_t         s_hrc_h0         = 0u;
static uint32_t         s_hrc_offsets[4] = {0};
static uint32_t         s_hrc_total_rays = 0u;
static uint32_t         s_ssilvb_slices  = 2u;
static uint32_t         s_ssilvb_steps   = 6u;

static VkDeviceSize     s_off_gbuf       = 0u;
static VkDeviceSize     s_off_hrc        = 0u;
static VkDeviceSize     s_off_gi[2]      = {0u, 0u};

static LightingCameraParams s_curr_cam   = { .qw = 1.0f, .zoom = 1.0f };
static LightingCameraParams s_prev_cam   = { .qw = 1.0f, .zoom = 1.0f };

static bool             s_has_ground     = false;
static float            s_ground_z       = 0.0f;
static float            s_ground_alb[3]  = { 0.08f, 0.20f, 0.38f };

static uint64_t         s_terrain_bda    = 0ull;
static float            s_elev_base_m    = 0.0f;
static float            s_vert_scale     = 1.0f;
static uint64_t         s_scene_hiz_bda  = 0ull;

static VkDeviceSize align_up_size(VkDeviceSize v, VkDeviceSize a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

static void parse_gi_env(void) {
    const char *env = getenv("ENGINE_GI");
    if (env == nullptr || env[0] == '\0') {
        s_gi_user_on = true;
        return;
    }
    if (strcasecmp(env, "0") == 0 || strcasecmp(env, "off") == 0 ||
        strcasecmp(env, "false") == 0 || strcasecmp(env, "no") == 0) {
        s_gi_user_on = false;
    } else {
        s_gi_user_on = true;
    }
}

static void select_profile_params(const VulkanContext *vk) {
    if (vk != nullptr && vk->device_info.execution_profile != VULKAN_PROFILE_UMA_IGPU) {
        s_oracle_mode   = LIGHTING_ORACLE_DISCRETE_HIGH;
        s_ssilvb_slices = 4u;
        s_ssilvb_steps  = 8u;
    } else {
        s_oracle_mode   = LIGHTING_ORACLE_ANALYTICAL_UMA;
        s_ssilvb_slices = 2u;
        s_ssilvb_steps  = 6u;
    }
}

static bool ensure_env_buffer(void) {
    if (s_env_ready && s_env_buf.buffer != VK_NULL_HANDLE && s_env_buf.map != nullptr) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    const VkDeviceSize total = (VkDeviceSize)k_env_ring_slots * K_LIGHTING_ENV_SLICE_SIZE;
    const VkBufferUsageFlags env_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(s_vk, total, env_usage, true, &s_env_buf) != 0 ||
        s_env_buf.map == nullptr || s_env_buf.address == 0ull) {
        return false;
    }
    memset(s_env_buf.map, 0, (size_t)total);
    uint8_t *base = (uint8_t *)s_env_buf.map;
    for (uint32_t i = 0u; i < k_env_ring_slots; ++i) {
        memcpy(base + (size_t)i * K_LIGHTING_ENV_SLICE_SIZE, s_mat_cpu, sizeof s_mat_cpu);
    }
    s_env_ready = true;
    return true;
}

static bool ensure_compute_pipelines(void) {
    if (s_pipe_gbuf != VK_NULL_HANDLE &&
        s_pipe_hrc != VK_NULL_HANDLE &&
        s_pipe_ssilvb != VK_NULL_HANDLE) {
        return true;
    }
    if (s_vk == nullptr || s_vk->device == VK_NULL_HANDLE) {
        return false;
    }
    if (s_comp_layout == VK_NULL_HANDLE) {
        VkPushConstantRange range = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0u,
            .size       = sizeof(LightingComputePush),
        };
        VkPipelineLayoutCreateInfo info = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1u,
            .pPushConstantRanges    = &range,
        };
        if (vkCreatePipelineLayout(s_vk->device, &info, nullptr, &s_comp_layout) != VK_SUCCESS) {
            s_comp_layout = VK_NULL_HANDLE;
            return false;
        }
    }

    if (s_pipe_gbuf == VK_NULL_HANDLE) {
        s_pipe_gbuf = vulkan_compute_pipeline(s_vk, k_gbuf_comp_spv, sizeof k_gbuf_comp_spv, s_comp_layout);
    }
    if (s_pipe_hrc == VK_NULL_HANDLE) {
        s_pipe_hrc = vulkan_compute_pipeline(s_vk, k_hrc_comp_spv, sizeof k_hrc_comp_spv, s_comp_layout);
    }
    if (s_pipe_ssilvb == VK_NULL_HANDLE) {
        s_pipe_ssilvb = vulkan_compute_pipeline(s_vk, k_ssilvb_comp_spv, sizeof k_ssilvb_comp_spv, s_comp_layout);
    }

    return s_pipe_gbuf != VK_NULL_HANDLE &&
           s_pipe_hrc != VK_NULL_HANDLE &&
           s_pipe_ssilvb != VK_NULL_HANDLE;
}

static bool ensure_gi_slab(uint32_t width, uint32_t height) {
    if (s_vk == nullptr || width == 0u || height == 0u) {
        return false;
    }

    uint32_t target_gi_w  = 0u;
    uint32_t target_gi_h  = 0u;
    uint32_t target_hrc_w = 0u;
    uint32_t target_hrc_h = 0u;
    uint32_t hrc_offsets[K_LIGHTING_MAX_CASCADES] = {0};
    const uint32_t ray_off = lighting_compute_hrc_rays(width, height, s_oracle_mode,
                                                       &target_gi_w, &target_gi_h,
                                                       &target_hrc_w, &target_hrc_h,
                                                       hrc_offsets);

    if (s_gi_slab.buffer != VK_NULL_HANDLE &&
        s_full_w == width && s_full_h == height &&
        s_gi_w == target_gi_w && s_gi_h == target_gi_h &&
        s_hrc_w0 == target_hrc_w && s_hrc_h0 == target_hrc_h) {
        return true;
    }

    for (uint32_t c = 0u; c < K_LIGHTING_MAX_CASCADES; ++c) {
        s_hrc_offsets[c] = hrc_offsets[c];
    }

    const VkDeviceSize gbuf_bytes = (VkDeviceSize)target_gi_w * target_gi_h * sizeof(LightingGbufTexel);
    const VkDeviceSize hrc_bytes  = (VkDeviceSize)ray_off * 8u; // sizeof(LightingHrcRayGpu) == 8B
    const VkDeviceSize gi_bytes   = (VkDeviceSize)target_gi_w * target_gi_h * sizeof(LightingGiTexel);

    VkDeviceSize cursor = 0u;
    s_off_gbuf  = cursor;
    cursor      = align_up_size(cursor + gbuf_bytes, 256u);
    s_off_hrc   = cursor;
    cursor      = align_up_size(cursor + hrc_bytes,  256u);
    s_off_gi[0] = cursor;
    cursor      = align_up_size(cursor + gi_bytes,   256u);
    s_off_gi[1] = cursor;
    cursor      = align_up_size(cursor + gi_bytes,   256u);

    if (s_gi_slab.buffer != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(s_vk->device);
        da_buffer_destroy(s_vk, &s_gi_slab);
    }

    const VkBufferUsageFlags gi_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(s_vk, cursor, gi_usage, false, &s_gi_slab) != 0 ||
        s_gi_slab.address == 0ull) {
        da_buffer_destroy(s_vk, &s_gi_slab);
        return false;
    }

    s_full_w         = width;
    s_full_h         = height;
    s_gi_w           = target_gi_w;
    s_gi_h           = target_gi_h;
    s_hrc_w0         = target_hrc_w;
    s_hrc_h0         = target_hrc_h;
    s_hrc_total_rays = ray_off;
    s_history_valid  = false;
    s_ping_index     = 0u;

    if (!s_logged_gi) {
        printf("[Lighting] 2026 SSILVB (%ux%u, %u slices x %u steps, 32-bit bitmask) + "
               "4-Level Split HRC (%ux%u C0 probes, %u rays, oracle=%s, %.2f MiB BDA)\n",
               s_gi_w, s_gi_h, s_ssilvb_slices, s_ssilvb_steps,
               s_hrc_w0, s_hrc_h0, s_hrc_total_rays,
               s_oracle_mode == LIGHTING_ORACLE_DISCRETE_HIGH ? "Discrete-High/RT" : "UMA-Analytical+DEM",
               (double)cursor / (1024.0 * 1024.0));
        fflush(stdout);
        s_logged_gi = true;
    }
    return true;
}

static void cmd_compute_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dst_stage,
                                VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = dst_stage,
        .dstAccessMask = dst_access,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

int lighting_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (!lighting_self_check()) {
        fprintf(stderr, "[Lighting Error] CPU mathematical & 32-bit bitmask self-check failed!\n");
        return -1;
    }
    s_vk = vk;
    memset(s_mat_cpu, 0, sizeof s_mat_cpu);
    parse_gi_env();
    select_profile_params(vk);
    lighting_reset();

    if (!s_logged_init) {
        printf("[Lighting] Core GI kernel armed (SSILVB + HRC, GI=%s, RayQuery=%s)\n",
               s_gi_user_on ? "on" : "off",
               vk->device_info.supports_ray_query ? "supported" : "analytical");
        fflush(stdout);
        s_logged_init = true;
    }
    return 0;
}

void lighting_shutdown(VulkanContext *vk) {
    VulkanContext *ctx = vk != nullptr ? vk : s_vk;
    if (ctx != nullptr && ctx->device != VK_NULL_HANDLE) {
        vulkan_pipe_release(ctx, s_pipe_gbuf);
        vulkan_pipe_release(ctx, s_pipe_hrc);
        vulkan_pipe_release(ctx, s_pipe_ssilvb);
        s_pipe_gbuf   = VK_NULL_HANDLE;
        s_pipe_hrc    = VK_NULL_HANDLE;
        s_pipe_ssilvb = VK_NULL_HANDLE;

        if (s_comp_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_comp_layout, nullptr);
            s_comp_layout = VK_NULL_HANDLE;
        }
        da_buffer_destroy(ctx, &s_gi_slab);
        da_buffer_destroy(ctx, &s_env_buf);
    }
    s_env_ready     = false;
    s_gi_frame_live = false;
    s_history_valid = false;
    s_vk            = nullptr;
}

void lighting_invalidate_history(void) {
    s_history_valid = false;
    s_have_prev_cam = false;
}

void lighting_reset(void) {
    memset(s_mat_cpu, 0, sizeof s_mat_cpu);
    if (s_env_ready && s_env_buf.map != nullptr) {
        memset(s_env_buf.map, 0, (size_t)k_env_ring_slots * K_LIGHTING_ENV_SLICE_SIZE);
    }
    s_proxy_count   = 0u;
    s_has_ground    = false;
    s_terrain_bda   = 0ull;
    s_scene_hiz_bda = 0ull;
    s_gi_frame_live = false;
    lighting_invalidate_history();
}

void lighting_material_set(uint32_t slot, float emit_r, float emit_g, float emit_b, float glow) {
    if (slot >= K_LIGHTING_MAT_SLOTS) {
        return;
    }
    s_mat_cpu[slot] = (LightingMatSlot){
        .emit_r = emit_r,
        .emit_g = emit_g,
        .emit_b = emit_b,
        .glow   = glow,
    };
    const bool non_zero = (glow > 0.0f) && (emit_r > 0.0f || emit_g > 0.0f || emit_b > 0.0f);
    if (!s_env_ready && non_zero) {
        (void)ensure_env_buffer();
    }
    if (s_env_ready && s_env_buf.map != nullptr) {
        uint8_t *base = (uint8_t *)s_env_buf.map;
        for (uint32_t i = 0u; i < k_env_ring_slots; ++i) {
            uint8_t *slice = base + (size_t)i * K_LIGHTING_ENV_SLICE_SIZE;
            memcpy(slice + (size_t)slot * sizeof(LightingMatSlot),
                   &s_mat_cpu[slot], sizeof(LightingMatSlot));
        }
    }
}

void lighting_material_get_emissive(uint32_t slot, float out_emit[3]) {
    if (out_emit == nullptr) {
        return;
    }
    if (slot >= K_LIGHTING_MAT_SLOTS) {
        out_emit[0] = 0.0f;
        out_emit[1] = 0.0f;
        out_emit[2] = 0.0f;
        return;
    }
    float g = s_mat_cpu[slot].glow;
    out_emit[0] = s_mat_cpu[slot].emit_r * g;
    out_emit[1] = s_mat_cpu[slot].emit_g * g;
    out_emit[2] = s_mat_cpu[slot].emit_b * g;
}

VkDeviceAddress lighting_env_bda(void) {
    if (!s_env_ready || s_env_buf.address == 0ull) {
        return 0ull;
    }
    return s_env_buf.address + (VkDeviceAddress)s_env_slot * K_LIGHTING_ENV_SLICE_SIZE;
}

void lighting_begin_frame(uint32_t width, uint32_t height, const LightingCameraParams *cam) {
    s_proxy_count   = 0u;
    s_has_ground    = false;
    s_terrain_bda   = 0ull;
    s_scene_hiz_bda = 0ull;
    s_gi_frame_live = false;

    if (width == 0u || height == 0u || cam == nullptr) {
        return;
    }
    if (!ensure_env_buffer()) {
        return;
    }

    s_env_slot = (s_env_slot + 1u) % k_env_ring_slots;
    if (s_have_prev_cam) {
        s_prev_cam = s_curr_cam;
    } else {
        s_prev_cam      = *cam;
        s_have_prev_cam = true;
    }
    s_curr_cam = *cam;
}

void lighting_set_ground_plane(bool enabled, float z_height, const float base_albedo[3]) {
    s_has_ground = enabled;
    s_ground_z   = z_height;
    if (base_albedo != nullptr) {
        s_ground_alb[0] = base_albedo[0];
        s_ground_alb[1] = base_albedo[1];
        s_ground_alb[2] = base_albedo[2];
    }
}

void lighting_set_terrain_field(uint64_t terrain_dir_bda, float elev_base_m, float vertical_scale) {
    s_terrain_bda = terrain_dir_bda;
    s_elev_base_m = elev_base_m;
    s_vert_scale  = vertical_scale;
}

void lighting_set_scene_hiz(uint64_t scene_hiz_bda) {
    s_scene_hiz_bda = scene_hiz_bda;
}

void lighting_submit_proxy(const LightingProxy *proxy) {
    if (proxy == nullptr || s_proxy_count >= K_LIGHTING_MAX_PROXIES) {
        return;
    }
    LightingProxy p = *proxy;
    if (p.bound_radius_f16 == 0u) {
        float ex = fmaxf(p.extents[0], 1.0e-4f);
        float ey = fmaxf(p.extents[1], 1.0e-4f);
        float ez = fmaxf(p.extents[2], 1.0e-4f);
        float br = (p.kind == LIGHTING_PROXY_OBB)
            ? (sqrtf(ex * ex + ey * ey + ez * ez) * 1.015f)
            : (fmaxf(ex, fmaxf(ey, ez)) * 1.015f);
        uint16_t sp = lighting_f32_to_f16(sinf(p.pitch_rad));
        uint16_t cp = lighting_f32_to_f16(cosf(p.pitch_rad));
        uint16_t sy = lighting_f32_to_f16(sinf(p.yaw_rad));
        uint16_t cy = lighting_f32_to_f16(cosf(p.yaw_rad));
        p.bound_radius_f16 = lighting_f32_to_f16(br);
        p.trig_pitch       = (uint32_t)sp | ((uint32_t)cp << 16u);
        p.trig_yaw         = (uint32_t)sy | ((uint32_t)cy << 16u);
    }
    s_proxies[s_proxy_count++] = p;
}

void lighting_prepare(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (cmd == VK_NULL_HANDLE || !s_env_ready || s_env_buf.map == nullptr ||
        width == 0u || height == 0u) {
        return;
    }

    uint8_t       *slice     = (uint8_t *)s_env_buf.map + (size_t)s_env_slot * K_LIGHTING_ENV_SLICE_SIZE;
    VkDeviceAddress slice_bda = lighting_env_bda();

    // Always synchronize material emission look slots into the active ring slice
    memcpy(slice, s_mat_cpu, sizeof s_mat_cpu);

    const bool has_3d_content = (s_proxy_count > 0u) || (s_terrain_bda != 0ull);
    const bool run_gi         = s_gi_user_on && has_3d_content &&
                                ensure_compute_pipelines() && ensure_gi_slab(width, height);

    const uint32_t curr_ping = s_ping_index & 1u;
    const uint32_t prev_ping = (curr_ping ^ 1u);

    float px_world  = 1.0f / fmaxf(s_curr_cam.zoom, 1.0e-4f);
    float thick_w   = (s_terrain_bda != 0ull) ? fmaxf(px_world * 14.0f, 18.0f)
                                              : fmaxf(px_world * 22.0f, 36.0f);
    float gi_boost  = (s_terrain_bda != 0ull) ? 0.92f : 1.18f;
    float ao_str    = (s_terrain_bda != 0ull) ? 0.85f : 0.92f;
    float radius_px = (s_oracle_mode == LIGHTING_ORACLE_DISCRETE_HIGH) ? 52.0f : 42.0f;

    LightingFrameHeader hdr = {
        .magic            = K_LIGHTING_MAGIC,
        .gi_enabled       = run_gi ? 1u : 0u,
        .oracle_mode      = s_oracle_mode,
        .frame_index      = s_frame_index,
        .full_width       = width,
        .full_height      = height,
        .gi_width         = run_gi ? s_gi_w : 0u,
        .gi_height        = run_gi ? s_gi_h : 0u,
        .hrc_w0           = run_gi ? s_hrc_w0 : 0u,
        .hrc_h0           = run_gi ? s_hrc_h0 : 0u,
        .proxy_count      = s_proxy_count,
        .has_ground       = s_has_ground ? 1u : 0u,
        .ground_z         = s_ground_z,
        .ground_r         = s_ground_alb[0],
        .ground_g         = s_ground_alb[1],
        .ground_b         = s_ground_alb[2],
        .proxies_bda      = s_proxy_count > 0u ? (slice_bda + K_LIGHTING_PROXIES_OFFSET) : 0ull,
        .gbuf_bda         = run_gi ? (s_gi_slab.address + s_off_gbuf) : 0ull,
        .hrc_bda          = run_gi ? (s_gi_slab.address + s_off_hrc)  : 0ull,
        .gi_curr_bda      = run_gi ? (s_gi_slab.address + s_off_gi[curr_ping]) : 0ull,
        .gi_prev_bda      = (run_gi && s_history_valid) ? (s_gi_slab.address + s_off_gi[prev_ping]) : 0ull,
        .terrain_dir_bda  = s_terrain_bda,
        .scene_hiz_bda    = s_scene_hiz_bda,
        .elev_base_m      = s_elev_base_m,
        .vertical_scale   = s_vert_scale,
        .history_valid    = (run_gi && s_history_valid) ? 1u : 0u,
        .ssilvb_slices    = s_ssilvb_slices,
        .ssilvb_steps     = s_ssilvb_steps,
        .hrc_cascades     = K_LIGHTING_MAX_CASCADES,
        .qx               = s_curr_cam.qx,
        .qy               = s_curr_cam.qy,
        .qz               = s_curr_cam.qz,
        .qw               = s_curr_cam.qw,
        .cam_x            = s_curr_cam.cam_x,
        .cam_y            = s_curr_cam.cam_y,
        .cam_z            = s_curr_cam.cam_z,
        .zoom             = s_curr_cam.zoom,
        .prev_qx          = s_prev_cam.qx,
        .prev_qy          = s_prev_cam.qy,
        .prev_qz          = s_prev_cam.qz,
        .prev_qw          = s_prev_cam.qw,
        .prev_cam_x       = s_prev_cam.cam_x,
        .prev_cam_y       = s_prev_cam.cam_y,
        .prev_cam_z       = s_prev_cam.cam_z,
        .prev_zoom        = s_prev_cam.zoom,
        .gi_intensity     = gi_boost,
        .ao_strength      = ao_str,
        .ssilvb_radius_px = radius_px,
        .thickness_world  = thick_w,
        .hrc_offset_rays  = {
            s_hrc_offsets[0], s_hrc_offsets[1], s_hrc_offsets[2], s_hrc_offsets[3],
        },
    };

    memcpy(slice + K_LIGHTING_HEADER_OFFSET, &hdr, sizeof hdr);
    if (s_proxy_count > 0u) {
        memcpy(slice + K_LIGHTING_PROXIES_OFFSET,
               s_proxies, (size_t)s_proxy_count * sizeof(LightingProxy));
    }

    if (!run_gi) {
        s_gi_frame_live = false;
        s_history_valid = false;
        return;
    }

    // 1. Primary GI G-Buffer + Direct/Multi-Bounce Radiance Compute Pass
    LightingComputePush push = {
        .env_bda     = slice_bda,
        .cascade_idx = 0u,
        .flags       = 0u,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_gbuf);
    vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, (s_gi_w + 7u) / 8u, (s_gi_h + 7u) / 8u, 1u);

    cmd_compute_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    // 2. 4-Level Split / Holographic Radiance Cascades (coarse c=3 -> fine c=0)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_hrc);
    for (int c = (int)K_LIGHTING_MAX_CASCADES - 1; c >= 0; --c) {
        uint32_t uc    = (uint32_t)c;
        uint32_t cw    = s_hrc_w0 >> uc;
        uint32_t ch    = s_hrc_h0 >> uc;
        if (cw == 0u) cw = 1u;
        if (ch == 0u) ch = 1u;
        uint32_t m_cnt = 4u << uc;
        uint32_t tx    = cw * m_cnt;

        push.cascade_idx = uc;
        vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
        vkCmdDispatch(cmd, (tx + 7u) / 8u, (ch + 7u) / 8u, 1u);

        cmd_compute_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // 3. 2026 SSILVB (32-Bit Visibility Bitmask) + HRC C0 Fusion + SLM Bilateral + Temporal
    push.cascade_idx = 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe_ssilvb);
    vulkan_cmd_push(cmd, s_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof push, &push);
    vkCmdDispatch(cmd, (s_gi_w + 7u) / 8u, (s_gi_h + 7u) / 8u, 1u);

    // Publish resolved LightingGiTexel grid to all downstream surface fragment shaders
    cmd_compute_barrier(cmd,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    s_gi_frame_live = true;
    s_history_valid = true;
    s_ping_index    = prev_ping;
    s_frame_index  += 1u;
}

bool lighting_gi_active(void) {
    return s_gi_user_on && s_gi_frame_live;
}

bool lighting_toggle_gi(void) {
    s_gi_user_on = !s_gi_user_on;
    if (!s_gi_user_on) {
        s_history_valid = false;
        s_gi_frame_live = false;
    }
    printf("[Lighting] Global Illumination (SSILVB + HRC): %s\n",
           s_gi_user_on ? "ON" : "OFF");
    fflush(stdout);
    return s_gi_user_on;
}

void lighting_get_stats(LightingBenchStats *out_stats) {
    if (out_stats == nullptr) {
        return;
    }
    uint64_t bda_total = 0ull;
    if (s_env_ready) {
        bda_total += (uint64_t)s_env_buf.bytes;
    }
    if (s_gi_slab.buffer != VK_NULL_HANDLE) {
        bda_total += (uint64_t)s_gi_slab.bytes;
    }
    *out_stats = (LightingBenchStats){
        .gi_enabled     = s_gi_user_on && s_gi_frame_live,
        .oracle_mode    = s_oracle_mode,
        .gi_width       = s_gi_frame_live ? s_gi_w : 0u,
        .gi_height      = s_gi_frame_live ? s_gi_h : 0u,
        .hrc_probes_c0  = s_gi_frame_live ? (s_hrc_w0 * s_hrc_h0) : 0u,
        .hrc_total_rays = s_gi_frame_live ? s_hrc_total_rays : 0u,
        .ssilvb_slices  = s_ssilvb_slices,
        .ssilvb_steps   = s_ssilvb_steps,
        .active_proxies = s_proxy_count,
        .bda_bytes      = bda_total,
    };
}
