// Slang 2026 Two-Phase GPU Meshlet Culler (Normal Cone + Frustum + Subgroup Hi-Z)
// Phase 0: Expands active (instance, meshlet) pairs, runs backface cone & frustum culling,
//          tests survivors against previous frame's reprojected Hi-Z pyramid, emits Phase-1
//          indirect draws, and queues Hi-Z-rejected candidates into rejected_pairs[].
// Phase 1: Re-tests rejected_pairs[] against fresh Frame-N Hi-Z pyramid and emits newly
//          disoccluded meshlets into Phase-2 indirect draws (zero 1-frame popping).

#include "scene_common.slang"

struct SceneCullPush {
    uint64_t meshlets_bda;
    uint64_t cull_slot_bda;
    uint64_t hiz_bda;
    float    qx, qy, qz, qw;
    float    cam_x, cam_y, cam_z;
    float    zoom;
    uint     width;
    uint     height;
    uint     work_count;
    uint     phase;        // 0 = Phase 1, 1 = Phase 2
    uint     enable_hiz;   // 1 = Hi-Z occlusion culling active
    float    persp;
};

[[vk::push_constant]] SceneCullPush pc;

groupshared uint s_wave_draws[64];
groupshared uint s_wave_rejs[64];
groupshared uint s_stat_cone;
groupshared uint s_stat_frustum;
groupshared uint s_stat_hiz;
groupshared uint s_max_verts;

bool test_hiz_occluded(float3 world_center, float world_radius, float depth_bias, bool use_prev_cam) {
    if (pc.enable_hiz == 0u || pc.hiz_bda == 0ull) {
        return false;
    }
    SceneHizHeader hdr = ((SceneHizHeader *)pc.hiz_bda)[0];
    if (hdr.mip_count == 0u || hdr.full_width == 0u || hdr.full_height == 0u) {
        return false;
    }
    if (use_prev_cam && hdr.valid == 0u) {
        return false;
    }

    float4 q    = use_prev_cam ? float4(hdr.prev_qx, hdr.prev_qy, hdr.prev_qz, hdr.prev_qw)
                               : float4(pc.qx, pc.qy, pc.qz, pc.qw);
    float3 cam  = use_prev_cam ? float3(hdr.prev_cam_x, hdr.prev_cam_y, hdr.prev_cam_z)
                               : float3(pc.cam_x, pc.cam_y, pc.cam_z);
    float zoom  = use_prev_cam ? hdr.prev_zoom : pc.zoom;
    float persp = use_prev_cam ? asfloat(hdr.pad0) : pc.persp;
    uint  fw    = hdr.full_width;
    uint  fh    = hdr.full_height;

    float3 vc = view_of(world_center, q, cam);
    float  z_sphere_max; // Closest Reverse-Z depth of sphere (higher = closer)
    float  cx_px, cy_px, r_px;

    if (persp > 1.0e-7) {
        float w_near_sph = 1.0 - (vc.z + world_radius) * persp;
        if (w_near_sph <= k_persp_w_near) {
            return false;
        }
        float w_c = max(1.0 - vc.z * persp, k_persp_w_near);
        z_sphere_max = (k_persp_w_near / w_near_sph) + depth_bias + 1.5e-4;
        cx_px = 0.5 * float(fw) + (vc.x / w_c) * zoom;
        cy_px = 0.5 * float(fh) - (vc.y / w_c) * zoom;
        r_px  = (world_radius / w_near_sph) * zoom + 2.0;
    } else {
        z_sphere_max = 0.5 + (vc.z + world_radius) / 8192.0 + depth_bias + 1.5e-4;
        cx_px = 0.5 * float(fw) + vc.x * zoom;
        cy_px = 0.5 * float(fh) - vc.y * zoom;
        r_px  = world_radius * zoom + 1.0;
    }
    if (z_sphere_max >= 1.0) {
        return false;
    }

    // In Phase 1 (reprojecting into frame N-1), anything crossing the old screen border
    // had unrecorded offscreen pixels in frame N-1, so treat it as immediately visible.
    if (use_prev_cam) {
        if (cx_px - r_px < 0.0 || cx_px + r_px >= float(fw) ||
            cy_px - r_px < 0.0 || cy_px + r_px >= float(fh)) {
            return false;
        }
    }

    if (cx_px + r_px < 0.0 || cx_px - r_px >= float(fw) ||
        cy_px + r_px < 0.0 || cy_px - r_px >= float(fh)) {
        return false;
    }

    uint x0 = uint(clamp(floor(cx_px - r_px), 0.0, float(fw - 1u)));
    uint x1 = uint(clamp(ceil(cx_px + r_px),  0.0, float(fw - 1u)));
    uint y0 = uint(clamp(floor(cy_px - r_px), 0.0, float(fh - 1u)));
    uint y1 = uint(clamp(ceil(cy_px + r_px),  0.0, float(fh - 1u)));

    uint span  = max(1u, max(x1 - x0, y1 - y0));
    uint mip   = min(uint(firstbithigh(span)), hdr.mip_count - 1u);
    uint shift = mip + 1u;

    SceneHizMipInfo mi = hdr.mips[mip];
    uint mx0 = min(x0 >> shift, mi.width - 1u);
    uint mx1 = min(x1 >> shift, mi.width - 1u);
    uint my0 = min(y0 >> shift, mi.height - 1u);
    uint my1 = min(y1 >> shift, mi.height - 1u);

    float *hiz_floats = (float *)(pc.hiz_bda + 256ull);
    uint base = mi.offset_floats;
    float d00 = hiz_floats[base + my0 * mi.width + mx0];
    float d10 = hiz_floats[base + my0 * mi.width + mx1];
    float d01 = hiz_floats[base + my1 * mi.width + mx0];
    float d11 = hiz_floats[base + my1 * mi.width + mx1];
    float hiz_min = min(min(d00, d10), min(d01, d11));

    // In Reverse-Z, smaller depth is farther; occluded when sphere's closest z < tile's farthest z
    return z_sphere_max < hiz_min;
}

void compute_meshlet_world_sphere(SceneMeshlet ml, SceneInstance inst,
                                  out float3 out_center, out float out_radius) {
    float sz  = clamp(1.0 + inst.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);
    out_center = scene_transform_point_squash(ml.center, inst);
    out_radius = max(sxy, sz) * inst.bound_scale * ml.radius;
}

bool test_cone_backfacing(SceneMeshlet ml, SceneInstance inst, float4 q_cam,
                          float3 world_center, float world_radius) {
    if ((inst.flags & k_scene_flag_two_sided) != 0u || ml.cone_cutoff_s16 <= -32767) {
        return false;
    }
    float cutoff = float(ml.cone_cutoff_s16) * (1.0 / 32767.0);
    float3 axis  = scene_oct_decode(ml.cone_axis_oct_x, ml.cone_axis_oct_y);

    float sz  = clamp(1.0 + inst.squash, 0.45, 1.85);
    float sxy = rsqrt(sz);
    float3 a0 = inst.world_r0.xyz * sxy;
    float3 a1 = inst.world_r1.xyz * sxy;
    float3 a2 = inst.world_r2.xyz * sz;

    // Conservative anisotropy ratio so non-uniformly scaled / sheared instances never false-cull
    float3 col0 = float3(a0.x, a1.x, a2.x);
    float3 col1 = float3(a0.y, a1.y, a2.y);
    float3 col2 = float3(a0.z, a1.z, a2.z);
    float  s0   = length(col0);
    float  s1   = length(col1);
    float  s2   = length(col2);
    float  smin = min(s0, min(s1, s2));
    float  smax = max(s0, max(s1, s2));
    if (smin < 1.0e-5) {
        return false;
    }

    float3 cam_dir_world;
    float  angular_pad = 0.0;
    if (pc.persp > 1.0e-7) {
        float3 cam_pos = float3(pc.cam_x, pc.cam_y, pc.cam_z);
        float3 eye_w   = cam_pos + scene_inv_view_rotate(q_cam, float3(0.0, 0.0, 1.0 / pc.persp));
        float3 to_eye  = eye_w - world_center;
        float  dist    = length(to_eye);
        if (dist <= world_radius * 1.1) {
            return false;
        }
        cam_dir_world = to_eye / dist;
        angular_pad   = (world_radius / dist) * (smax / smin);
    } else {
        cam_dir_world = scene_inv_view_rotate(q_cam, float3(0.0, 0.0, 1.0));
    }

    // Transform world view direction to local space via adjugate(A) * sign(det(A))
    // so dot(world_n, cam_dir_world) > 0 <=> dot(local_n, w_model) > 0
    float3 c0 = cross(a1, a2);
    float3 c1 = cross(a2, a0);
    float3 c2 = cross(a0, a1);
    float  ds = dot(a0, c0) < 0.0 ? -1.0 : 1.0;
    float3 vm = (c0 * cam_dir_world.x + c1 * cam_dir_world.y + c2 * cam_dir_world.z) * ds;
    float  vl2 = dot(vm, vm);
    if (vl2 < 1.0e-12) {
        return false;
    }
    float3 w_model = vm * rsqrt(vl2);

    return dot(axis, w_model) < (cutoff - angular_pad);
}

bool test_sphere_in_frustum(float3 world_center, float world_radius, float depth_bias) {
    float4 q   = float4(pc.qx, pc.qy, pc.qz, pc.qw);
    float3 cam = float3(pc.cam_x, pc.cam_y, pc.cam_z);
    float3 vc  = view_of(world_center, q, cam);

    float half_w_world = (max(float(pc.width),  1.0) * 0.5) / max(pc.zoom, 1.0e-6);
    float half_h_world = (max(float(pc.height), 1.0) * 0.5) / max(pc.zoom, 1.0e-6);

    if (pc.persp > 1.0e-7) {
        float w_far_sph  = 1.0 - (vc.z - world_radius) * pc.persp;
        if (w_far_sph < k_persp_w_near) {
            return false;
        }
        float w_max = max(w_far_sph, k_persp_w_near);
        float rx = world_radius * sqrt(1.0 + (half_w_world * pc.persp) * (half_w_world * pc.persp));
        float ry = world_radius * sqrt(1.0 + (half_h_world * pc.persp) * (half_h_world * pc.persp));
        if (vc.x + rx < -half_w_world * w_max || vc.x - rx > half_w_world * w_max ||
            vc.y + ry < -half_h_world * w_max || vc.y - ry > half_h_world * w_max) {
            return false;
        }
        return true;
    }

    if (vc.x + world_radius < -half_w_world || vc.x - world_radius > half_w_world ||
        vc.y + world_radius < -half_h_world || vc.y - world_radius > half_h_world) {
        return false;
    }

    float z_max = 0.5 + (vc.z + world_radius) / 8192.0 + depth_bias;
    float z_min = 0.5 + (vc.z - world_radius) / 8192.0 + depth_bias;
    if (z_max < 0.0 || z_min > 1.0) {
        return false;
    }
    return true;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID) {
    uint tid = gtid.x;
    if (pc.meshlets_bda == 0ull || pc.cull_slot_bda == 0ull) {
        return;
    }

    uint *hdr_words      = (uint *)pc.cull_slot_bda;
    uint *rejected_pairs = (uint *)(pc.cull_slot_bda + k_slot_off_rejected);
    uint *work_pairs     = (uint *)(pc.cull_slot_bda + k_slot_off_work);
    SceneInstance *insts = (SceneInstance *)(pc.cull_slot_bda + k_slot_off_instances);
    SceneMeshlet  *mls   = (SceneMeshlet *)pc.meshlets_bda;

    SceneDrawIndirectCmd *out_cmds = (pc.phase == 0u)
        ? (SceneDrawIndirectCmd *)(pc.cull_slot_bda + k_slot_off_cmds1)
        : (SceneDrawIndirectCmd *)(pc.cull_slot_bda + k_slot_off_cmds2);

    uint total_items = (pc.phase == 0u)
        ? min(pc.work_count, k_scene_max_draws)
        : min(hdr_words[2], k_scene_max_draws); // hdr_words[2] == rejected_count from Phase 0

    if (tid == 0u) {
        s_stat_cone    = 0u;
        s_stat_frustum = 0u;
        s_stat_hiz     = 0u;
        s_max_verts    = 0u;
        if (pc.phase == 0u) {
            hdr_words[0] = 0u;
            hdr_words[1] = 0u;
            hdr_words[2] = 0u;
            hdr_words[3] = total_items;
            hdr_words[4] = 0u;
            hdr_words[5] = 0u;
            hdr_words[6] = 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    uint   wave_id     = tid / max(WaveGetLaneCount(), 1u);
    uint   num_waves   = (256u + max(WaveGetLaneCount(), 1u) - 1u) / max(WaveGetLaneCount(), 1u);
    float4 q_cam       = float4(pc.qx, pc.qy, pc.qz, pc.qw);
    uint   total_draws = 0u;
    uint   total_rejs  = 0u;

    for (uint base = 0u; base < total_items; base += 256u) {
        uint idx = base + tid;
        bool emit_draw = false;
        bool emit_rej  = false;
        bool c_cone    = false;
        bool c_frust   = false;
        bool c_hiz     = false;
        uint pair      = 0u;
        uint vcount    = 0u;

        if (idx < total_items) {
            pair = (pc.phase == 0u) ? work_pairs[idx] : rejected_pairs[idx];
            uint inst_id    = pair >> 16u;
            uint meshlet_id = pair & 0xFFFFu;
            SceneInstance inst = insts[inst_id];
            SceneMeshlet  ml   = mls[meshlet_id];
            vcount = uint(ml.tri_count) * 3u;

            float3 w_center;
            float  w_radius;
            compute_meshlet_world_sphere(ml, inst, w_center, w_radius);

            if (pc.phase == 0u) {
                if (test_cone_backfacing(ml, inst, q_cam, w_center, w_radius)) {
                    c_cone = true;
                } else if (!test_sphere_in_frustum(w_center, w_radius, inst.depth_bias)) {
                    c_frust = true;
                } else if (test_hiz_occluded(w_center, w_radius, inst.depth_bias, true)) {
                    emit_rej = true;
                } else {
                    emit_draw = true;
                }
            } else {
                // Phase 2: already passed cone & frustum in Phase 1; test fresh Frame-N Hi-Z!
                if (test_hiz_occluded(w_center, w_radius, inst.depth_bias, false)) {
                    c_hiz = true;
                } else {
                    emit_draw = true;
                }
            }
        }

        // Subgroup wave-level prefix compaction + workgroup scan
        uint wave_draw_prefix = WavePrefixCountBits(emit_draw);
        uint wave_draw_total  = WaveActiveCountBits(emit_draw);
        uint wave_rej_prefix  = WavePrefixCountBits(emit_rej);
        uint wave_rej_total   = WaveActiveCountBits(emit_rej);
        uint wave_cone_sum    = WaveActiveCountBits(c_cone);
        uint wave_frust_sum   = WaveActiveCountBits(c_frust);
        uint wave_hiz_sum     = WaveActiveCountBits(c_hiz);
        uint wave_v_max       = WaveActiveMax(emit_draw ? vcount : 0u);

        if (WaveIsFirstLane()) {
            s_wave_draws[wave_id] = wave_draw_total;
            s_wave_rejs[wave_id]  = wave_rej_total;
            InterlockedAdd(s_stat_cone, wave_cone_sum);
            InterlockedAdd(s_stat_frustum, wave_frust_sum);
            InterlockedAdd(s_stat_hiz, wave_hiz_sum);
            InterlockedMax(s_max_verts, wave_v_max);
        }
        GroupMemoryBarrierWithGroupSync();

        uint wg_draw_base = total_draws;
        uint wg_rej_base  = total_rejs;
        uint batch_draws  = 0u;
        uint batch_rejs   = 0u;
        for (uint w = 0u; w < num_waves; ++w) {
            uint wd = s_wave_draws[w];
            uint wr = s_wave_rejs[w];
            if (w < wave_id) {
                wg_draw_base += wd;
                wg_rej_base  += wr;
            }
            batch_draws += wd;
            batch_rejs  += wr;
        }
        total_draws += batch_draws;
        total_rejs  += batch_rejs;

        if (emit_draw) {
            uint slot = wg_draw_base + wave_draw_prefix;
            SceneDrawIndirectCmd cmd;
            cmd.vertexCount   = vcount;
            cmd.instanceCount = 1u;
            cmd.firstVertex   = 0u;
            cmd.firstInstance = pair;
            out_cmds[slot]    = cmd;
        }
        if (emit_rej) {
            uint slot = wg_rej_base + wave_rej_prefix;
            rejected_pairs[slot] = pair;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0u) {
        if (pc.phase == 0u) {
            hdr_words[0] = total_draws;
            hdr_words[2] = total_rejs;
            hdr_words[4] = s_stat_cone;
            hdr_words[5] = s_stat_frustum;

            SceneDrawIndirectCmd *single1 = (SceneDrawIndirectCmd *)(pc.cull_slot_bda + k_slot_off_single1);
            SceneDrawIndirectCmd sc;
            sc.vertexCount   = s_max_verts;
            sc.instanceCount = total_draws;
            sc.firstVertex   = 0u;
            sc.firstInstance = 0u;
            single1[0]       = sc;
        } else {
            hdr_words[1] = total_draws;
            hdr_words[6] = s_stat_hiz;

            SceneDrawIndirectCmd *single2 = (SceneDrawIndirectCmd *)(pc.cull_slot_bda + k_slot_off_single2);
            SceneDrawIndirectCmd sc;
            sc.vertexCount   = s_max_verts;
            sc.instanceCount = total_draws;
            sc.firstVertex   = 0u;
            sc.firstInstance = 0u;
            single2[0]       = sc;
        }
    }
}
