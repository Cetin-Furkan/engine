#include "scene_gpu_int.h"
#include "lighting/lighting.h"
#include "texture/texture.h"
#include <math.h>
#include <string.h>

void scene_begin_frame(uint32_t width, uint32_t height, const SceneViewParams *view) {
    (void)width;
    (void)height;
    g_scene.prepared_this_frame = false;
    if (!g_scene.ready || view == nullptr) {
        g_scene.cur_instances   = 0u;
        g_scene.cur_work_items  = 0u;
        g_scene.cur_blend_items = 0u;
        g_scene.blend_entry_cnt = 0u;
        g_scene.any_two_sided   = false;
        return;
    }
    g_scene.frame_slot      = (uint32_t)(g_scene.frame_counter % K_SCENE_CULL_RING_SLOTS);
    g_scene.cur_instances   = 0u;
    g_scene.cur_work_items  = 0u;
    g_scene.cur_blend_items = 0u;
    g_scene.blend_entry_cnt = 0u;
    g_scene.any_two_sided   = false;
    g_scene.cur_view        = *view;
}

static float mat3x4_spectral_bound(const float m[12]) {
    const float c0[3] = { m[0], m[4], m[8]  };
    const float c1[3] = { m[1], m[5], m[9]  };
    const float c2[3] = { m[2], m[6], m[10] };
    const float g00 = c0[0]*c0[0] + c0[1]*c0[1] + c0[2]*c0[2];
    const float g11 = c1[0]*c1[0] + c1[1]*c1[1] + c1[2]*c1[2];
    const float g22 = c2[0]*c2[0] + c2[1]*c2[1] + c2[2]*c2[2];
    const float g01 = fabsf(c0[0]*c1[0] + c0[1]*c1[1] + c0[2]*c1[2]);
    const float g02 = fabsf(c0[0]*c2[0] + c0[1]*c2[1] + c0[2]*c2[2]);
    const float g12 = fabsf(c1[0]*c2[0] + c1[1]*c2[1] + c1[2]*c2[2]);
    const float r0  = g00 + g01 + g02;
    const float r1  = g11 + g01 + g12;
    const float r2  = g22 + g02 + g12;
    return sqrtf(fmaxf(fmaxf(r0, fmaxf(r1, r2)), 1.0e-8f));
}

static float mat3x4_det3(const float m[12]) {
    const float c0x = m[5] * m[10] - m[6] * m[9];
    const float c0y = m[6] * m[8]  - m[4] * m[10];
    const float c0z = m[4] * m[9]  - m[5] * m[8];
    return m[0] * c0x + m[1] * c0y + m[2] * c0z;
}

static float point_view_z(const SceneViewParams *v, float wx, float wy, float wz) {
    const float rx = wx - v->cam_x;
    const float ry = wy - v->cam_y;
    const float rz = wz - v->cam_z;
    const float tx = 2.0f * (v->qy * rz - v->qz * ry);
    const float ty = 2.0f * (v->qz * rx - v->qx * rz);
    const float tz = 2.0f * (v->qx * ry - v->qy * rx);
    return rz + v->qw * tz + (v->qx * ty - v->qy * tx);
}

static void submit_instance_impl(uint32_t mesh_id, const float world_m[12],
                                 float squash, float roughness, float depth_bias,
                                 uint32_t mat_index, uint32_t pbr_mat_id,
                                 uint32_t flags, uint32_t entity_id) {
    if (!g_scene.ready || world_m == nullptr || mesh_id >= g_scene.mesh_count ||
        !g_scene.meshes[mesh_id].used || g_scene.meshes[mesh_id].meshlet_count == 0u ||
        g_scene.cur_instances >= K_SCENE_MAX_INSTANCES) {
        return;
    }
    SceneMeshRecord *rec = &g_scene.meshes[mesh_id];
    const bool is_blend = (flags & SCENE_INST_FLAG_ALPHA_BLEND) != 0u;
    if (!is_blend && g_scene.cur_work_items + rec->meshlet_count > K_SCENE_MAX_DRAWS) {
        return;
    }
    if (is_blend && g_scene.cur_blend_items + rec->meshlet_count > K_SCENE_MAX_BLEND_DRAWS) {
        return;
    }
    if (g_scene.cur_signal_point != 0u) {
        rec->last_used_point = g_scene.cur_signal_point;
    }

    if (mat3x4_det3(world_m) < 0.0f) {
        flags |= SCENE_INST_FLAG_FLIP_WINDING;
        g_scene.any_two_sided = true;
    }
    if ((flags & SCENE_INST_FLAG_TWO_SIDED) != 0u) {
        g_scene.any_two_sided = true;
    }

    const float bound_scale = mat3x4_spectral_bound(world_m);
    SceneCullSlot *slot = scene_cull_slot_cpu(g_scene.frame_slot);
    const uint32_t inst_idx = g_scene.cur_instances++;
    SceneInstance inst = {
        .squash        = squash,
        .roughness     = roughness,
        .depth_bias    = depth_bias,
        .bound_scale   = bound_scale,
        .mat_index     = mat_index,
        .mesh_id       = mesh_id,
        .first_meshlet = rec->first_meshlet,
        .meshlet_count = rec->meshlet_count,
        .flags         = flags,
        .entity_id     = entity_id,
        .pbr_mat_id    = pbr_mat_id,
        .pad1          = 0u,
    };
    memcpy(inst.world_m, world_m, sizeof inst.world_m);
    slot->instances[inst_idx] = inst;

    const float sz  = fminf(fmaxf(1.0f + squash, 0.45f), 1.85f);
    const float sxy = 1.0f / sqrtf(sz);
    const float lx  = rec->local_center[0];
    const float ly  = rec->local_center[1];
    const float lz  = rec->local_center[2];

    const float c_world_x = (world_m[0]*lx + world_m[1]*ly + world_m[2]*lz)  * sxy + world_m[3];
    const float c_world_y = (world_m[4]*lx + world_m[5]*ly + world_m[6]*lz)  * sxy + world_m[7];
    const float c_world_z = (world_m[8]*lx + world_m[9]*ly + world_m[10]*lz) * sz  + world_m[11];

    if (is_blend) {
        if (g_scene.blend_entry_cnt < K_SCENE_MAX_INSTANCES) {
            g_scene.blend_entries[g_scene.blend_entry_cnt++] = (SceneBlendEntry){
                .inst_idx = inst_idx,
                .first_ml = rec->first_meshlet,
                .ml_count = rec->meshlet_count,
                .view_z   = point_view_z(&g_scene.cur_view, c_world_x, c_world_y, c_world_z),
            };
            g_scene.cur_blend_items += rec->meshlet_count;
        }
    } else {
        for (uint32_t m = 0u; m < rec->meshlet_count; ++m) {
            slot->work_pairs[g_scene.cur_work_items++] = (inst_idx << 16u) | (rec->first_meshlet + m);
        }
    }

    const float hx = rec->half_extents[0];
    const float hy = rec->half_extents[1];
    const float hz = rec->half_extents[2];
    const float scale_max = fmaxf(sxy, sz) * bound_scale;
    const float bound_r   = sqrtf(hx * hx + hy * hy + hz * hz) * scale_max * 1.04f + 0.5f;

    g_scene.inst_bounds[inst_idx][0] = c_world_x;
    g_scene.inst_bounds[inst_idx][1] = c_world_y;
    g_scene.inst_bounds[inst_idx][2] = c_world_z;
    g_scene.inst_bounds[inst_idx][3] = bound_r;

    if (!is_blend) {
        const float aabb_hx = (fabsf(world_m[0])*hx + fabsf(world_m[1])*hy + fabsf(world_m[2])*hz)  * sxy;
        const float aabb_hy = (fabsf(world_m[4])*hx + fabsf(world_m[5])*hy + fabsf(world_m[6])*hz)  * sxy;
        const float aabb_hz = (fabsf(world_m[8])*hx + fabsf(world_m[9])*hy + fabsf(world_m[10])*hz) * sz;

        float emi[3] = {0};
        if (pbr_mat_id == 0u) {
            lighting_material_get_emissive(mat_index, emi);
        }
        const float alb[3]      = { rec->avg_albedo[0], rec->avg_albedo[1], rec->avg_albedo[2] };
        const float p_center[3] = { c_world_x, c_world_y, c_world_z };
        const float p_ext[3]    = { fmaxf(aabb_hx, 1.0f), fmaxf(aabb_hy, 1.0f), fmaxf(aabb_hz, 1.0f) };
        LightingProxy proxy = {0};
        lighting_proxy_fill(&proxy, LIGHTING_PROXY_ELLIPSOID, p_center, p_ext,
                            0.0f, 0.0f, roughness > 0.0f ? roughness : rec->avg_roughness,
                            alb, emi);
        lighting_submit_proxy(&proxy);
    }
}

void scene_submit_instance(uint32_t mesh_id, const float world_m[12],
                           float squash, float roughness, float depth_bias,
                           uint32_t mat_index, uint32_t flags, uint32_t entity_id) {
    submit_instance_impl(mesh_id, world_m, squash, roughness, depth_bias,
                         mat_index, 0u, flags, entity_id);
}

void scene_submit_instance_pbr(uint32_t mesh_id, const float world_m[12],
                               uint32_t pbr_mat_id, uint32_t flags, uint32_t entity_id) {
    submit_instance_impl(mesh_id, world_m, 0.0f, 0.0f, 0.0f,
                         0u, pbr_mat_id, flags | SCENE_INST_FLAG_PBR, entity_id);
}

static void cmd_push_sampled_image(VkCommandBuffer cmd, VkPipelineBindPoint bind_point,
                                   VkPipelineLayout layout, VkImageView view) {
    VkDescriptorImageInfo img_info = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = view,
        .imageLayout = g_scene.vk->device_info.supports_unified_image_layouts
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = VK_NULL_HANDLE,
        .dstBinding      = 0u,
        .dstArrayElement = 0u,
        .descriptorCount = 1u,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo      = &img_info,
    };
    vulkan_cmd_push_descriptors(g_scene.vk, cmd, bind_point, layout, 0u, 1u, &write);
}

static void cmd_push_resolve_descriptors(VkCommandBuffer cmd, VkPipelineLayout layout, VkImageView vis_view) {
    VkImageView   sh_view    = VK_NULL_HANDLE;
    VkSampler     sh_sampler = VK_NULL_HANDLE;
    VkImageLayout sh_layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    lighting_shadow_get_descriptor(&sh_view, &sh_sampler, &sh_layout);

    VkDescriptorImageInfo vis_info = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = vis_view,
        .imageLayout = g_scene.vk->device_info.supports_unified_image_layouts
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDescriptorImageInfo sh_info = {
        .sampler     = sh_sampler,
        .imageView   = sh_view,
        .imageLayout = sh_layout,
    };
    VkWriteDescriptorSet writes[2] = {
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = VK_NULL_HANDLE,
            .dstBinding      = 0u,
            .dstArrayElement = 0u,
            .descriptorCount = 1u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo      = &vis_info,
        },
        {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = VK_NULL_HANDLE,
            .dstBinding      = 1u,
            .dstArrayElement = 0u,
            .descriptorCount = 1u,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &sh_info,
        },
    };
    vulkan_cmd_push_descriptors(g_scene.vk, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0u, 2u, writes);
}

static void bind_bindless_set(VkCommandBuffer cmd, VkPipelineLayout layout) {
    VkDescriptorSet ds = texture_bindless_set();
    if (ds != VK_NULL_HANDLE && layout != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                1u, 1u, &ds, 0u, nullptr);
    }
}

static void emit_indirect_draws(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t phase,
                                uint32_t width, uint32_t height) {
    const bool full_indirect = g_scene.vk->device_info.supports_draw_indirect_count &&
                               g_scene.vk->device_info.supports_multi_draw_indirect &&
                               g_scene.vk->device_info.supports_draw_indirect_first_instance;

    SceneDrawPush push = {
        .meshlets_bda   = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_MESHLETS,
        .vertices_bda   = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_VERTICES,
        .triangles_bda  = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_TRIANGLES,
        .cull_slot_bda  = scene_cull_slot_bda(g_scene.frame_slot),
        .pbr_mats_bda   = texture_materials_bda(),
        .width          = width,
        .height         = height,
        .phase_cmd_mode = full_indirect ? 0u : (phase == 0u ? 1u : 2u),
        .draw_flags     = g_scene.any_two_sided ? 1u : 0u,
        .view           = g_scene.cur_view,
    };

    vulkan_cmd_push(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);

    VkDeviceSize base_off = scene_cull_slot_buf_offset(g_scene.frame_slot);
    if (full_indirect) {
        VkDeviceSize cmds_off  = base_off + (phase == 0u ? offsetof(SceneCullSlot, phase1_cmds)
                                                         : offsetof(SceneCullSlot, phase2_cmds));
        VkDeviceSize count_off = base_off + (phase == 0u ? offsetof(SceneCullSlot, phase1_draw_count)
                                                         : offsetof(SceneCullSlot, phase2_draw_count));
        vkCmdDrawIndirectCount(cmd, g_scene.cull_buf.buffer, cmds_off,
                               g_scene.cull_buf.buffer, count_off,
                               g_scene.cur_work_items, sizeof(SceneDrawIndirectCmd));
    } else {
        VkDeviceSize single_off = base_off + (phase == 0u ? offsetof(SceneCullSlot, phase1_single_cmd)
                                                          : offsetof(SceneCullSlot, phase2_single_cmd));
        vkCmdDrawIndirect(cmd, g_scene.cull_buf.buffer, single_off, 1u, sizeof(SceneDrawIndirectCmd));
    }
}

static void record_offscreen_pass(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                  uint32_t phase, bool clear_attachments) {
    VulkanGfxLive *gfx = g_scene.visbuf_enabled ? &g_scene.visbuf_gfx : &g_scene.depth_only_gfx;
    if (!vulkan_gfx_bind(g_scene.vk, cmd, gfx)) {
        return;
    }
    vkCmdSetCullMode(cmd, g_scene.any_two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
    if (g_scene.visbuf_enabled) {
        bind_bindless_set(cmd, g_scene.draw_layout);
    }

    const bool unified = g_scene.vk->device_info.supports_unified_image_layouts;
    VkRenderingAttachmentInfo color_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = g_scene.vis_color.view,
        .imageLayout = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = clear_attachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .uint32 = { 0u, 0u, 0u, 0u } } },
    };
    VkRenderingAttachmentInfo depth_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = g_scene.vis_depth.view,
        .imageLayout = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp      = clear_attachments ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .depthStencil = { .depth = 0.0f, .stencil = 0u } },
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { .offset = { 0, 0 }, .extent = { width, height } },
        .layerCount           = 1u,
        .colorAttachmentCount = g_scene.visbuf_enabled ? 1u : 0u,
        .pColorAttachments    = g_scene.visbuf_enabled ? &color_att : nullptr,
        .pDepthAttachment     = &depth_att,
    };

    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)width, .height = (float)height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D sc = { .offset = { 0, 0 }, .extent = { width, height } };
    vkCmdSetViewport(cmd, 0u, 1u, &vp);
    vkCmdSetScissor(cmd, 0u, 1u, &sc);
    emit_indirect_draws(cmd, g_scene.draw_layout, phase, width, height);
    vkCmdEndRendering(cmd);
}

static void record_hiz_pyramid(VkCommandBuffer cmd, bool commit_prev) {
    if (!g_scene.can_hiz_push || g_scene.hiz_pipe == VK_NULL_HANDLE || g_scene.hiz_hdr_cpu.mip_count == 0u) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_scene.hiz_pipe);
    cmd_push_sampled_image(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_scene.hiz_layout, g_scene.vis_depth.view);

    SceneHizPush p0 = {
        .hiz_bda       = g_scene.hiz_buf.address,
        .cull_slot_bda = scene_cull_slot_bda(g_scene.frame_slot),
        .qx            = g_scene.cur_view.qx,
        .qy            = g_scene.cur_view.qy,
        .qz            = g_scene.cur_view.qz,
        .qw            = g_scene.cur_view.qw,
        .cam_x         = g_scene.cur_view.cam_x,
        .cam_y         = g_scene.cur_view.cam_y,
        .cam_z         = g_scene.cur_view.cam_z,
        .zoom          = g_scene.cur_view.zoom,
        .mode          = 0u,
        .commit_prev   = commit_prev ? 1u : 0u,
        .persp         = g_scene.cur_view.persp,
    };
    vulkan_cmd_push(cmd, g_scene.hiz_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof p0, &p0);
    uint32_t wg_x = (g_scene.hiz_hdr_cpu.mips[0].width + 15u) / 16u;
    uint32_t wg_y = (g_scene.hiz_hdr_cpu.mips[0].height + 15u) / 16u;
    vkCmdDispatch(cmd, wg_x, wg_y, 1u);

    VkMemoryBarrier2 mid_bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    VkDependencyInfo mid_dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &mid_bar,
    };
    vkCmdPipelineBarrier2(cmd, &mid_dep);

    if (g_scene.hiz_hdr_cpu.mip_count > 5u || commit_prev) {
        SceneHizPush p1 = p0;
        p1.mode        = 1u;
        p1.commit_prev = commit_prev ? 1u : 0u;
        vulkan_cmd_push(cmd, g_scene.hiz_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof p1, &p1);
        vkCmdDispatch(cmd, 1u, 1u, 1u);
        vkCmdPipelineBarrier2(cmd, &mid_dep);
    }
}

static void prepare_sorted_blend_pairs(void) {
    if (g_scene.blend_entry_cnt == 0u || g_scene.cull_buf.map == nullptr) {
        g_scene.cur_blend_items = 0u;
        return;
    }
    for (uint32_t i = 1u; i < g_scene.blend_entry_cnt; ++i) {
        SceneBlendEntry key = g_scene.blend_entries[i];
        uint32_t j = i;
        while (j > 0u && g_scene.blend_entries[j - 1u].view_z > key.view_z) {
            g_scene.blend_entries[j] = g_scene.blend_entries[j - 1u];
            j--;
        }
        g_scene.blend_entries[j] = key;
    }
    SceneCullSlot *slot = scene_cull_slot_cpu(g_scene.frame_slot);
    uint32_t cursor = 0u;
    for (uint32_t i = 0u; i < g_scene.blend_entry_cnt; ++i) {
        const SceneBlendEntry *e = &g_scene.blend_entries[i];
        for (uint32_t m = 0u; m < e->ml_count && cursor < K_SCENE_MAX_BLEND_DRAWS; ++m) {
            slot->blend_pairs[cursor++] = (e->inst_idx << 16u) | (e->first_ml + m);
        }
    }
    g_scene.cur_blend_items = cursor;
}

void scene_prepare_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    g_scene.prepared_this_frame = false;
    g_scene.used_hiz_this_frame = false;
    if (!g_scene.ready || !g_scene.gpu_ready || cmd == VK_NULL_HANDLE) {
        return;
    }
    scene_uploads_flush(cmd);
    texture_flush_uploads(cmd);
    prepare_sorted_blend_pairs();

    if (g_scene.cur_instances > 0u && g_scene.geom_slab.address != 0ull) {
        lighting_set_scene_geom(
            g_scene.geom_slab.address + K_SCENE_SLAB_OFF_MESHLETS,
            g_scene.geom_slab.address + K_SCENE_SLAB_OFF_VERTICES,
            g_scene.geom_slab.address + K_SCENE_SLAB_OFF_TRIANGLES,
            scene_cull_slot_bda(g_scene.frame_slot) + offsetof(SceneCullSlot, instances),
            g_scene.cur_instances,
            g_scene.inst_bounds);
    }
    if (width == 0u || height == 0u || (g_scene.cur_work_items == 0u && g_scene.cur_blend_items == 0u)) {
        return;
    }
    if (g_scene.cur_work_items == 0u) {
        g_scene.prepared_this_frame = true;
        g_scene.frame_counter++;
        return;
    }

    const uint32_t hiz_auto_min =
        (g_scene.vk->device_info.execution_profile == VULKAN_PROFILE_UMA_IGPU)
            ? K_SCENE_HIZ_AUTO_UMA_MIN
            : K_SCENE_HIZ_AUTO_DGPU_MIN;
    bool want_hiz = false;
    if (g_scene.can_hiz_push) {
        if (g_scene.hiz_policy == SCENE_HIZ_ON) {
            want_hiz = true;
        } else if (g_scene.hiz_policy == SCENE_HIZ_AUTO) {
            want_hiz = (g_scene.cur_work_items >= hiz_auto_min);
        }
    }

    const bool need_offscreen = g_scene.visbuf_enabled || want_hiz;
    const bool screen_ok      = !need_offscreen ||
                                scene_targets_ensure(width, height, g_scene.visbuf_enabled, want_hiz) == 0;
    const bool use_hiz        = want_hiz && screen_ok;
    const bool use_visbuf     = g_scene.visbuf_enabled && screen_ok;

    if (use_hiz && g_scene.hiz_header_dirty) {
        vkCmdUpdateBuffer(cmd, g_scene.hiz_buf.buffer, g_scene.hiz_buf.offset,
                          sizeof(SceneHizHeader), &g_scene.hiz_hdr_cpu);
        VkMemoryBarrier2 up_bar = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        };
        VkDependencyInfo up_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers    = &up_bar,
        };
        vkCmdPipelineBarrier2(cmd, &up_dep);
        g_scene.hiz_header_dirty = false;
    }

    SceneCullPush cull_p1 = {
        .meshlets_bda  = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_MESHLETS,
        .cull_slot_bda = scene_cull_slot_bda(g_scene.frame_slot),
        .hiz_bda       = use_hiz ? g_scene.hiz_buf.address : 0ull,
        .qx            = g_scene.cur_view.qx,
        .qy            = g_scene.cur_view.qy,
        .qz            = g_scene.cur_view.qz,
        .qw            = g_scene.cur_view.qw,
        .cam_x         = g_scene.cur_view.cam_x,
        .cam_y         = g_scene.cur_view.cam_y,
        .cam_z         = g_scene.cur_view.cam_z,
        .zoom          = g_scene.cur_view.zoom,
        .width         = width,
        .height        = height,
        .work_count    = g_scene.cur_work_items,
        .phase         = 0u,
        .enable_hiz    = use_hiz ? 1u : 0u,
        .persp         = g_scene.cur_view.persp,
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_scene.cull_pipe);
    vulkan_cmd_push(cmd, g_scene.cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof cull_p1, &cull_p1);
    vkCmdDispatch(cmd, 1u, 1u, 1u);

    VkMemoryBarrier2 cull1_bar = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    };
    VkDependencyInfo cull1_dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1u,
        .pMemoryBarriers    = &cull1_bar,
    };
    vkCmdPipelineBarrier2(cmd, &cull1_dep);

    if (!use_hiz && !use_visbuf) {
        g_scene.prepared_this_frame = true;
        g_scene.frame_counter++;
        return;
    }

    const bool unified = g_scene.vk->device_info.supports_unified_image_layouts;
    VkImageMemoryBarrier2 pre_p1_imgs[2] = {0};
    uint32_t n_pre_p1 = 0u;
    pre_p1_imgs[n_pre_p1++] = (VkImageMemoryBarrier2){
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask       = 0u,
        .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = g_scene.vis_depth.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u },
    };
    if (use_visbuf) {
        pre_p1_imgs[n_pre_p1++] = (VkImageMemoryBarrier2){
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = 0u,
            .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = g_scene.vis_color.image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
        };
    }
    VkDependencyInfo pre_p1_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = n_pre_p1,
        .pImageMemoryBarriers    = pre_p1_imgs,
    };
    vkCmdPipelineBarrier2(cmd, &pre_p1_dep);

    record_offscreen_pass(cmd, width, height, 0u, true);

    if (!use_hiz) {
        if (unified) {
            VkMemoryBarrier2 vis_mem = {
                .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            };
            VkDependencyInfo vis_dep = {
                .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1u,
                .pMemoryBarriers    = &vis_mem,
            };
            vkCmdPipelineBarrier2(cmd, &vis_dep);
        } else {
            VkImageMemoryBarrier2 vis_to_read = {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = g_scene.vis_color.image,
                .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
            };
            VkDependencyInfo vis_read_dep = {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1u,
                .pImageMemoryBarriers    = &vis_to_read,
            };
            vkCmdPipelineBarrier2(cmd, &vis_read_dep);
        }
        g_scene.prepared_this_frame = true;
        g_scene.frame_counter++;
        return;
    }

    VkImageMemoryBarrier2 d_to_read = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = g_scene.vis_depth.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u },
    };
    if (unified) {
        VkMemoryBarrier2 d_mem = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        };
        VkDependencyInfo d_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers    = &d_mem,
        };
        vkCmdPipelineBarrier2(cmd, &d_dep);
    } else {
        VkDependencyInfo d_read_dep = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers    = &d_to_read,
        };
        vkCmdPipelineBarrier2(cmd, &d_read_dep);
    }

    record_hiz_pyramid(cmd, false);

    SceneCullPush cull_p2 = cull_p1;
    cull_p2.phase = 1u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_scene.cull_pipe);
    vulkan_cmd_push(cmd, g_scene.cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof cull_p2, &cull_p2);
    vkCmdDispatch(cmd, 1u, 1u, 1u);
    vkCmdPipelineBarrier2(cmd, &cull1_dep);

    if (unified) {
        VkMemoryBarrier2 att_mem = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        };
        VkDependencyInfo att_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers    = &att_mem,
        };
        vkCmdPipelineBarrier2(cmd, &att_dep);
    } else {
        VkImageMemoryBarrier2 d_to_att = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask       = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = g_scene.vis_depth.image,
            .subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u },
        };
        VkDependencyInfo d_att_dep = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1u,
            .pImageMemoryBarriers    = &d_to_att,
        };
        vkCmdPipelineBarrier2(cmd, &d_att_dep);
    }

    record_offscreen_pass(cmd, width, height, 1u, false);

    if (unified) {
        VkMemoryBarrier2 post_mem = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                             (use_visbuf ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : 0ull),
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             (use_visbuf ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : 0ull),
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             (use_visbuf ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : 0ull),
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        };
        VkDependencyInfo post_dep = {
            .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1u,
            .pMemoryBarriers    = &post_mem,
        };
        vkCmdPipelineBarrier2(cmd, &post_dep);
    } else {
        VkImageMemoryBarrier2 post_p2_imgs[2] = {0};
        uint32_t n_post_p2 = 0u;
        post_p2_imgs[n_post_p2++] = d_to_read;
        if (use_visbuf) {
            post_p2_imgs[n_post_p2++] = (VkImageMemoryBarrier2){
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = g_scene.vis_color.image,
                .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u },
            };
        }
        VkDependencyInfo post_p2_dep = {
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = n_post_p2,
            .pImageMemoryBarriers    = post_p2_imgs,
        };
        vkCmdPipelineBarrier2(cmd, &post_p2_dep);
    }

    record_hiz_pyramid(cmd, true);
    lighting_set_scene_hiz(g_scene.hiz_buf.address);
    g_scene.screen_last_pt      = g_scene.cur_signal_point;
    g_scene.used_hiz_this_frame = true;
    g_scene.prepared_this_frame = true;
    g_scene.frame_counter++;
}

void scene_draw_shadow(VkCommandBuffer cmd, const float light_clip_vp[16]) {
    if (!g_scene.ready || !g_scene.gpu_ready || cmd == VK_NULL_HANDLE ||
        light_clip_vp == nullptr || g_scene.cur_work_items == 0u ||
        g_scene.geom_slab.address == 0ull || g_scene.cull_buf.address == 0ull) {
        return;
    }
    if (!vulkan_gfx_bind(g_scene.vk, cmd, &g_scene.shadow_gfx)) {
        return;
    }
    SceneShadowPush push = {
        .meshlets_bda  = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_MESHLETS,
        .vertices_bda  = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_VERTICES,
        .triangles_bda = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_TRIANGLES,
        .cull_slot_bda = scene_cull_slot_bda(g_scene.frame_slot),
    };
    memcpy(push.light_vp, light_clip_vp, sizeof push.light_vp);
    vulkan_cmd_push(cmd, g_scene.shadow_layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof push, &push);
    vkCmdDraw(cmd, K_SCENE_MESHLET_MAX_TRIS * 3u, g_scene.cur_work_items, 0u, 0u);
}

static void draw_translucent_pass(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (g_scene.cur_blend_items == 0u) {
        return;
    }
    if (!vulkan_gfx_bind(g_scene.vk, cmd, &g_scene.meshlet_blend_gfx)) {
        return;
    }
    lighting_bind_shadow(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_scene.meshlet_layout);
    bind_bindless_set(cmd, g_scene.meshlet_layout);
    SceneDrawPush push = {
        .meshlets_bda   = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_MESHLETS,
        .vertices_bda   = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_VERTICES,
        .triangles_bda  = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_TRIANGLES,
        .cull_slot_bda  = scene_cull_slot_bda(g_scene.frame_slot),
        .pbr_mats_bda   = texture_materials_bda(),
        .width          = width,
        .height         = height,
        .phase_cmd_mode = 3u,
        .draw_flags     = 1u,
        .view           = g_scene.cur_view,
    };
    vulkan_cmd_push(cmd, g_scene.meshlet_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    sizeof push, &push);
    vkCmdDraw(cmd, K_SCENE_MESHLET_MAX_TRIS * 3u, g_scene.cur_blend_items, 0u, 0u);
}

void scene_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!g_scene.ready || !g_scene.prepared_this_frame || cmd == VK_NULL_HANDLE ||
        width == 0u || height == 0u || (g_scene.cur_work_items == 0u && g_scene.cur_blend_items == 0u)) {
        return;
    }

    if (g_scene.cur_work_items > 0u) {
        if (g_scene.visbuf_enabled && g_scene.vis_color.view != VK_NULL_HANDLE) {
            if (vulkan_gfx_bind(g_scene.vk, cmd, &g_scene.resolve_gfx)) {
                SceneDrawPush push = {
                    .meshlets_bda   = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_MESHLETS,
                    .vertices_bda   = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_VERTICES,
                    .triangles_bda  = g_scene.geom_slab.address + K_SCENE_SLAB_OFF_TRIANGLES,
                    .cull_slot_bda  = scene_cull_slot_bda(g_scene.frame_slot),
                    .pbr_mats_bda   = texture_materials_bda(),
                    .width          = width,
                    .height         = height,
                    .phase_cmd_mode = 0u,
                    .draw_flags     = g_scene.any_two_sided ? 1u : 0u,
                    .view           = g_scene.cur_view,
                };
                cmd_push_resolve_descriptors(cmd, g_scene.resolve_layout, g_scene.vis_color.view);
                bind_bindless_set(cmd, g_scene.resolve_layout);
                vulkan_cmd_push(cmd, g_scene.resolve_layout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                sizeof push, &push);
                vkCmdDraw(cmd, 3u, 1u, 0u, 0u);
                g_scene.screen_last_pt = g_scene.cur_signal_point;
            }
        } else if (vulkan_gfx_bind(g_scene.vk, cmd, &g_scene.meshlet_gfx)) {
            vkCmdSetCullMode(cmd, g_scene.any_two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            lighting_bind_shadow(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_scene.meshlet_layout);
            bind_bindless_set(cmd, g_scene.meshlet_layout);
            emit_indirect_draws(cmd, g_scene.meshlet_layout, 0u, width, height);
            if (g_scene.used_hiz_this_frame) {
                emit_indirect_draws(cmd, g_scene.meshlet_layout, 1u, width, height);
            }
        }
    }

    draw_translucent_pass(cmd, width, height);
}

void scene_get_stats(SceneBenchStats *out_stats) {
    if (out_stats == nullptr) {
        return;
    }
    *out_stats = (SceneBenchStats){0};
    if (!g_scene.ready) {
        return;
    }

    out_stats->registered_meshes = g_scene.mesh_count;
    out_stats->slab_meshlets     = g_scene.slab_ml_cursor;
    out_stats->slab_bytes        = (uint64_t)g_scene.slab_ml_cursor * sizeof(SceneMeshlet) +
                                   (uint64_t)g_scene.slab_vx_cursor * sizeof(ScenePackedVertex) +
                                   (uint64_t)g_scene.slab_tr_cursor * sizeof(uint32_t);
    out_stats->hiz_bytes         = g_scene.hiz_buf.bytes;
    out_stats->active_instances  = g_scene.cur_instances;
    out_stats->hiz_mips          = g_scene.hiz_hdr_cpu.mip_count;
    out_stats->visbuf_enabled    = g_scene.visbuf_enabled;
    out_stats->hiz_enabled       = g_scene.used_hiz_this_frame;

    if (g_scene.cur_work_items > 0u && g_scene.cull_buf.map != nullptr && g_scene.frame_counter > 0u) {
        uint64_t retired_seq = g_scene.frame_counter >= 3u ? (g_scene.frame_counter - 3u) : 0u;
        const SceneCullSlot *ret = scene_cull_slot_cpu((uint32_t)(retired_seq % K_SCENE_CULL_RING_SLOTS));
        out_stats->active_meshlets  = ret->active_meshlets;
        out_stats->phase1_draws     = ret->phase1_draw_count;
        out_stats->phase2_draws     = ret->phase2_draw_count;
        out_stats->visible_meshlets = ret->phase1_draw_count + ret->phase2_draw_count;
        out_stats->cone_culled      = ret->cone_culled;
        out_stats->frustum_culled   = ret->frustum_culled;
        out_stats->hiz_culled       = ret->hiz_culled;
    }
}
