#include "backend/internal.h"
#include "vulkan/core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

alignas(uint32_t) static const unsigned char k_mvs_comp_spv[] = {
#embed "survey_mvs.comp.spv"
};

alignas(uint32_t) static const unsigned char k_cull_comp_spv[] = {
#embed "survey_2dgs_cull.comp.spv"
};

alignas(uint32_t) static const unsigned char k_surfel_vert_spv[] = {
#embed "survey_2dgs.vert.spv"
};

alignas(uint32_t) static const unsigned char k_surfel_frag_spv[] = {
#embed "survey_2dgs.frag.spv"
};

static_assert(sizeof k_mvs_comp_spv % 4u == 0u, "survey_mvs.comp.spv align");
static_assert(sizeof k_cull_comp_spv % 4u == 0u, "survey_2dgs_cull.comp.spv align");
static_assert(sizeof k_surfel_vert_spv % 4u == 0u, "survey_2dgs.vert.spv align");
static_assert(sizeof k_surfel_frag_spv % 4u == 0u, "survey_2dgs.frag.spv align");

enum : uint32_t {
    k_mvs_magic_tag     = 0x53564D47u, // "GMVS"
    k_mvs_max_pts       = 256u,
    k_mvs_max_bls       = 256u,
    k_mvs_max_photos    = 8u,
    k_mvs_photo_stage_w = 256u,
    k_mvs_photo_stage_h = 256u,
    k_mvs_queue_cap     = 16u,
    k_mvs_retire_frames = 4u,
    k_ind_header_bytes  = 64u,
};

typedef struct MvsPush {
    uint64_t hdr_bda;
    uint32_t flags;
    uint32_t pad0;
} MvsPush;

static_assert(sizeof(MvsPush) == 16u, "MvsPush must be 16 bytes");

typedef struct GpuMvsHeader {
    uint32_t magic;
    uint32_t tile_dim;
    uint32_t photo_count;
    uint32_t point_count;
    uint32_t breakline_count;
    uint32_t sweep_steps;
    float    world_min_x;
    float    world_min_y;
    float    span_x;
    float    span_y;
    float    mvs_range_m;
    float    mvs_step_m;
    float    mvs_min_ncc;
    float    min_kernel_r;
    uint32_t roi_min_x;
    uint32_t roi_max_x;
    uint32_t roi_min_y;
    uint32_t roi_max_y;
    uint32_t pad0;
    uint32_t pad1;
    uint64_t prior_elev_bda;
    uint64_t out_elev_bda;
    uint64_t io_rgba_bda;
    uint64_t photos_bda;
    uint64_t points_bda;
    uint64_t breaklines_bda;
} GpuMvsHeader;

static_assert(sizeof(GpuMvsHeader) == 128u, "GpuMvsHeader must be 128 bytes");

typedef struct GpuMvsPoint {
    float    wx;
    float    wy;
    float    elev_m;
    float    radius_m;
    float    weight;
    uint32_t rgba;
    uint32_t has_color;
    uint32_t pad0;
} GpuMvsPoint;

static_assert(sizeof(GpuMvsPoint) == 32u, "GpuMvsPoint must be 32 bytes");

typedef struct GpuMvsBreakline {
    float ax;
    float ay;
    float ea;
    float bx;
    float by;
    float eb;
    float radius_m;
    float weight;
} GpuMvsBreakline;

static_assert(sizeof(GpuMvsBreakline) == 32u, "GpuMvsBreakline must be 32 bytes");

typedef struct GpuMvsPhoto {
    uint64_t pixels_bda;
    uint32_t width_px;
    uint32_t height_px;
    float    fx_px;
    float    fy_px;
    float    cx_px;
    float    cy_px;
    float    k1;
    float    k2;
    float    k3;
    float    p1;
    float    p2;
    float    cam_x;
    float    cam_y;
    float    alt_m;
    float    right_x;
    float    right_y;
    float    right_z;
    float    blend_weight;
    float    up_x;
    float    up_y;
    float    up_z;
    float    enable_mvs;
    float    fwd_x;
    float    fwd_y;
    float    fwd_z;
    float    pad0;
    uint32_t pad_tail[4];
} GpuMvsPhoto;

static_assert(sizeof(GpuMvsPhoto) == 128u, "GpuMvsPhoto must be 128 bytes");

typedef struct GpuMvsWorkspace {
    alignas(64) GpuMvsHeader hdr;
    GpuMvsPhoto              photos[k_mvs_max_photos];
    GpuMvsPoint              points[k_mvs_max_pts];
    GpuMvsBreakline          breaklines[k_mvs_max_bls];
    float                    prior_elev[k_gmpack_tile_samples];
    float                    out_elev[k_gmpack_tile_samples];
    uint32_t                 io_rgba[k_gmpack_tile_samples];
    uint32_t                 photo_stage[2][k_mvs_photo_stage_w * k_mvs_photo_stage_h];
} GpuMvsWorkspace;

typedef struct SurfelCullPush {
    uint64_t surfels_bda;
    uint64_t indirect_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    float    elev_base_m;
    float    vertical_scale;
    uint32_t width;
    uint32_t height;
    uint32_t surfel_count;
    uint32_t flags;
} SurfelCullPush;

static_assert(sizeof(SurfelCullPush) == 72u, "SurfelCullPush must be 72 bytes");

typedef struct SurfelDrawPush {
    uint64_t surfels_bda;
    uint64_t indirect_bda;
    uint32_t width;
    uint32_t height;
    float    elev_base_m;
    float    vertical_scale;
    uint8_t  view_bytes[48];
} SurfelDrawPush;

static_assert(offsetof(SurfelDrawPush, view_bytes) == 32u, "SurfelDrawPush view offset");
static_assert(sizeof(SurfelDrawPush) == 80u, "SurfelDrawPush must be 80 bytes");

typedef struct MvsQueueEntry {
    uint32_t slot_index;
    uint32_t node_index;
} MvsQueueEntry;

static struct {
    VulkanContext        *vk;
    DaBuffer              surfel_buf;
    DaBuffer              surfel_ind_buf;
    DaBuffer              mvs_buf;
    TerrainSurfel2DGS    *surfel_cpu;
    uint32_t             *surfel_ind_cpu;
    GpuMvsWorkspace      *mvs_cpu;
    VkPipelineLayout      mvs_layout;
    VkPipeline            mvs_pipe;
    VkPipelineLayout      cull_layout;
    VkPipeline            cull_pipe;
    VkPipelineLayout      draw_layout;
    VulkanGfxLive         draw_gfx;
    VkFormat              armed_depth_format;
    VkSampleCountFlagBits armed_samples;
    uint32_t              surfel_cap;
    uint32_t              surfel_count;
    uint32_t              visible_surfels;
    uint32_t              tile_sample_stride;
    float                 mvs_step_scale;
    uint32_t              mvs_dispatches;
    uint32_t              frame_tick;
    MvsQueueEntry         mvs_queue[k_mvs_queue_cap];
    uint32_t              mvs_queue_count;
    bool                  mvs_job_inflight;
    uint32_t              mvs_job_slot;
    uint32_t              mvs_job_node;
    uint32_t              mvs_job_tick;
    bool                  enabled_2dgs;
    bool                  initialized;
} s_gs;

static bool parse_2dgs_env(void) {
    const char *env = getenv("ENGINE_2DGS");
    if (env == nullptr || env[0] == '\0') {
        return true;
    }
    if (strcmp(env, "0") == 0 || strcmp(env, "off") == 0 ||
        strcmp(env, "false") == 0 || strcmp(env, "no") == 0) {
        return false;
    }
    return true;
}

static int ensure_mvs_workspace_allocated(void) {
    if (s_gs.mvs_cpu != nullptr && s_gs.mvs_buf.address != 0ull) {
        return 0;
    }
    if (s_gs.vk == nullptr) {
        return -1;
    }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(s_gs.vk, sizeof(GpuMvsWorkspace), usage, true, &s_gs.mvs_buf) != 0 ||
        s_gs.mvs_buf.map == nullptr || s_gs.mvs_buf.address == 0ull) {
        da_buffer_destroy(s_gs.vk, &s_gs.mvs_buf);
        return -1;
    }
    s_gs.mvs_cpu = (GpuMvsWorkspace *)s_gs.mvs_buf.map;
    memset(s_gs.mvs_cpu, 0, sizeof(GpuMvsWorkspace));
    return 0;
}

static void stage_photo_thumbnail(const TerrainSurveyPhoto *ph,
                                  uint32_t                 *dst_pixels,
                                  uint64_t                  dst_bda,
                                  GpuMvsPhoto              *out_gpu) {
    memset(out_gpu, 0, sizeof *out_gpu);
    if (ph == nullptr) {
        return;
    }
    const TerrainCameraIntrinsics *in = &ph->intrinsics;
    const TerrainCameraPose       *po = &ph->pose;

    out_gpu->k1           = in->k1;
    out_gpu->k2           = in->k2;
    out_gpu->k3           = in->k3;
    out_gpu->p1           = in->p1;
    out_gpu->p2           = in->p2;
    out_gpu->cam_x        = po->world_x;
    out_gpu->cam_y        = po->world_y;
    out_gpu->alt_m        = po->altitude_msl_m;
    out_gpu->right_x      = po->right[0];
    out_gpu->right_y      = po->right[1];
    out_gpu->right_z      = po->right[2];
    out_gpu->blend_weight = ph->blend_weight;
    out_gpu->up_x         = po->up[0];
    out_gpu->up_y         = po->up[1];
    out_gpu->up_z         = po->up[2];
    out_gpu->enable_mvs   = ph->enable_mvs ? 1.0f : 0.0f;
    out_gpu->fwd_x        = po->forward[0];
    out_gpu->fwd_y        = po->forward[1];
    out_gpu->fwd_z        = po->forward[2];

    if (ph->rgba_pixels == nullptr || in->width_px == 0u || in->height_px == 0u ||
        dst_pixels == nullptr || dst_bda == 0ull) {
        return;
    }

    uint32_t dst_w = in->width_px < k_mvs_photo_stage_w ? in->width_px : k_mvs_photo_stage_w;
    uint32_t dst_h = in->height_px < k_mvs_photo_stage_h ? in->height_px : k_mvs_photo_stage_h;
    float    sx    = (float)dst_w / (float)in->width_px;
    float    sy    = (float)dst_h / (float)in->height_px;

    for (uint32_t y = 0u; y < dst_h; ++y) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * in->height_px) / dst_h);
        if (src_y >= in->height_px) {
            src_y = in->height_px - 1u;
        }
        for (uint32_t x = 0u; x < dst_w; ++x) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * in->width_px) / dst_w);
            if (src_x >= in->width_px) {
                src_x = in->width_px - 1u;
            }
            dst_pixels[y * dst_w + x] = ph->rgba_pixels[(size_t)src_y * in->width_px + src_x];
        }
    }

    out_gpu->pixels_bda = dst_bda;
    out_gpu->width_px   = dst_w;
    out_gpu->height_px  = dst_h;
    out_gpu->fx_px      = in->fx_px * sx;
    out_gpu->fy_px      = in->fy_px * sy;
    out_gpu->cx_px      = in->cx_px * sx;
    out_gpu->cy_px      = in->cy_px * sy;
}

static bool prepare_mvs_workspace_for_tile(uint32_t slot_index, uint32_t node_index) {
    if (!g_backend.initialized || g_backend.gpu_hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        return false;
    }
    if (slot_index >= backend_slot_cap() || node_index >= k_gmpack_node_count) {
        return false;
    }
    const TerrainSurveyDataset *ds   = &g_backend.survey;
    const GpuMathQuadNode      *node = &g_backend.gpu_hdr->nodes[node_index];
    if (!backend_survey_tile_may_overlap(ds, node)) {
        return false;
    }
    if (ds->point_count == 0u && ds->tiepoint_count == 0u &&
        ds->breakline_count == 0u && ds->photo_count == 0u) {
        return false;
    }
    if (ensure_mvs_workspace_allocated() != 0) {
        return false;
    }

    GpuMvsWorkspace      *ws   = s_gs.mvs_cpu;
    const uint64_t        base = s_gs.mvs_buf.address;
    const EpackTileBlock *ep   = &g_backend.gpu_tiles[slot_index];

    for (uint32_t sy = 0u; sy < k_gmpack_tile_dim; ++sy) {
        for (uint32_t sx = 0u; sx < k_gmpack_tile_dim; ++sx) {
            uint32_t idx        = sy * k_gmpack_tile_dim + sx;
            float    h_m        = backend_epack_load_height_m(ep, sx, sy);
            ws->prior_elev[idx] = h_m;
            ws->out_elev[idx]   = h_m;
            ws->io_rgba[idx]    = backend_epack_load_rgba8(ep, sx, sy);
        }
    }

    float span_x       = fmaxf(node->world_max_x - node->world_min_x, 1.0f);
    float span_y       = fmaxf(node->world_max_y - node->world_min_y, 1.0f);
    float post_step    = span_x / (float)(k_gmpack_tile_dim - 1u);
    float min_kernel_r = fmaxf(post_step * 2.25f, 8.0f);

    uint32_t pt_cnt = 0u;
    for (uint32_t i = 0u; i < ds->point_count && pt_cnt < k_mvs_max_pts; ++i) {
        const TerrainSurveyPoint *sp = &ds->points[i];
        float r = fmaxf(sp->radius_m, min_kernel_r);
        if (sp->world_x + r < node->world_min_x || sp->world_x - r > node->world_max_x ||
            sp->world_y + r < node->world_min_y || sp->world_y - r > node->world_max_y) {
            continue;
        }
        ws->points[pt_cnt++] = (GpuMvsPoint){
            .wx        = sp->world_x,
            .wy        = sp->world_y,
            .elev_m    = sp->elevation_m,
            .radius_m  = r,
            .weight    = sp->weight,
            .rgba      = (uint32_t)sp->r | ((uint32_t)sp->g << 8u) | ((uint32_t)sp->b << 16u) | 0xFF000000u,
            .has_color = sp->has_color ? 1u : 0u,
        };
    }

    for (uint32_t i = 0u; i < ds->tiepoint_count && pt_cnt < k_mvs_max_pts; ++i) {
        const TerrainStereoTiePoint *tp = &ds->tiepoints[i];
        if (!tp->valid) {
            continue;
        }
        float r = fmaxf(tp->radius_m, min_kernel_r);
        if (tp->triangulated_x + r < node->world_min_x || tp->triangulated_x - r > node->world_max_x ||
            tp->triangulated_y + r < node->world_min_y || tp->triangulated_y - r > node->world_max_y) {
            continue;
        }
        ws->points[pt_cnt++] = (GpuMvsPoint){
            .wx        = tp->triangulated_x,
            .wy        = tp->triangulated_y,
            .elev_m    = tp->triangulated_elev_m,
            .radius_m  = r,
            .weight    = tp->weight,
            .rgba      = 0u,
            .has_color = 0u,
        };
    }

    uint32_t bl_cnt = 0u;
    for (uint32_t i = 0u; i < ds->breakline_count && bl_cnt < k_mvs_max_bls; ++i) {
        const TerrainSurveyBreakline *bl = &ds->breaklines[i];
        float r     = fmaxf(bl->radius_m, min_kernel_r);
        float min_x = fminf(bl->world_ax, bl->world_bx) - r;
        float max_x = fmaxf(bl->world_ax, bl->world_bx) + r;
        float min_y = fminf(bl->world_ay, bl->world_by) - r;
        float max_y = fmaxf(bl->world_ay, bl->world_by) + r;
        if (max_x < node->world_min_x || min_x > node->world_max_x ||
            max_y < node->world_min_y || min_y > node->world_max_y) {
            continue;
        }
        ws->breaklines[bl_cnt++] = (GpuMvsBreakline){
            .ax       = bl->world_ax,
            .ay       = bl->world_ay,
            .ea       = bl->elev_a_m,
            .bx       = bl->world_bx,
            .by       = bl->world_by,
            .eb       = bl->elev_b_m,
            .radius_m = r,
            .weight   = bl->weight,
        };
    }

    uint32_t ph_cnt = ds->photo_count < k_mvs_max_photos ? ds->photo_count : k_mvs_max_photos;
    for (uint32_t i = 0u; i < ph_cnt; ++i) {
        uint32_t *stage_ptr = (i < 2u) ? ws->photo_stage[i] : nullptr;
        uint64_t  stage_bda = (i < 2u) ? (base + offsetof(GpuMvsWorkspace, photo_stage) +
                                          (uint64_t)i * sizeof(ws->photo_stage[0]))
                                       : 0ull;
        stage_photo_thumbnail(&ds->photos[i], stage_ptr, stage_bda, &ws->photos[i]);
    }

    if (pt_cnt == 0u && bl_cnt == 0u && ph_cnt == 0u) {
        return false;
    }

    float range_m   = ds->mvs_search_range_m > 0.25f ? ds->mvs_search_range_m : 12.0f;
    float base_step = ds->mvs_step_m > 0.05f ? ds->mvs_step_m : 0.75f;
    float step_m    = fmaxf(base_step * s_gs.mvs_step_scale, 0.10f);
    uint32_t steps  = (ph_cnt >= 2u) ? ((uint32_t)ceilf((2.0f * range_m) / step_m) + 1u) : 0u;
    if (steps > 96u) {
        steps  = 96u;
        step_m = (2.0f * range_m) / 95.0f;
    }

    ws->hdr = (GpuMvsHeader){
        .magic           = k_mvs_magic_tag,
        .tile_dim        = k_gmpack_tile_dim,
        .photo_count     = ph_cnt,
        .point_count     = pt_cnt,
        .breakline_count = bl_cnt,
        .sweep_steps     = steps,
        .world_min_x     = node->world_min_x,
        .world_min_y     = node->world_min_y,
        .span_x          = span_x,
        .span_y          = span_y,
        .mvs_range_m     = range_m,
        .mvs_step_m      = step_m,
        .mvs_min_ncc     = ds->mvs_min_ncc > 0.05f ? ds->mvs_min_ncc : 0.55f,
        .min_kernel_r    = min_kernel_r,
        .roi_min_x       = 0u,
        .roi_max_x       = k_gmpack_tile_dim - 1u,
        .roi_min_y       = 0u,
        .roi_max_y       = k_gmpack_tile_dim - 1u,
        .prior_elev_bda  = base + offsetof(GpuMvsWorkspace, prior_elev),
        .out_elev_bda    = base + offsetof(GpuMvsWorkspace, out_elev),
        .io_rgba_bda     = base + offsetof(GpuMvsWorkspace, io_rgba),
        .photos_bda      = ph_cnt > 0u ? (base + offsetof(GpuMvsWorkspace, photos)) : 0ull,
        .points_bda      = pt_cnt > 0u ? (base + offsetof(GpuMvsWorkspace, points)) : 0ull,
        .breaklines_bda  = bl_cnt > 0u ? (base + offsetof(GpuMvsWorkspace, breaklines)) : 0ull,
    };
    return true;
}

static void retire_completed_mvs_job(void) {
    if (!s_gs.mvs_job_inflight || s_gs.mvs_cpu == nullptr) {
        return;
    }
    if ((s_gs.frame_tick - s_gs.mvs_job_tick) < k_mvs_retire_frames) {
        return;
    }
    s_gs.mvs_job_inflight = false;

    uint32_t slot = s_gs.mvs_job_slot;
    uint32_t nid  = s_gs.mvs_job_node;
    if (!g_backend.initialized || g_backend.gpu_tiles == nullptr ||
        slot >= backend_slot_cap() || nid >= k_gmpack_node_count ||
        g_backend.slot_to_node[slot] != nid) {
        return;
    }

    if (g_backend.fuse_scratch == nullptr) {
        g_backend.fuse_scratch = aligned_alloc(4096u, sizeof(GpuMathTileBlock));
        if (g_backend.fuse_scratch == nullptr) {
            return;
        }
    }

    const GpuMvsWorkspace *ws  = s_gs.mvs_cpu;
    GpuMathTileBlock      *raw = g_backend.fuse_scratch;
    bool                   any_delta = false;
    for (uint32_t i = 0u; i < k_gmpack_tile_samples; ++i) {
        float hm = ws->out_elev[i];
        if (fabsf(hm - ws->prior_elev[i]) > 0.03f) {
            any_delta = true;
        }
        float q16 = roundf(fminf(fmaxf(hm, 0.0f), 4095.9375f) * 16.0f);
        raw->height_u16[i]    = (uint16_t)q16;
        raw->imagery_rgba8[i] = ws->io_rgba[i];
    }

    if (any_delta || ws->hdr.photo_count > 0u) {
        backend_epack_encode_tile(raw, &g_backend.gpu_tiles[slot]);
        if (g_backend.fused_cache[nid] != nullptr) {
            memcpy(g_backend.fused_cache[nid], &g_backend.gpu_tiles[slot], sizeof(EpackTileBlock));
        }
        if (g_backend.slot_dirty[slot] == 0u) {
            g_backend.slot_dirty[slot] = 1u;
            g_backend.dirty_slot_count++;
            if (slot < 64u) {
                g_backend.dirty_slots_mask |= (1ull << slot);
            }
        }
        backend_gpu_survey_rebuild_surfels();
    }
}

int backend_gpu_survey_init(VulkanContext *vk) {
    if (vk == nullptr || vk->device == VK_NULL_HANDLE) {
        return -1;
    }
    if (s_gs.initialized) {
        return 0;
    }
    if (!backend_surfel_2dgs_self_check()) {
        fprintf(stderr, "[Backend Error] 2DGS geodetic surfel math self-check failed.\n");
        return -1;
    }

    memset(&s_gs, 0, sizeof s_gs);
    s_gs.vk           = vk;
    s_gs.enabled_2dgs = parse_2dgs_env();

    const bool is_uma       = (vk->device_info.execution_profile == VULKAN_PROFILE_UMA_IGPU);
    s_gs.surfel_cap         = is_uma ? k_surfel_cap_uma : k_surfel_cap_dgpu;
    s_gs.tile_sample_stride = is_uma ? 10u : 5u;
    s_gs.mvs_step_scale     = is_uma ? 1.0f : 0.5f;

    const VkDeviceSize surfel_bytes = (VkDeviceSize)s_gs.surfel_cap * sizeof(TerrainSurfel2DGS);
    const VkBufferUsageFlags surfel_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(vk, surfel_bytes, surfel_usage, true, &s_gs.surfel_buf) != 0 ||
        s_gs.surfel_buf.map == nullptr || s_gs.surfel_buf.address == 0ull) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }
    s_gs.surfel_cpu = (TerrainSurfel2DGS *)s_gs.surfel_buf.map;

    const VkDeviceSize ind_bytes = k_ind_header_bytes + (VkDeviceSize)s_gs.surfel_cap * sizeof(uint32_t);
    const VkBufferUsageFlags ind_usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (da_buffer_create(vk, ind_bytes, ind_usage, true, &s_gs.surfel_ind_buf) != 0 ||
        s_gs.surfel_ind_buf.map == nullptr || s_gs.surfel_ind_buf.address == 0ull) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }
    s_gs.surfel_ind_cpu = (uint32_t *)s_gs.surfel_ind_buf.map;
    memset(s_gs.surfel_ind_cpu, 0, k_ind_header_bytes);
    s_gs.surfel_ind_cpu[4u] = 6u;

    VkPushConstantRange mvs_push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(MvsPush),
    };
    VkPipelineLayoutCreateInfo mvs_layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &mvs_push,
    };
    if (vkCreatePipelineLayout(vk->device, &mvs_layout_ci, nullptr, &s_gs.mvs_layout) != VK_SUCCESS) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }
    s_gs.mvs_pipe = vulkan_compute_pipeline(vk, k_mvs_comp_spv, sizeof k_mvs_comp_spv, s_gs.mvs_layout);
    if (s_gs.mvs_pipe == VK_NULL_HANDLE) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }

    VkPushConstantRange cull_push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0u,
        .size       = sizeof(SurfelCullPush),
    };
    VkPipelineLayoutCreateInfo cull_layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &cull_push,
    };
    if (vkCreatePipelineLayout(vk->device, &cull_layout_ci, nullptr, &s_gs.cull_layout) != VK_SUCCESS) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }
    s_gs.cull_pipe = vulkan_compute_pipeline(vk, k_cull_comp_spv, sizeof k_cull_comp_spv, s_gs.cull_layout);
    if (s_gs.cull_pipe == VK_NULL_HANDLE) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }

    VkPushConstantRange draw_push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0u,
        .size       = sizeof(SurfelDrawPush),
    };
    VkPipelineLayoutCreateInfo draw_layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges    = &draw_push,
    };
    if (vkCreatePipelineLayout(vk->device, &draw_layout_ci, nullptr, &s_gs.draw_layout) != VK_SUCCESS) {
        backend_gpu_survey_shutdown(vk);
        return -1;
    }

    s_gs.initialized = true;
    printf("[Backend] 2026 Oriented 2DGS Surfels & Compute MVS ready (cap=%u surfels, %.0f KiB BDA, 2dgs=%s).\n",
           s_gs.surfel_cap,
           (double)(s_gs.surfel_buf.bytes + s_gs.surfel_ind_buf.bytes) / 1024.0,
           s_gs.enabled_2dgs ? "on" : "off");
    return 0;
}

void backend_gpu_survey_shutdown(VulkanContext *vk) {
    VulkanContext *ctx = (vk != nullptr) ? vk : s_gs.vk;
    if (ctx != nullptr && ctx->device != VK_NULL_HANDLE) {
        vulkan_gfx_disarm(ctx, &s_gs.draw_gfx);
        vulkan_pipe_release(ctx, s_gs.mvs_pipe);
        vulkan_pipe_release(ctx, s_gs.cull_pipe);
        if (s_gs.mvs_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_gs.mvs_layout, nullptr);
        }
        if (s_gs.cull_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_gs.cull_layout, nullptr);
        }
        if (s_gs.draw_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->device, s_gs.draw_layout, nullptr);
        }
        da_buffer_destroy(ctx, &s_gs.mvs_buf);
        da_buffer_destroy(ctx, &s_gs.surfel_ind_buf);
        da_buffer_destroy(ctx, &s_gs.surfel_buf);
    }
    memset(&s_gs, 0, sizeof s_gs);
}

void backend_gpu_survey_rebuild_surfels(void) {
    if (!s_gs.initialized || s_gs.surfel_cpu == nullptr || s_gs.surfel_cap == 0u) {
        return;
    }
    const GpuMathArchiveHeader *hdr = g_backend.gpu_hdr;
    if (hdr == nullptr || g_backend.gpu_tiles == nullptr) {
        s_gs.surfel_count = 0u;
        return;
    }

    uint32_t written = backend_surfel_2dgs_extract_survey(&g_backend.survey,
                                                          s_gs.surfel_cpu,
                                                          s_gs.surfel_cap);

    uint8_t  visited[k_gmpack_node_count] = {0};
    uint32_t res_nodes[k_gmpack_active_cut_cap];
    uint32_t res_slots[k_gmpack_active_cut_cap];
    uint32_t res_count = 0u;

    uint32_t leaf_cnt = hdr->dir.active_leaf_count;
    if (leaf_cnt > k_gmpack_active_cut_cap) {
        leaf_cnt = k_gmpack_active_cut_cap;
    }

    const uint32_t slot_cap = backend_slot_cap();
    for (uint32_t i = 0u; i < leaf_cnt; ++i) {
        uint32_t nid = hdr->dir.active_leaves[i];
        if (nid >= k_gmpack_node_count) {
            continue;
        }
        for (uint32_t guard = 0u; guard < 8u && nid != 0u && hdr->nodes[nid].resident == 0u; ++guard) {
            nid = hdr->nodes[nid].parent_index;
        }
        if (nid >= k_gmpack_node_count || hdr->nodes[nid].resident == 0u) {
            nid = 0u;
        }
        uint32_t slot = hdr->nodes[nid].gpu_slot;
        if (slot >= slot_cap || visited[nid] != 0u) {
            continue;
        }
        visited[nid]         = 1u;
        res_nodes[res_count] = nid;
        res_slots[res_count] = slot;
        res_count++;
    }

    if (res_count == 0u && hdr->nodes[0].resident != 0u) {
        res_nodes[0] = 0u;
        res_slots[0] = 0u;
        res_count    = 1u;
    }

    float    vscale        = hdr->dir.vertical_scale > 0.01f ? hdr->dir.vertical_scale : 1.15f;
    uint32_t per_tile_soft = (s_gs.surfel_cap > k_surfel_cap_uma) ? 4096u : 1024u;

    for (uint32_t i = 0u; i < res_count && written < s_gs.surfel_cap; ++i) {
        uint32_t rem      = s_gs.surfel_cap - written;
        uint32_t tile_cap = rem < per_tile_soft ? rem : per_tile_soft;
        uint32_t nid      = res_nodes[i];
        uint32_t slot     = res_slots[i];
        written += backend_surfel_2dgs_extract_tile(&hdr->nodes[nid],
                                                    nid,
                                                    &g_backend.gpu_tiles[slot],
                                                    vscale,
                                                    s_gs.tile_sample_stride,
                                                    s_gs.surfel_cpu + written,
                                                    tile_cap);
    }

    s_gs.surfel_count = written;
}

void backend_gpu_survey_queue_mvs_tile(uint32_t slot_index, uint32_t node_index) {
    if (!s_gs.initialized || g_backend.gpu_hdr == nullptr) {
        return;
    }
    if (node_index >= k_gmpack_node_count || slot_index >= backend_slot_cap()) {
        return;
    }
    const TerrainSurveyDataset *ds = &g_backend.survey;
    if (!backend_survey_tile_may_overlap(ds, &g_backend.gpu_hdr->nodes[node_index])) {
        return;
    }
    for (uint32_t i = 0u; i < s_gs.mvs_queue_count; ++i) {
        if (s_gs.mvs_queue[i].node_index == node_index) {
            s_gs.mvs_queue[i].slot_index = slot_index;
            return;
        }
    }
    if (s_gs.mvs_queue_count < k_mvs_queue_cap) {
        s_gs.mvs_queue[s_gs.mvs_queue_count++] = (MvsQueueEntry){
            .slot_index = slot_index,
            .node_index = node_index,
        };
    }
}

void backend_gpu_survey_record_compute(VkCommandBuffer cmd, const TerrainCullParams *cull) {
    if (!s_gs.initialized || cmd == VK_NULL_HANDLE) {
        return;
    }
    s_gs.frame_tick++;

    // Harvest previous completed GPU MVS job without blocking the GPU timeline
    retire_completed_mvs_job();

    if (s_gs.surfel_ind_cpu != nullptr) {
        s_gs.visible_surfels = s_gs.surfel_ind_cpu[0u];
    }

    // 1. Dispatch queued tiled Slang 2026 Compute Plane-Sweep ZNCC MVS + Wendland C^2 RBF
    if (!s_gs.mvs_job_inflight && s_gs.mvs_queue_count > 0u && s_gs.mvs_pipe != VK_NULL_HANDLE) {
        MvsQueueEntry job = s_gs.mvs_queue[0u];
        memmove(&s_gs.mvs_queue[0u],
                &s_gs.mvs_queue[1u],
                (size_t)(s_gs.mvs_queue_count - 1u) * sizeof(MvsQueueEntry));
        s_gs.mvs_queue_count--;

        if (prepare_mvs_workspace_for_tile(job.slot_index, job.node_index)) {
            MvsPush mvs_pc = {
                .hdr_bda = s_gs.mvs_buf.address,
                .flags   = 1u,
                .pad0    = 0u,
            };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gs.mvs_pipe);
            vkCmdPushConstants(cmd, s_gs.mvs_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof mvs_pc, &mvs_pc);
            uint32_t groups_xy = (k_gmpack_tile_dim + 7u) / 8u;
            vkCmdDispatch(cmd, groups_xy, groups_xy, 1u);

            VkMemoryBarrier2 mvs_bar = {
                .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT,
                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
            };
            VkDependencyInfo mvs_dep = {
                .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1u,
                .pMemoryBarriers    = &mvs_bar,
            };
            vkCmdPipelineBarrier2(cmd, &mvs_dep);

            s_gs.mvs_job_inflight = true;
            s_gs.mvs_job_slot     = job.slot_index;
            s_gs.mvs_job_node     = job.node_index;
            s_gs.mvs_job_tick     = s_gs.frame_tick;
            s_gs.mvs_dispatches++;
        }
    }

    // 2. Dispatch deterministic subgroup WavePrefixCountBits 2DGS surfel culling
    if (s_gs.cull_pipe == VK_NULL_HANDLE || s_gs.surfel_ind_buf.address == 0ull) {
        return;
    }

    SurfelCullPush pc = {
        .surfels_bda    = s_gs.surfel_buf.address,
        .indirect_bda   = s_gs.surfel_ind_buf.address,
        .qx             = cull != nullptr ? cull->qx : 0.0f,
        .qy             = cull != nullptr ? cull->qy : 0.0f,
        .qz             = cull != nullptr ? cull->qz : 0.0f,
        .qw             = cull != nullptr ? cull->qw : 1.0f,
        .cam_x          = cull != nullptr ? cull->cam_x : 0.0f,
        .cam_y          = cull != nullptr ? cull->cam_y : 0.0f,
        .cam_z          = cull != nullptr ? cull->cam_z : 0.0f,
        .zoom           = (cull != nullptr && cull->zoom > 1.0e-5f) ? cull->zoom : 1.0f,
        .elev_base_m    = cull != nullptr ? cull->elev_base_m : k_besparmak_elev_min,
        .vertical_scale = (cull != nullptr && cull->vertical_scale > 0.01f) ? cull->vertical_scale : 1.15f,
        .width          = (cull != nullptr && cull->width > 0u) ? cull->width : 1920u,
        .height         = (cull != nullptr && cull->height > 0u) ? cull->height : 1080u,
        .surfel_count   = s_gs.surfel_count,
        .flags          = (s_gs.enabled_2dgs && cull != nullptr) ? 1u : 0u,
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_gs.cull_pipe);
    vkCmdPushConstants(cmd, s_gs.cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof pc, &pc);
    vkCmdDispatch(cmd, 1u, 1u, 1u);

    VkMemoryBarrier2 cull_bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo cull_dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &cull_bar,
    };
    vkCmdPipelineBarrier2(cmd, &cull_dep);
}

void backend_record_draw_surfels(VkCommandBuffer       cmd,
                                 uint32_t              width,
                                 uint32_t              height,
                                 VkFormat              depth_format,
                                 VkSampleCountFlagBits samples,
                                 float                 elev_base_m,
                                 float                 vertical_scale,
                                 const void           *view_push_ptr,
                                 size_t                view_push_bytes) {
    if (!s_gs.initialized || !s_gs.enabled_2dgs || s_gs.surfel_count == 0u ||
        cmd == VK_NULL_HANDLE || s_gs.vk == nullptr || view_push_ptr == nullptr ||
        view_push_bytes != 48u || depth_format == VK_FORMAT_UNDEFINED || samples == 0) {
        return;
    }

    if (!s_gs.draw_gfx.armed ||
        s_gs.armed_depth_format != depth_format ||
        s_gs.armed_samples != samples) {
        vulkan_gfx_disarm(s_gs.vk, &s_gs.draw_gfx);
        VulkanGfxPipe gfx = {
            .vert          = k_surfel_vert_spv,
            .vert_bytes    = sizeof k_surfel_vert_spv,
            .frag          = k_surfel_frag_spv,
            .frag_bytes    = sizeof k_surfel_frag_spv,
            .layout        = s_gs.draw_layout,
            .depth_format  = depth_format,
            .samples       = samples,
            .cull          = VK_CULL_MODE_NONE,
            .depth_test    = VK_TRUE,
            .depth_write   = VK_FALSE,
            .depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL,
            .blend         = VK_TRUE,
        };
        vulkan_gfx_arm(&s_gs.draw_gfx, &gfx, VK_FORMAT_B8G8R8A8_SRGB, nullptr);
        s_gs.armed_depth_format = depth_format;
        s_gs.armed_samples      = samples;
    }

    VkPipeline pipe = vulkan_gfx_use(s_gs.vk, &s_gs.draw_gfx);
    if (pipe == VK_NULL_HANDLE) {
        return;
    }

    SurfelDrawPush push = {
        .surfels_bda    = s_gs.surfel_buf.address,
        .indirect_bda   = s_gs.surfel_ind_buf.address,
        .width          = width,
        .height         = height,
        .elev_base_m    = elev_base_m,
        .vertical_scale = vertical_scale,
    };
    memcpy(push.view_bytes, view_push_ptr, 48u);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd,
                       s_gs.draw_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0u,
                       sizeof push,
                       &push);
    vkCmdDrawIndirect(cmd,
                      s_gs.surfel_ind_buf.buffer,
                      s_gs.surfel_ind_buf.offset + 16u,
                      1u,
                      16u);
}

bool backend_toggle_2dgs(void) {
    s_gs.enabled_2dgs = !s_gs.enabled_2dgs;
    if (!s_gs.enabled_2dgs) {
        s_gs.visible_surfels = 0u;
    }
    printf("[Backend] Oriented 2DGS Geodetic Surfels: %s (surfels=%u, cap=%u)\n",
           s_gs.enabled_2dgs ? "ENABLED" : "DISABLED",
           s_gs.surfel_count,
           s_gs.surfel_cap);
    fflush(stdout);
    return s_gs.enabled_2dgs;
}

bool backend_2dgs_active(void) {
    return s_gs.enabled_2dgs;
}

void backend_gpu_survey_get_stats(uint64_t *out_bda_bytes,
                                  uint32_t *out_total_surfels,
                                  uint32_t *out_visible_surfels,
                                  uint32_t *out_mvs_dispatches) {
    if (out_bda_bytes != nullptr) {
        *out_bda_bytes = (uint64_t)s_gs.surfel_buf.bytes +
                         (uint64_t)s_gs.surfel_ind_buf.bytes +
                         (uint64_t)s_gs.mvs_buf.bytes;
    }
    if (out_total_surfels != nullptr) {
        *out_total_surfels = s_gs.surfel_count;
    }
    if (out_visible_surfels != nullptr) {
        uint32_t live_vis = (s_gs.surfel_ind_cpu != nullptr) ? s_gs.surfel_ind_cpu[0u] : s_gs.visible_surfels;
        *out_visible_surfels = s_gs.enabled_2dgs ? live_vis : 0u;
    }
    if (out_mvs_dispatches != nullptr) {
        *out_mvs_dispatches = s_gs.mvs_dispatches;
    }
}
