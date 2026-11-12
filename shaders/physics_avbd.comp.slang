// Slang 2026 Fused Workgroup SLM Augmented Vertex Block Descent (AVBD) Compute Kernel
// Reference: Giles, Diaz & Yuksel, "Augmented Vertex Block Descent" (SIGGRAPH 2025/2026)
// - Descriptorless Vulkan 1.4 64-bit Buffer Device Address (BDA) pointers (0 VkDescriptorSets)
// - Stages vertex positions & inertial targets in workgroup SLM (groupshared)
// - Race-free parallel primal block updates per greedy conflict-graph color
// - Square-root-free 3x3 LDL^T register solver (immune to fp32 O(k^3) overflow)
// - Parallel Rockafellar-Wets unilateral contact & bilateral spring/anchor dual updates

static const uint k_avbd_magic      = 0x44425641u;
static const uint k_avbd_max_verts  = 256u;
static const uint k_avbd_max_cons   = 512u;
static const uint k_avbd_max_colors = 16u;

static const uint k_con_spring_3d     = 0u;
static const uint k_con_anchor_3d     = 1u;
static const uint k_con_contact_plane = 2u;
static const uint k_con_contact_pair  = 3u;

struct PhysicsComputePush {
    uint64_t hdr_bda;
    uint     flags;
    uint     pad0;
};

[[vk::push_constant]] PhysicsComputePush pc;

// Matches 64-byte AvbdVertex in include/physics/physics.h
struct AvbdVertexGpu {
    float pos_x, pos_y, pos_z, inv_mass;
    float vel_x, vel_y, vel_z, radius;
    float prev_x, prev_y, prev_z, restitution;
    float inert_x, inert_y, inert_z;
    uint  flags;
};

// Matches 64-byte AvbdConstraint in include/physics/physics.h
struct AvbdConstraintGpu {
    uint  kind;
    uint  vert_a;
    uint  vert_b;
    uint  hash_key;
    float nx, ny, nz;
    float rest_or_offset;
    float stiffness_min;
    float stiffness_max;
    float penalty_k;
    float lambda0;
    float lambda1;
    float lambda2;
    float beta_ramp;
    float compliance_pad;
};

// Matches 256-byte AvbdStepHeader in include/physics/physics.h
struct AvbdStepHeaderGpu {
    uint     magic;
    uint     vert_count;
    uint     con_count;
    uint     num_colors;
    uint     iterations;
    uint     csr_edge_count;
    float    dt;
    float    warm_decay;
    float    gx, gy, gz;
    float    damp_keep;
    uint64_t verts_bda;
    uint64_t cons_bda;
    uint64_t csr_row_bda;
    uint64_t csr_col_bda;
    uint64_t color_verts_bda;
    uint64_t stats_bda;
    uint     color_start[16];
    uint     color_count[16];
    uint     gpu_completed_iters;
    float    max_violation;
    uint     reserved[6];
};

groupshared float3 s_pos[k_avbd_max_verts];
groupshared float3 s_prev[k_avbd_max_verts];
groupshared float3 s_inert_y[k_avbd_max_verts];
groupshared float  s_inv_m[k_avbd_max_verts];
groupshared uint   s_max_viol_bits;

// Square-root-free 3x3 LDL^T linear solver in scalar registers
float3 solve_sym3x3_ldlt(float3 h_diag, float3 h_off, float3 rhs) {
    float d0     = max(h_diag.x, 1.0e-12);
    float inv_d0 = 1.0 / d0;
    float l10    = h_off.x * inv_d0;
    float l20    = h_off.y * inv_d0;

    float d1     = max(h_diag.y - l10 * h_off.x, 1.0e-12);
    float inv_d1 = 1.0 / d1;
    float l21    = (h_off.z - l20 * h_off.x) * inv_d1;

    float d2     = max(h_diag.z - l20 * h_off.y - d1 * l21 * l21, 1.0e-12);
    float inv_d2 = 1.0 / d2;

    float y0 = rhs.x;
    float y1 = rhs.y - l10 * y0;
    float y2 = rhs.z - l20 * y0 - l21 * y1;

    float z0 = y0 * inv_d0;
    float z1 = y1 * inv_d1;
    float z2 = y2 * inv_d2;

    float dx2 = z2;
    float dx1 = z1 - l21 * dx2;
    float dx0 = z0 - l10 * dx1 - l20 * dx2;

    float3 res = float3(dx0, dx1, dx2);
    return any(isnan(res)) || any(isinf(res)) ? float3(0.0) : res;
}

void accum_constraint_slm(
    uint              vid,
    float3            p_self,
    AvbdConstraintGpu con,
    inout float3      h_diag,
    inout float3      h_off,
    inout float3      grad)
{
    float k = max(con.penalty_k, con.stiffness_min);
    if (!(k > 0.0)) {
        return;
    }

    if (con.kind == k_con_spring_3d) {
        bool   is_b = (vid == con.vert_b);
        float  sgn  = is_b ? 1.0 : -1.0;
        float3 d    = is_b ? (p_self - s_pos[con.vert_a]) : (s_pos[con.vert_b] - p_self);
        float  len  = length(d);
        if (len < 1.0e-6) {
            return;
        }
        float  inv_l = 1.0 / len;
        float3 u     = d * inv_l;
        float  viol  = len - con.rest_or_offset;
        float  force = k * viol + con.lambda0;
        float  k_geo = abs(force) * inv_l;
        float  k_dir = k - k_geo;

        grad   += u * (force * sgn);
        h_diag += float3(k_geo) + k_dir * (u * u);
        h_off  += k_dir * float3(u.x * u.y, u.x * u.z, u.y * u.z);
    } else if (con.kind == k_con_anchor_3d) {
        float3 target = float3(con.nx, con.ny, con.nz);
        float3 r      = p_self - target;
        grad   += k * r + float3(con.lambda0, con.lambda1, con.lambda2);
        h_diag += float3(k);
    } else if (con.kind == k_con_contact_plane) {
        float3 n    = float3(con.nx, con.ny, con.nz);
        float  gap  = dot(n, p_self) - con.rest_or_offset;
        float  fhat = k * gap + con.lambda0;
        if (fhat < 0.0) {
            grad   += n * fhat;
            h_diag += k * (n * n);
            h_off  += k * float3(n.x * n.y, n.x * n.z, n.y * n.z);
        }
    } else if (con.kind == k_con_contact_pair) {
        bool   is_b = (vid == con.vert_b);
        float  sgn  = is_b ? 1.0 : -1.0;
        float3 n    = float3(con.nx, con.ny, con.nz);
        float3 d    = is_b ? (p_self - s_pos[con.vert_a]) : (s_pos[con.vert_b] - p_self);
        float  gap  = dot(n, d) - con.rest_or_offset;
        float  fhat = k * gap + con.lambda0;
        if (fhat < 0.0) {
            grad   += n * (fhat * sgn);
            h_diag += k * (n * n);
            h_off  += k * float3(n.x * n.y, n.x * n.z, n.y * n.z);
        }
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID) {
    uint tid = gtid.x;
    if (pc.hdr_bda == 0ull) {
        return;
    }

    AvbdStepHeaderGpu *hdr = (AvbdStepHeaderGpu *)pc.hdr_bda;
    if (hdr->magic != k_avbd_magic) {
        return;
    }

    uint vert_count = min(hdr->vert_count, k_avbd_max_verts);
    uint con_count  = min(hdr->con_count, k_avbd_max_cons);
    uint num_colors = min(hdr->num_colors, k_avbd_max_colors);
    uint iters      = clamp(hdr->iterations, 1u, 32u);
    float dt        = hdr->dt;
    if (vert_count == 0u || !(dt > 1.0e-6)) {
        return;
    }

    AvbdVertexGpu     *verts       = (AvbdVertexGpu *)hdr->verts_bda;
    AvbdConstraintGpu *cons        = (AvbdConstraintGpu *)hdr->cons_bda;
    uint              *csr_row     = (uint *)hdr->csr_row_bda;
    uint              *csr_col     = (uint *)hdr->csr_col_bda;
    uint              *color_verts = (uint *)hdr->color_verts_bda;

    float  dt2    = dt * dt;
    float  inv_dt = 1.0 / dt;
    float3 grav   = float3(hdr->gx, hdr->gy, hdr->gz);

    if (tid == 0u) {
        s_max_viol_bits = 0u;
    }

    // 1. Cooperative SLM stage + primal inertial prediction y_i = x_i^t + h v_i^t + h^2 g
    if (tid < vert_count) {
        AvbdVertexGpu v = verts[tid];
        float3 p0   = float3(v.pos_x, v.pos_y, v.pos_z);
        float3 vel0 = float3(v.vel_x, v.vel_y, v.vel_z);
        s_prev[tid]  = p0;
        s_inv_m[tid] = v.inv_mass;
        if (v.inv_mass > 0.0) {
            float3 y        = p0 + dt * vel0 + dt2 * grav;
            s_inert_y[tid]  = y;
            s_pos[tid]      = y;
        } else {
            s_inert_y[tid]  = p0;
            s_pos[tid]      = p0;
        }
    }

    for (uint c = tid; c < con_count; c += 256u) {
        if (cons[c].penalty_k < cons[c].stiffness_min) {
            cons[c].penalty_k = cons[c].stiffness_min;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // 2. Fused SLM Primal-Dual Augmented Vertex Block Descent sweeps
    for (uint iter = 0u; iter < iters; ++iter) {
        for (uint col = 0u; col < num_colors; ++col) {
            uint   c_start = hdr->color_start[col];
            uint   c_count = hdr->color_count[col];
            uint   vid     = 0u;
            float3 p_next  = float3(0.0);
            if (tid < c_count) {
                vid           = color_verts[c_start + tid];
                float3 p_self = s_pos[vid];
                float  alpha  = 1.0 / (s_inv_m[vid] * dt2 + 1.0e-20);
                float3 h_diag = float3(alpha);
                float3 h_off  = float3(0.0);
                float3 grad   = alpha * (p_self - s_inert_y[vid]);

                uint e_beg = csr_row[vid];
                uint e_end = csr_row[vid + 1u];
                for (uint e = e_beg; e < e_end; ++e) {
                    uint cid = csr_col[e];
                    if (cid < con_count) {
                        accum_constraint_slm(vid, p_self, cons[cid], h_diag, h_off, grad);
                    }
                }

                float3 dx = solve_sym3x3_ldlt(h_diag, h_off, -grad);
                p_next    = p_self + dx;
            }
            GroupMemoryBarrierWithGroupSync();
            if (tid < c_count) {
                s_pos[vid] = p_next;
            }
            GroupMemoryBarrierWithGroupSync();
        }

        // Parallel dual multiplier & adaptive penalty update across all constraints
        float lane_max_viol = 0.0;
        for (uint c = tid; c < con_count; c += 256u) {
            AvbdConstraintGpu con = cons[c];
            float k    = clamp(con.penalty_k, con.stiffness_min, con.stiffness_max);
            float beta = max(con.beta_ramp, 1.0);

            if (con.kind == k_con_spring_3d && con.vert_a < vert_count && con.vert_b < vert_count) {
                float viol = length(s_pos[con.vert_b] - s_pos[con.vert_a]) - con.rest_or_offset;
                float av   = abs(viol);
                lane_max_viol = max(lane_max_viol, av);
                if (con.stiffness_max > con.stiffness_min) {
                    con.lambda0 += k * viol;
                    if (av > 1.0e-4) {
                        con.penalty_k = min(con.stiffness_max, k * beta);
                    }
                }
            } else if (con.kind == k_con_anchor_3d && con.vert_a < vert_count) {
                float3 r  = s_pos[con.vert_a] - float3(con.nx, con.ny, con.nz);
                float  av = length(r);
                lane_max_viol = max(lane_max_viol, av);
                if (con.stiffness_max > con.stiffness_min) {
                    con.lambda0 += k * r.x;
                    con.lambda1 += k * r.y;
                    con.lambda2 += k * r.z;
                    if (av > 1.0e-4) {
                        con.penalty_k = min(con.stiffness_max, k * beta);
                    }
                }
            } else if (con.kind == k_con_contact_plane && con.vert_a < vert_count) {
                float3 n   = float3(con.nx, con.ny, con.nz);
                float  gap = dot(n, s_pos[con.vert_a]) - con.rest_or_offset;
                if (gap < 0.0) {
                    lane_max_viol = max(lane_max_viol, -gap);
                }
                con.lambda0 = min(0.0, con.lambda0 + k * gap);
                if (gap < -1.0e-4) {
                    con.penalty_k = min(con.stiffness_max, k * beta);
                }
            } else if (con.kind == k_con_contact_pair && con.vert_a < vert_count && con.vert_b < vert_count) {
                float3 n   = float3(con.nx, con.ny, con.nz);
                float  gap = dot(n, s_pos[con.vert_b] - s_pos[con.vert_a]) - con.rest_or_offset;
                if (gap < 0.0) {
                    lane_max_viol = max(lane_max_viol, -gap);
                }
                con.lambda0 = min(0.0, con.lambda0 + k * gap);
                if (gap < -1.0e-4) {
                    con.penalty_k = min(con.stiffness_max, k * beta);
                }
            }
            cons[c] = con;
        }

        if (iter + 1u == iters && lane_max_viol > 0.0) {
            InterlockedMax(s_max_viol_bits, asuint(lane_max_viol));
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // 3. Commit solved primal positions & implicit Euler velocities back to BDA
    if (tid < vert_count) {
        AvbdVertexGpu v = verts[tid];
        float3 p1   = s_pos[tid];
        float3 p0   = s_prev[tid];
        float3 in_y = s_inert_y[tid];
        v.pos_x   = p1.x;
        v.pos_y   = p1.y;
        v.pos_z   = p1.z;
        v.prev_x  = p0.x;
        v.prev_y  = p0.y;
        v.prev_z  = p0.z;
        v.inert_x = in_y.x;
        v.inert_y = in_y.y;
        v.inert_z = in_y.z;
        if (v.inv_mass > 0.0) {
            float3 vel1 = ((p1 - p0) * inv_dt) * hdr->damp_keep;
            v.vel_x     = vel1.x;
            v.vel_y     = vel1.y;
            v.vel_z     = vel1.z;
        }
        verts[tid] = v;
    }

    if (tid == 0u) {
        hdr->gpu_completed_iters = iters;
        hdr->max_violation       = asfloat(s_max_viol_bits);
    }
}
