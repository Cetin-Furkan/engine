#include "scene_gpu_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool scene_point_retired(uint64_t point) {
    if (point == 0u) {
        return true;
    }
    if (g_scene.vk == nullptr || g_scene.vk->device == VK_NULL_HANDLE ||
        g_scene.upload_sem == VK_NULL_HANDLE) {
        return false;
    }
    uint64_t done = 0u;
    return vkGetSemaphoreCounterValue(g_scene.vk->device, g_scene.upload_sem, &done) == VK_SUCCESS &&
           done >= point;
}

void scene_uploads_reap(VulkanContext *dev_vk, bool force_all) {
    for (uint32_t i = 0u; i < K_SCENE_MAX_PENDING_UPLOADS; ++i) {
        ScenePendingUpload *up = &g_scene.pending_uploads[i];
        if (!up->in_use) {
            continue;
        }
        bool retired = force_all ||
                       (up->recorded && up->signal_point != 0u &&
                        scene_point_retired(up->signal_point));
        if (retired) {
            da_buffer_destroy(dev_vk, &up->stage);
            *up = (ScenePendingUpload){0};
        }
    }
}

void scene_uploads_flush(VkCommandBuffer cmd) {
    bool any_copied = false;
    for (uint32_t i = 0u; i < K_SCENE_MAX_PENDING_UPLOADS; ++i) {
        ScenePendingUpload *up = &g_scene.pending_uploads[i];
        if (!up->in_use || up->recorded) {
            continue;
        }
        VkBufferCopy2 region = {
            .sType     = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
            .srcOffset = up->stage.offset,
            .dstOffset = g_scene.geom_slab.offset + up->dst_off,
            .size      = up->bytes,
        };
        VkCopyBufferInfo2 copy_info = {
            .sType       = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
            .srcBuffer   = up->stage.buffer,
            .dstBuffer   = g_scene.geom_slab.buffer,
            .regionCount = 1u,
            .pRegions    = &region,
        };
        vkCmdCopyBuffer2(cmd, &copy_info);
        up->recorded     = true;
        up->signal_point = g_scene.cur_signal_point;
        any_copied       = true;
    }
    if (any_copied) {
        VkMemoryBarrier2 copy_bar = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
        };
        VkDependencyInfo copy_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers    = &copy_bar,
        };
        vkCmdPipelineBarrier2(cmd, &copy_dep);
    }
}

static int upload_slab_bytes(VkDeviceSize dst_off, const void *src, VkDeviceSize bytes) {
    if (bytes == 0u) {
        return 0;
    }
    if (g_scene.geom_slab.buffer == VK_NULL_HANDLE || dst_off + bytes > g_scene.geom_slab.bytes) {
        return -1;
    }
    if (g_scene.geom_slab.map != nullptr) {
        memcpy((uint8_t *)g_scene.geom_slab.map + dst_off, src, (size_t)bytes);
        return 0;
    }

    scene_uploads_reap(g_scene.vk, false);
    int slot = -1;
    for (uint32_t i = 0u; i < K_SCENE_MAX_PENDING_UPLOADS; ++i) {
        if (!g_scene.pending_uploads[i].in_use) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    DaBuffer stage = {0};
    if (da_buffer_create(g_scene.vk, bytes,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true, &stage) != 0 || stage.map == nullptr) {
        da_buffer_destroy(g_scene.vk, &stage);
        return -1;
    }
    memcpy(stage.map, src, (size_t)bytes);

    g_scene.pending_uploads[slot] = (ScenePendingUpload){
        .stage        = stage,
        .dst_off      = dst_off,
        .bytes        = bytes,
        .signal_point = 0u,
        .recorded     = false,
        .in_use       = true,
    };
    return 0;
}

static void compute_mesh_proxy_meta(SceneMeshRecord *rec, const SceneMeshletBuild *b) {
    if (rec == nullptr || b == nullptr || b->meshlet_count == 0u) {
        return;
    }
    float bmin[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
    float bmax[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
    for (uint32_t i = 0u; i < b->meshlet_count; ++i) {
        const SceneMeshlet *ml = &b->meshlets[i];
        for (int a = 0; a < 3; ++a) {
            float lo = ml->origin[a];
            float hi = ml->origin[a] + ml->scale[a];
            if (lo < bmin[a]) bmin[a] = lo;
            if (hi > bmax[a]) bmax[a] = hi;
        }
    }
    for (int a = 0; a < 3; ++a) {
        rec->local_center[a] = 0.5f * (bmin[a] + bmax[a]);
        rec->half_extents[a] = fmaxf(0.5f * (bmax[a] - bmin[a]), 1.0e-3f);
    }

    double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_a = 0.0;
    uint32_t vc  = b->vertex_count;
    for (uint32_t v = 0u; v < vc; ++v) {
        uint32_t rgba = b->vertices[v].rgba8;
        sum_r += (double)( rgba         & 0xFFu) * (1.0 / 255.0);
        sum_g += (double)((rgba >>  8u) & 0xFFu) * (1.0 / 255.0);
        sum_b += (double)((rgba >> 16u) & 0xFFu) * (1.0 / 255.0);
        sum_a += (double)((rgba >> 24u) & 0xFFu) * (1.0 / 255.0);
    }
    if (vc > 0u) {
        double inv = 1.0 / (double)vc;
        rec->avg_albedo[0] = (float)(sum_r * inv);
        rec->avg_albedo[1] = (float)(sum_g * inv);
        rec->avg_albedo[2] = (float)(sum_b * inv);
        rec->avg_roughness = (float)(sum_a * inv);
    } else {
        rec->avg_albedo[0] = 0.5f;
        rec->avg_albedo[1] = 0.5f;
        rec->avg_albedo[2] = 0.5f;
        rec->avg_roughness = 0.35f;
    }
}

static int upload_mesh_build_at(uint32_t mesh_id, uint32_t ml_base, uint32_t vx_base,
                                uint32_t tr_base, const SceneMeshletBuild *b) {
    SceneMeshlet *patched = malloc((size_t)b->meshlet_count * sizeof(*patched));
    if (patched == nullptr) {
        return -1;
    }
    for (uint32_t i = 0u; i < b->meshlet_count; ++i) {
        patched[i] = b->meshlets[i];
        patched[i].vert_offset += vx_base;
        patched[i].tri_offset  += tr_base;
        patched[i].mesh_id      = mesh_id;
    }

    int rc = 0;
    if (upload_slab_bytes(K_SCENE_SLAB_OFF_MESHLETS + (VkDeviceSize)ml_base * sizeof(SceneMeshlet),
                          patched, (VkDeviceSize)b->meshlet_count * sizeof(SceneMeshlet)) != 0 ||
        upload_slab_bytes(K_SCENE_SLAB_OFF_VERTICES + (VkDeviceSize)vx_base * sizeof(ScenePackedVertex),
                          b->vertices, (VkDeviceSize)b->vertex_count * sizeof(ScenePackedVertex)) != 0 ||
        upload_slab_bytes(K_SCENE_SLAB_OFF_TRIANGLES + (VkDeviceSize)tr_base * sizeof(uint32_t),
                          b->triangles, (VkDeviceSize)b->triangle_count * sizeof(uint32_t)) != 0) {
        rc = -1;
    }
    free(patched);
    return rc;
}

static int upload_mesh_build(uint32_t mesh_id, const SceneMeshletBuild *b) {
    if (mesh_id >= K_SCENE_MAX_MESHES || b == nullptr) {
        return -1;
    }
    if (b->meshlet_count == 0u) {
        g_scene.meshes[mesh_id] = (SceneMeshRecord){ .used = true };
        return 0;
    }

    if (g_scene.slab_ml_cursor + b->meshlet_count > K_SCENE_MAX_MESHLETS ||
        g_scene.slab_vx_cursor + b->vertex_count > K_SCENE_VERT_SLAB_CAP ||
        g_scene.slab_tr_cursor + b->triangle_count > K_SCENE_TRI_SLAB_CAP) {
        fprintf(stderr, "[Scene] Meshlet BDA slab full (ml=%u/%u, vx=%u/%u, tr=%u/%u)\n",
                g_scene.slab_ml_cursor + b->meshlet_count, K_SCENE_MAX_MESHLETS,
                g_scene.slab_vx_cursor + b->vertex_count, K_SCENE_VERT_SLAB_CAP,
                g_scene.slab_tr_cursor + b->triangle_count, K_SCENE_TRI_SLAB_CAP);
        return -1;
    }

    const uint32_t ml_base = g_scene.slab_ml_cursor;
    const uint32_t vx_base = g_scene.slab_vx_cursor;
    const uint32_t tr_base = g_scene.slab_tr_cursor;
    if (upload_mesh_build_at(mesh_id, ml_base, vx_base, tr_base, b) != 0) {
        return -1;
    }

    g_scene.slab_ml_cursor += b->meshlet_count;
    g_scene.slab_vx_cursor += b->vertex_count;
    g_scene.slab_tr_cursor += b->triangle_count;

    g_scene.meshes[mesh_id] = (SceneMeshRecord){
        .used          = true,
        .first_meshlet = ml_base,
        .meshlet_count = b->meshlet_count,
        .vert_offset   = vx_base,
        .vert_count    = b->vertex_count,
        .tri_offset    = tr_base,
        .tri_count     = b->triangle_count,
    };
    compute_mesh_proxy_meta(&g_scene.meshes[mesh_id], b);
    return 0;
}

uint32_t scene_register_mesh(const SceneCorner *corners, uint32_t corner_count,
                             float default_roughness) {
    if (!g_scene.ready || g_scene.mesh_count >= K_SCENE_MAX_MESHES || !scene_ensure_gpu_ready()) {
        return K_SCENE_INVALID_MESH;
    }
    SceneMeshletBuild build = {0};
    if (scene_build_meshlets(corners, corner_count, default_roughness, &build) != 0) {
        return K_SCENE_INVALID_MESH;
    }
    const uint32_t id = g_scene.mesh_count;
    if (upload_mesh_build(id, &build) != 0) {
        scene_meshlet_build_free(&build);
        return K_SCENE_INVALID_MESH;
    }
    g_scene.mesh_count++;
    scene_meshlet_build_free(&build);
    return id;
}

uint32_t scene_register_mesh_pbr(const ScenePbrCorner *corners, uint32_t corner_count,
                                 bool has_uv, bool has_tangent) {
    if (!g_scene.ready || g_scene.mesh_count >= K_SCENE_MAX_MESHES || !scene_ensure_gpu_ready()) {
        return K_SCENE_INVALID_MESH;
    }
    SceneMeshletBuild build = {0};
    if (scene_build_meshlets_pbr(corners, corner_count, has_uv, has_tangent, &build) != 0) {
        return K_SCENE_INVALID_MESH;
    }
    const uint32_t id = g_scene.mesh_count;
    if (upload_mesh_build(id, &build) != 0) {
        scene_meshlet_build_free(&build);
        return K_SCENE_INVALID_MESH;
    }
    g_scene.mesh_count++;
    scene_meshlet_build_free(&build);
    return id;
}

SceneSlabMark scene_slab_mark(void) {
    return (SceneSlabMark){
        .mesh_count = g_scene.mesh_count,
        .ml_cursor  = g_scene.slab_ml_cursor,
        .vx_cursor  = g_scene.slab_vx_cursor,
        .tr_cursor  = g_scene.slab_tr_cursor,
    };
}

void scene_slab_restore(SceneSlabMark mark) {
    if (mark.mesh_count > g_scene.mesh_count || mark.ml_cursor > g_scene.slab_ml_cursor ||
        mark.vx_cursor > g_scene.slab_vx_cursor || mark.tr_cursor > g_scene.slab_tr_cursor) {
        return;
    }
    uint64_t wait_pt = 0u;
    for (uint32_t i = mark.mesh_count; i < g_scene.mesh_count; ++i) {
        if (g_scene.meshes[i].last_used_point > wait_pt) {
            wait_pt = g_scene.meshes[i].last_used_point;
        }
        g_scene.meshes[i] = (SceneMeshRecord){0};
    }
    if (wait_pt > 0u && g_scene.vk != nullptr && g_scene.vk->device != VK_NULL_HANDLE &&
        g_scene.upload_sem != VK_NULL_HANDLE && !scene_point_retired(wait_pt)) {
        VkSemaphoreWaitInfo wi = {
            .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1u,
            .pSemaphores    = &g_scene.upload_sem,
            .pValues        = &wait_pt,
        };
        (void)vkWaitSemaphores(g_scene.vk->device, &wi, 3000000000ull);
    }
    g_scene.mesh_count     = mark.mesh_count;
    g_scene.slab_ml_cursor = mark.ml_cursor;
    g_scene.slab_vx_cursor = mark.vx_cursor;
    g_scene.slab_tr_cursor = mark.tr_cursor;
    scene_invalidate_hiz();
}

int scene_update_mesh(uint32_t mesh_id, const SceneCorner *corners,
                      uint32_t corner_count, float default_roughness) {
    if (!g_scene.ready || mesh_id >= g_scene.mesh_count) {
        return -1;
    }
    SceneMeshletBuild build = {0};
    if (scene_build_meshlets(corners, corner_count, default_roughness, &build) != 0) {
        return -1;
    }

    SceneMeshRecord *rec = &g_scene.meshes[mesh_id];
    if (build.meshlet_count == 0u) {
        rec->meshlet_count = 0u;
        scene_meshlet_build_free(&build);
        scene_invalidate_hiz();
        return 0;
    }

    if (build.meshlet_count <= rec->meshlet_count &&
        build.vertex_count <= rec->vert_count &&
        build.triangle_count <= rec->tri_count &&
        scene_point_retired(rec->last_used_point)) {
        int rc = upload_mesh_build_at(mesh_id, rec->first_meshlet, rec->vert_offset,
                                      rec->tri_offset, &build);
        if (rc == 0) {
            rec->meshlet_count = build.meshlet_count;
            compute_mesh_proxy_meta(rec, &build);
            scene_invalidate_hiz();
        }
        scene_meshlet_build_free(&build);
        return rc;
    }

    int rc = upload_mesh_build(mesh_id, &build);
    scene_meshlet_build_free(&build);
    if (rc == 0) {
        scene_invalidate_hiz();
    }
    return rc;
}

void scene_claim_frame(VkSemaphore acquire, uint64_t signal_point) {
    g_scene.upload_sem       = acquire;
    g_scene.cur_signal_point = signal_point;
    if (!g_scene.ready || g_scene.vk == nullptr) {
        return;
    }
    scene_uploads_reap(g_scene.vk, false);
    scene_targets_reap(g_scene.vk, false);
    for (uint32_t i = 0u; i < K_SCENE_MAX_PENDING_UPLOADS; ++i) {
        if (g_scene.pending_uploads[i].in_use && g_scene.pending_uploads[i].recorded &&
            g_scene.pending_uploads[i].signal_point == 0u) {
            g_scene.pending_uploads[i].signal_point = signal_point;
        }
    }
}

void scene_abort_frame(uint64_t signal_point) {
    if (signal_point == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < K_SCENE_MAX_PENDING_UPLOADS; ++i) {
        if (g_scene.pending_uploads[i].in_use &&
            g_scene.pending_uploads[i].signal_point == signal_point) {
            g_scene.pending_uploads[i].recorded     = false;
            g_scene.pending_uploads[i].signal_point = 0u;
        }
    }
    if (g_scene.cur_signal_point == signal_point) {
        g_scene.cur_signal_point = 0u;
    }
}
