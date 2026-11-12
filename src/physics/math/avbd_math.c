#include "physics/physics.h"
#include <math.h>
#include <string.h>

// ============================================================================
// 2026 Augmented Vertex Block Descent (AVBD) Mathematical Kernel (avbd_math.c)
// Reference: Giles, Diaz & Yuksel, "Augmented Vertex Block Descent" (SIGGRAPH 2025/2026)
// ============================================================================

static float clamp_f32(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static uint32_t avbd_hash_u32(uint32_t kind, uint32_t a, uint32_t b) {
    uint32_t h = 0x811C9DC5u;
    h = (h ^ kind) * 0x01000193u;
    h = (h ^ a)    * 0x01000193u;
    h = (h ^ b)    * 0x01000193u;
    return h == 0u ? 1u : h;
}

void avbd_solve_sym3x3(const float h_diag[3],
                       const float h_off[3],
                       const float rhs[3],
                       float       out_dx[3]) {
    // Square-root-free 3x3 LDL^T factorization:
    //   H = [ h00  h01  h02 ]
    //       [ h01  h11  h12 ]
    //       [ h02  h12  h22 ]
    // Scaling-invariant up to k = 1e30 in IEEE fp32 (no O(k^3) determinant overflow, 0 sqrt stalls).
    const float h00 = fmaxf(h_diag[0], 1.0e-12f);
    const float h11 = h_diag[1];
    const float h22 = h_diag[2];
    const float h01 = h_off[0];
    const float h02 = h_off[1];
    const float h12 = h_off[2];

    const float d0     = h00;
    const float inv_d0 = 1.0f / d0;
    const float l10    = h01 * inv_d0;
    const float l20    = h02 * inv_d0;

    const float d1     = fmaxf(h11 - l10 * h01, 1.0e-12f);
    const float inv_d1 = 1.0f / d1;
    const float l21    = (h12 - l20 * h01) * inv_d1;

    const float d2     = fmaxf(h22 - l20 * h02 - d1 * l21 * l21, 1.0e-12f);
    const float inv_d2 = 1.0f / d2;

    // Forward substitution: L * y = rhs
    const float y0 = rhs[0];
    const float y1 = rhs[1] - l10 * y0;
    const float y2 = rhs[2] - l20 * y0 - l21 * y1;

    // Diagonal solve: D * z = y
    const float z0 = y0 * inv_d0;
    const float z1 = y1 * inv_d1;
    const float z2 = y2 * inv_d2;

    // Back substitution: L^T * dx = z
    const float dx2 = z2;
    const float dx1 = z1 - l21 * dx2;
    const float dx0 = z0 - l10 * dx1 - l20 * dx2;

    out_dx[0] = isfinite(dx0) ? dx0 : 0.0f;
    out_dx[1] = isfinite(dx1) ? dx1 : 0.0f;
    out_dx[2] = isfinite(dx2) ? dx2 : 0.0f;
}

uint32_t avbd_build_csr(uint32_t              vert_count,
                        const AvbdConstraint *cons,
                        uint32_t              con_count,
                        uint32_t             *out_row_ptr,
                        uint32_t             *out_col_con,
                        uint32_t              col_cap) {
    if (out_row_ptr == nullptr || vert_count == 0u || vert_count > K_AVBD_MAX_VERTS) {
        if (out_row_ptr != nullptr) {
            out_row_ptr[0] = 0u;
        }
        return 0u;
    }

    uint32_t counts[K_AVBD_MAX_VERTS];
    memset(counts, 0, (size_t)vert_count * sizeof(uint32_t));

    for (uint32_t c = 0u; c < con_count; ++c) {
        uint32_t va = cons[c].vert_a;
        uint32_t vb = cons[c].vert_b;
        if (va < vert_count) {
            counts[va]++;
        }
        if (vb < vert_count && vb != va &&
            (cons[c].kind == AVBD_CON_SPRING_3D || cons[c].kind == AVBD_CON_CONTACT_PAIR)) {
            counts[vb]++;
        }
    }

    uint32_t running = 0u;
    for (uint32_t v = 0u; v < vert_count; ++v) {
        out_row_ptr[v] = running;
        uint32_t add = counts[v];
        if (running + add > col_cap) {
            add = col_cap > running ? (col_cap - running) : 0u;
        }
        counts[v] = running;
        running  += add;
    }
    out_row_ptr[vert_count] = running;

    if (out_col_con == nullptr || running == 0u) {
        return running;
    }

    for (uint32_t c = 0u; c < con_count; ++c) {
        uint32_t va = cons[c].vert_a;
        uint32_t vb = cons[c].vert_b;
        if (va < vert_count && counts[va] < out_row_ptr[va + 1u]) {
            out_col_con[counts[va]++] = c;
        }
        if (vb < vert_count && vb != va &&
            (cons[c].kind == AVBD_CON_SPRING_3D || cons[c].kind == AVBD_CON_CONTACT_PAIR) &&
            counts[vb] < out_row_ptr[vb + 1u]) {
            out_col_con[counts[vb]++] = c;
        }
    }
    return running;
}

uint32_t avbd_color_graph(const AvbdVertex     *verts,
                          uint32_t              vert_count,
                          const AvbdConstraint *cons,
                          uint32_t              con_count,
                          const uint32_t       *csr_row_ptr,
                          const uint32_t       *csr_col_con,
                          uint32_t             *out_color_verts,
                          uint32_t              out_color_start[K_AVBD_MAX_COLORS],
                          uint32_t              out_color_count[K_AVBD_MAX_COLORS]) {
    memset(out_color_start, 0, K_AVBD_MAX_COLORS * sizeof(uint32_t));
    memset(out_color_count, 0, K_AVBD_MAX_COLORS * sizeof(uint32_t));
    if (verts == nullptr || out_color_verts == nullptr ||
        vert_count == 0u || vert_count > K_AVBD_MAX_VERTS) {
        return 0u;
    }

    uint8_t v_color[K_AVBD_MAX_VERTS];
    memset(v_color, 0xFF, (size_t)vert_count);
    uint32_t max_color_used = 0u;
    uint32_t active_dynamic = 0u;

    for (uint32_t v = 0u; v < vert_count; ++v) {
        if (!(verts[v].inv_mass > 0.0f)) {
            continue;
        }
        active_dynamic++;
        uint32_t forbidden = 0u;
        if (csr_row_ptr != nullptr && csr_col_con != nullptr) {
            uint32_t beg = csr_row_ptr[v];
            uint32_t end = csr_row_ptr[v + 1u];
            for (uint32_t e = beg; e < end; ++e) {
                uint32_t cid = csr_col_con[e];
                if (cid >= con_count) {
                    continue;
                }
                const AvbdConstraint *con = &cons[cid];
                if (con->kind != AVBD_CON_SPRING_3D && con->kind != AVBD_CON_CONTACT_PAIR) {
                    continue;
                }
                uint32_t nbr = (con->vert_a == v) ? con->vert_b : con->vert_a;
                if (nbr < vert_count && v_color[nbr] < K_AVBD_MAX_COLORS) {
                    forbidden |= (1u << v_color[nbr]);
                }
            }
        }
        uint32_t chosen = 0u;
        while (chosen + 1u < K_AVBD_MAX_COLORS && (forbidden & (1u << chosen)) != 0u) {
            chosen++;
        }
        v_color[v] = (uint8_t)chosen;
        out_color_count[chosen]++;
        if (chosen + 1u > max_color_used) {
            max_color_used = chosen + 1u;
        }
    }

    if (active_dynamic == 0u || max_color_used == 0u) {
        return 0u;
    }

    uint32_t write_cursor[K_AVBD_MAX_COLORS];
    uint32_t prefix = 0u;
    for (uint32_t c = 0u; c < max_color_used; ++c) {
        out_color_start[c] = prefix;
        write_cursor[c]    = prefix;
        prefix            += out_color_count[c];
    }
    for (uint32_t v = 0u; v < vert_count; ++v) {
        uint8_t c = v_color[v];
        if (c < max_color_used) {
            out_color_verts[write_cursor[c]++] = v;
        }
    }
    return max_color_used;
}

void avbd_warm_cache_reset(AvbdWarmCache *cache) {
    if (cache != nullptr) {
        memset(cache, 0, sizeof(*cache));
    }
}

void avbd_warm_apply(const AvbdWarmCache *cache,
                     AvbdConstraint      *cons,
                     uint32_t             con_count,
                     float                warm_gamma) {
    if (cache == nullptr || cons == nullptr || cache->count == 0u) {
        return;
    }
    const float gamma = clamp_f32(warm_gamma, 0.0f, 0.98f);
    for (uint32_t c = 0u; c < con_count; ++c) {
        uint32_t key = cons[c].hash_key;
        if (key == 0u) {
            continue;
        }
        for (uint32_t i = 0u; i < cache->count; ++i) {
            if (cache->entries[i].hash_key == key) {
                cons[c].lambda[0] = cache->entries[i].lambda[0] * gamma;
                cons[c].lambda[1] = cache->entries[i].lambda[1] * gamma;
                cons[c].lambda[2] = cache->entries[i].lambda[2] * gamma;
                float warm_k = cache->entries[i].penalty_k * gamma;
                cons[c].penalty_k = clamp_f32(warm_k, cons[c].stiffness_min, cons[c].stiffness_max);
                break;
            }
        }
    }
}

void avbd_warm_store(AvbdWarmCache        *cache,
                     const AvbdConstraint *cons,
                     uint32_t              con_count) {
    if (cache == nullptr || cons == nullptr) {
        return;
    }
    cache->count = 0u;
    for (uint32_t c = 0u; c < con_count && cache->count < K_AVBD_WARM_CACHE; ++c) {
        if (cons[c].hash_key == 0u) {
            continue;
        }
        cache->entries[cache->count++] = (AvbdWarmEntry){
            .hash_key  = cons[c].hash_key,
            .age       = 0u,
            .penalty_k = cons[c].penalty_k,
            .lambda    = { cons[c].lambda[0], cons[c].lambda[1], cons[c].lambda[2] },
        };
    }
}

static void avbd_accum_constraint_for_vert(uint32_t              vid,
                                           const AvbdVertex     *verts,
                                           const AvbdConstraint *con,
                                           float                 h_diag[3],
                                           float                 h_off[3],
                                           float                 grad[3]) {
    const float k = fmaxf(con->penalty_k, con->stiffness_min);
    if (!(k > 0.0f)) {
        return;
    }

    switch (con->kind) {
    case AVBD_CON_SPRING_3D: {
        const uint32_t va = con->vert_a;
        const uint32_t vb = con->vert_b;
        const float sgn = (vid == vb) ? 1.0f : -1.0f;
        const float dx  = verts[vb].pos[0] - verts[va].pos[0];
        const float dy  = verts[vb].pos[1] - verts[va].pos[1];
        const float dz  = verts[vb].pos[2] - verts[va].pos[2];
        const float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len < 1.0e-6f) {
            return;
        }
        const float inv_l = 1.0f / len;
        const float ux    = dx * inv_l;
        const float uy    = dy * inv_l;
        const float uz    = dz * inv_l;
        const float viol  = len - con->rest_or_offset;
        const float force = k * viol + con->lambda[0];
        // SPD geometric stiffness stabilization: k (u u^T) + (|f| / len) (I - u u^T)
        const float k_geo = fabsf(force) * inv_l;
        const float k_dir = k - k_geo;

        grad[0] += force * sgn * ux;
        grad[1] += force * sgn * uy;
        grad[2] += force * sgn * uz;

        h_diag[0] += k_geo + k_dir * ux * ux;
        h_diag[1] += k_geo + k_dir * uy * uy;
        h_diag[2] += k_geo + k_dir * uz * uz;
        h_off[0]  += k_dir * ux * uy;
        h_off[1]  += k_dir * ux * uz;
        h_off[2]  += k_dir * uy * uz;
        break;
    }
    case AVBD_CON_ANCHOR_3D: {
        const float rx = verts[vid].pos[0] - con->normal_or_target[0];
        const float ry = verts[vid].pos[1] - con->normal_or_target[1];
        const float rz = verts[vid].pos[2] - con->normal_or_target[2];
        grad[0]   += k * rx + con->lambda[0];
        grad[1]   += k * ry + con->lambda[1];
        grad[2]   += k * rz + con->lambda[2];
        h_diag[0] += k;
        h_diag[1] += k;
        h_diag[2] += k;
        break;
    }
    case AVBD_CON_CONTACT_PLANE: {
        // Unilateral half-space C(x) = n . x - d >= 0; dual force f_hat = min(0, k C + lambda)
        const float nx   = con->normal_or_target[0];
        const float ny   = con->normal_or_target[1];
        const float nz   = con->normal_or_target[2];
        const float gap  = nx * verts[vid].pos[0] + ny * verts[vid].pos[1] + nz * verts[vid].pos[2] - con->rest_or_offset;
        const float fhat = k * gap + con->lambda[0];
        if (fhat < 0.0f) {
            grad[0]   += fhat * nx;
            grad[1]   += fhat * ny;
            grad[2]   += fhat * nz;
            h_diag[0] += k * nx * nx;
            h_diag[1] += k * ny * ny;
            h_diag[2] += k * nz * nz;
            h_off[0]  += k * nx * ny;
            h_off[1]  += k * nx * nz;
            h_off[2]  += k * ny * nz;
        }
        break;
    }
    case AVBD_CON_CONTACT_PAIR: {
        // Unilateral pair separation C(x) = n . (x_b - x_a) - d_sep >= 0
        const uint32_t va  = con->vert_a;
        const uint32_t vb  = con->vert_b;
        const float    sgn = (vid == vb) ? 1.0f : -1.0f;
        const float    nx  = con->normal_or_target[0];
        const float    ny  = con->normal_or_target[1];
        const float    nz  = con->normal_or_target[2];
        const float    dx  = verts[vb].pos[0] - verts[va].pos[0];
        const float    dy  = verts[vb].pos[1] - verts[va].pos[1];
        const float    dz  = verts[vb].pos[2] - verts[va].pos[2];
        const float    gap = (nx * dx + ny * dy + nz * dz) - con->rest_or_offset;
        const float    fhat = k * gap + con->lambda[0];
        if (fhat < 0.0f) {
            grad[0]   += fhat * sgn * nx;
            grad[1]   += fhat * sgn * ny;
            grad[2]   += fhat * sgn * nz;
            h_diag[0] += k * nx * nx;
            h_diag[1] += k * ny * ny;
            h_diag[2] += k * nz * nz;
            h_off[0]  += k * nx * ny;
            h_off[1]  += k * nx * nz;
            h_off[2]  += k * ny * nz;
        }
        break;
    }
    default:
        break;
    }
}

static float avbd_dual_update_all(const AvbdVertex *verts,
                                  uint32_t          vert_count,
                                  AvbdConstraint   *cons,
                                  uint32_t          con_count) {
    float max_viol = 0.0f;
    for (uint32_t c = 0u; c < con_count; ++c) {
        AvbdConstraint *con = &cons[c];
        const float k    = clamp_f32(con->penalty_k, con->stiffness_min, con->stiffness_max);
        const float beta = con->beta_ramp > 1.0f ? con->beta_ramp : 1.0f;

        switch (con->kind) {
        case AVBD_CON_SPRING_3D: {
            if (con->vert_a >= vert_count || con->vert_b >= vert_count) {
                break;
            }
            const float dx   = verts[con->vert_b].pos[0] - verts[con->vert_a].pos[0];
            const float dy   = verts[con->vert_b].pos[1] - verts[con->vert_a].pos[1];
            const float dz   = verts[con->vert_b].pos[2] - verts[con->vert_a].pos[2];
            const float viol = sqrtf(dx * dx + dy * dy + dz * dz) - con->rest_or_offset;
            const float av   = fabsf(viol);
            if (av > max_viol) {
                max_viol = av;
            }
            if (con->stiffness_max > con->stiffness_min) {
                con->lambda[0] += k * viol;
                if (av > 1.0e-4f) {
                    con->penalty_k = fminf(con->stiffness_max, k * beta);
                }
            }
            break;
        }
        case AVBD_CON_ANCHOR_3D: {
            if (con->vert_a >= vert_count) {
                break;
            }
            const float rx = verts[con->vert_a].pos[0] - con->normal_or_target[0];
            const float ry = verts[con->vert_a].pos[1] - con->normal_or_target[1];
            const float rz = verts[con->vert_a].pos[2] - con->normal_or_target[2];
            const float av = sqrtf(rx * rx + ry * ry + rz * rz);
            if (av > max_viol) {
                max_viol = av;
            }
            if (con->stiffness_max > con->stiffness_min) {
                con->lambda[0] += k * rx;
                con->lambda[1] += k * ry;
                con->lambda[2] += k * rz;
                if (av > 1.0e-4f) {
                    con->penalty_k = fminf(con->stiffness_max, k * beta);
                }
            }
            break;
        }
        case AVBD_CON_CONTACT_PLANE: {
            if (con->vert_a >= vert_count) {
                break;
            }
            const float nx  = con->normal_or_target[0];
            const float ny  = con->normal_or_target[1];
            const float nz  = con->normal_or_target[2];
            const float gap = nx * verts[con->vert_a].pos[0] +
                              ny * verts[con->vert_a].pos[1] +
                              nz * verts[con->vert_a].pos[2] - con->rest_or_offset;
            if (gap < 0.0f && -gap > max_viol) {
                max_viol = -gap;
            }
            const float next_l = fminf(0.0f, con->lambda[0] + k * gap);
            con->lambda[0] = next_l;
            if (gap < -1.0e-4f) {
                con->penalty_k = fminf(con->stiffness_max, k * beta);
            }
            break;
        }
        case AVBD_CON_CONTACT_PAIR: {
            if (con->vert_a >= vert_count || con->vert_b >= vert_count) {
                break;
            }
            const float nx  = con->normal_or_target[0];
            const float ny  = con->normal_or_target[1];
            const float nz  = con->normal_or_target[2];
            const float dx  = verts[con->vert_b].pos[0] - verts[con->vert_a].pos[0];
            const float dy  = verts[con->vert_b].pos[1] - verts[con->vert_a].pos[1];
            const float dz  = verts[con->vert_b].pos[2] - verts[con->vert_a].pos[2];
            const float gap = (nx * dx + ny * dy + nz * dz) - con->rest_or_offset;
            if (gap < 0.0f && -gap > max_viol) {
                max_viol = -gap;
            }
            const float next_l = fminf(0.0f, con->lambda[0] + k * gap);
            con->lambda[0] = next_l;
            if (gap < -1.0e-4f) {
                con->penalty_k = fminf(con->stiffness_max, k * beta);
            }
            break;
        }
        default:
            break;
        }
    }
    return max_viol;
}

float avbd_solve_step(AvbdVertex     *verts,
                      uint32_t        vert_count,
                      AvbdConstraint *cons,
                      uint32_t        con_count,
                      float           dt,
                      const float     gravity[3],
                      uint32_t        iterations) {
    if (verts == nullptr || vert_count == 0u || vert_count > K_AVBD_MAX_VERTS || !(dt > 1.0e-6f)) {
        return 0.0f;
    }
    const uint32_t safe_cons  = con_count > K_AVBD_MAX_CONS ? K_AVBD_MAX_CONS : con_count;
    const uint32_t safe_iters = iterations == 0u ? 4u : (iterations > 32u ? 32u : iterations);
    const float    gx         = gravity != nullptr ? gravity[0] : 0.0f;
    const float    gy         = gravity != nullptr ? gravity[1] : 0.0f;
    const float    gz         = gravity != nullptr ? gravity[2] : 0.0f;
    const float    dt2        = dt * dt;
    const float    inv_dt     = 1.0f / dt;
    const float    inv_dt2    = 1.0f / dt2;

    // 1. Inertial prediction y_i = x_i^t + h v_i^t + h^2 g
    for (uint32_t i = 0u; i < vert_count; ++i) {
        AvbdVertex *v = &verts[i];
        v->prev_pos[0] = v->pos[0];
        v->prev_pos[1] = v->pos[1];
        v->prev_pos[2] = v->pos[2];
        if (v->inv_mass > 0.0f) {
            v->inert_y[0] = v->pos[0] + dt * v->vel[0] + dt2 * gx;
            v->inert_y[1] = v->pos[1] + dt * v->vel[1] + dt2 * gy;
            v->inert_y[2] = v->pos[2] + dt * v->vel[2] + dt2 * gz;
            v->pos[0]     = v->inert_y[0];
            v->pos[1]     = v->inert_y[1];
            v->pos[2]     = v->inert_y[2];
        } else {
            v->inert_y[0] = v->pos[0];
            v->inert_y[1] = v->pos[1];
            v->inert_y[2] = v->pos[2];
        }
    }

    if (cons == nullptr || safe_cons == 0u) {
        for (uint32_t i = 0u; i < vert_count; ++i) {
            if (verts[i].inv_mass > 0.0f) {
                verts[i].vel[0] = (verts[i].pos[0] - verts[i].prev_pos[0]) * inv_dt;
                verts[i].vel[1] = (verts[i].pos[1] - verts[i].prev_pos[1]) * inv_dt;
                verts[i].vel[2] = (verts[i].pos[2] - verts[i].prev_pos[2]) * inv_dt;
            }
        }
        return 0.0f;
    }

    for (uint32_t c = 0u; c < safe_cons; ++c) {
        if (cons[c].penalty_k < cons[c].stiffness_min) {
            cons[c].penalty_k = cons[c].stiffness_min;
        }
    }

    // 2. Build CSR vertex-to-constraint incidence & greedy conflict graph coloring
    uint32_t csr_row[K_AVBD_MAX_VERTS + 1u];
    uint32_t csr_col[K_AVBD_MAX_CSR_EDGES];
    (void)avbd_build_csr(vert_count, cons, safe_cons, csr_row, csr_col, K_AVBD_MAX_CSR_EDGES);

    uint32_t color_verts[K_AVBD_MAX_VERTS];
    uint32_t color_start[K_AVBD_MAX_COLORS];
    uint32_t color_count[K_AVBD_MAX_COLORS];
    uint32_t num_colors = avbd_color_graph(verts, vert_count, cons, safe_cons,
                                           csr_row, csr_col,
                                           color_verts, color_start, color_count);

    // 3. Primal-Dual Augmented Vertex Block Descent sweeps
    float max_viol = 0.0f;
    for (uint32_t iter = 0u; iter < safe_iters; ++iter) {
        for (uint32_t col = 0u; col < num_colors; ++col) {
            const uint32_t start = color_start[col];
            const uint32_t cnt   = color_count[col];
            for (uint32_t lane = 0u; lane < cnt; ++lane) {
                const uint32_t vid = color_verts[start + lane];
                AvbdVertex *v      = &verts[vid];
                const float alpha  = 1.0f / (v->inv_mass * dt2 + 1.0e-20f);
                float h_diag[3]    = { alpha, alpha, alpha };
                float h_off[3]     = { 0.0f, 0.0f, 0.0f };
                float grad[3]      = {
                    alpha * (v->pos[0] - v->inert_y[0]),
                    alpha * (v->pos[1] - v->inert_y[1]),
                    alpha * (v->pos[2] - v->inert_y[2]),
                };

                const uint32_t e_beg = csr_row[vid];
                const uint32_t e_end = csr_row[vid + 1u];
                for (uint32_t e = e_beg; e < e_end; ++e) {
                    avbd_accum_constraint_for_vert(vid, verts, &cons[csr_col[e]], h_diag, h_off, grad);
                }

                const float rhs[3] = { -grad[0], -grad[1], -grad[2] };
                float dx[3]        = { 0.0f, 0.0f, 0.0f };
                avbd_solve_sym3x3(h_diag, h_off, rhs, dx);
                v->pos[0] += dx[0];
                v->pos[1] += dx[1];
                v->pos[2] += dx[2];
            }
        }
        max_viol = avbd_dual_update_all(verts, vert_count, cons, safe_cons);
    }

    // 4. Implicit Euler primal velocity extraction
    (void)inv_dt2;
    for (uint32_t i = 0u; i < vert_count; ++i) {
        if (verts[i].inv_mass > 0.0f) {
            verts[i].vel[0] = (verts[i].pos[0] - verts[i].prev_pos[0]) * inv_dt;
            verts[i].vel[1] = (verts[i].pos[1] - verts[i].prev_pos[1]) * inv_dt;
            verts[i].vel[2] = (verts[i].pos[2] - verts[i].prev_pos[2]) * inv_dt;
        }
    }
    return max_viol;
}

static void push_grid_spring(AvbdConstraint *cons,
                             uint32_t       *io_count,
                             uint32_t        cap,
                             uint32_t        va,
                             uint32_t        vb,
                             float           rest,
                             float           stiff) {
    if (*io_count >= cap) {
        return;
    }
    cons[(*io_count)++] = (AvbdConstraint){
        .kind           = AVBD_CON_SPRING_3D,
        .vert_a         = va,
        .vert_b         = vb,
        .hash_key       = avbd_hash_u32(AVBD_CON_SPRING_3D, va, vb),
        .rest_or_offset = rest,
        .stiffness_min  = stiff,
        .stiffness_max  = stiff, // Elastic physical compliance (finite Hookean stiffness)
        .penalty_k      = stiff,
        .beta_ramp      = 1.0f,
    };
}

void avbd_step_soft_grid_2d(float          *io_x,
                            float          *io_y,
                            float          *io_vx,
                            float          *io_vy,
                            uint32_t        cols,
                            uint32_t        rows,
                            float           spacing,
                            float           spring_stiff,
                            float           damp_rate,
                            float           gx,
                            float           gy,
                            float           dt,
                            uint32_t        iterations,
                            AvbdVertex     *opt_gpu_verts,
                            uint32_t       *opt_vert_count,
                            AvbdConstraint *opt_gpu_cons,
                            uint32_t       *opt_con_count) {
    const uint32_t count = cols * rows;
    if (io_x == nullptr || io_y == nullptr || io_vx == nullptr || io_vy == nullptr ||
        cols < 2u || rows < 2u || count > K_AVBD_MAX_VERTS || !(dt > 0.0f)) {
        if (opt_vert_count != nullptr) *opt_vert_count = 0u;
        if (opt_con_count != nullptr)  *opt_con_count  = 0u;
        return;
    }

    AvbdVertex     verts[K_AVBD_MAX_VERTS];
    AvbdConstraint cons[K_AVBD_MAX_CONS];
    memset(verts, 0, (size_t)count * sizeof(AvbdVertex));

    for (uint32_t k = 0u; k < count; ++k) {
        verts[k].pos[0]   = io_x[k];
        verts[k].pos[1]   = io_y[k];
        verts[k].pos[2]   = 0.0f;
        verts[k].vel[0]   = io_vx[k];
        verts[k].vel[1]   = io_vy[k];
        verts[k].vel[2]   = 0.0f;
        verts[k].inv_mass = 1.0f;
    }

    const float k_base  = fmaxf(spring_stiff, 4.0f) * 18.0f;
    const float k_shear = k_base * 0.85f;
    const float k_bend  = k_base * 0.35f;
    const float diag    = spacing * 1.41421356237f;
    uint32_t    con_cnt = 0u;

    // 1. Structural horizontal & vertical Hookean springs
    for (uint32_t r = 0u; r < rows; ++r) {
        for (uint32_t c = 0u; c + 1u < cols; ++c) {
            uint32_t a = r * cols + c;
            push_grid_spring(cons, &con_cnt, K_AVBD_MAX_CONS, a, a + 1u, spacing, k_base);
        }
    }
    for (uint32_t r = 0u; r + 1u < rows; ++r) {
        for (uint32_t c = 0u; c < cols; ++c) {
            uint32_t a = r * cols + c;
            push_grid_spring(cons, &con_cnt, K_AVBD_MAX_CONS, a, a + cols, spacing, k_base);
        }
    }
    // 2. Diagonal shear springs (eliminates 2D quad shear zero-energy modes)
    for (uint32_t r = 0u; r + 1u < rows; ++r) {
        for (uint32_t c = 0u; c + 1u < cols; ++c) {
            uint32_t a = r * cols + c;
            push_grid_spring(cons, &con_cnt, K_AVBD_MAX_CONS, a, a + cols + 1u, diag, k_shear);
            push_grid_spring(cons, &con_cnt, K_AVBD_MAX_CONS, a + 1u, a + cols, diag, k_shear);
        }
    }
    // 3. 2-hop flexion / bending springs (prevents grid inversion under high-speed impact)
    for (uint32_t r = 0u; r < rows; ++r) {
        for (uint32_t c = 0u; c + 2u < cols; ++c) {
            uint32_t a = r * cols + c;
            push_grid_spring(cons, &con_cnt, K_AVBD_MAX_CONS, a, a + 2u, spacing * 2.0f, k_bend);
        }
    }
    for (uint32_t r = 0u; r + 2u < rows; ++r) {
        for (uint32_t c = 0u; c < cols; ++c) {
            uint32_t a = r * cols + c;
            push_grid_spring(cons, &con_cnt, K_AVBD_MAX_CONS, a, a + 2u * cols, spacing * 2.0f, k_bend);
        }
    }

    if (opt_gpu_verts != nullptr && opt_vert_count != nullptr &&
        opt_gpu_cons != nullptr && opt_con_count != nullptr) {
        memcpy(opt_gpu_verts, verts, (size_t)count * sizeof(AvbdVertex));
        memcpy(opt_gpu_cons, cons, (size_t)con_cnt * sizeof(AvbdConstraint));
        *opt_vert_count = count;
        *opt_con_count  = con_cnt;
    }

    const float grav[3] = { gx, gy, 0.0f };
    (void)avbd_solve_step(verts, count, cons, con_cnt, dt, grav, iterations);

    const float keep = expf(-fmaxf(damp_rate, 0.1f) * dt);
    for (uint32_t k = 0u; k < count; ++k) {
        if (isfinite(verts[k].pos[0]) && isfinite(verts[k].pos[1])) {
            io_x[k]  = verts[k].pos[0];
            io_y[k]  = verts[k].pos[1];
            io_vx[k] = verts[k].vel[0] * keep;
            io_vy[k] = verts[k].vel[1] * keep;
        }
    }
}

bool physics_self_check(void) {
    // 1. Verify LDL^T 3x3 solver at extreme stiffness k = 1e18 (where Cramer det = 1e54 overflows fp32!)
    const float k_huge    = 1.0e18f;
    const float h_diag[3] = { 2.0f * k_huge, 3.0f * k_huge, 4.0f * k_huge };
    const float h_off[3]  = { 0.5f * k_huge, 0.25f * k_huge, 0.5f * k_huge };
    const float x_ref[3]  = { 1.25f, -2.50f, 0.75f };
    const float rhs[3]    = {
        h_diag[0] * x_ref[0] + h_off[0]  * x_ref[1] + h_off[1]  * x_ref[2],
        h_off[0]  * x_ref[0] + h_diag[1] * x_ref[1] + h_off[2]  * x_ref[2],
        h_off[1]  * x_ref[0] + h_off[2]  * x_ref[1] + h_diag[2] * x_ref[2],
    };
    float dx[3] = {0};
    avbd_solve_sym3x3(h_diag, h_off, rhs, dx);
    if (fabsf(dx[0] - x_ref[0]) > 1.0e-4f ||
        fabsf(dx[1] - x_ref[1]) > 1.0e-4f ||
        fabsf(dx[2] - x_ref[2]) > 1.0e-4f) {
        return false;
    }

    // 2. Verify 1000:1 high-mass-ratio stack + warm-start cache roundtrip
    AvbdWarmCache warm;
    avbd_warm_cache_reset(&warm);
    AvbdVertex stack[2] = {
        { .pos = { 0.0f, 0.0f, 0.5f }, .inv_mass = 1.0f },     // Light 1 kg block on bottom
        { .pos = { 0.0f, 0.0f, 1.4f }, .inv_mass = 0.001f },   // Heavy 1000 kg block on top
    };
    AvbdConstraint stack_cons[2] = {
        {
            .kind              = AVBD_CON_CONTACT_PLANE,
            .vert_a            = 0u,
            .vert_b            = UINT32_MAX,
            .hash_key          = 101u,
            .normal_or_target  = { 0.0f, 0.0f, 1.0f },
            .rest_or_offset    = 0.5f,
            .stiffness_min     = 1.0e5f,
            .stiffness_max     = 1.0e9f,
            .penalty_k         = 1.0e5f,
            .beta_ramp         = 8.0f,
        },
        {
            .kind              = AVBD_CON_CONTACT_PAIR,
            .vert_a            = 0u,
            .vert_b            = 1u,
            .hash_key          = 102u,
            .normal_or_target  = { 0.0f, 0.0f, 1.0f },
            .rest_or_offset    = 1.0f,
            .stiffness_min     = 1.0e5f,
            .stiffness_max     = 1.0e9f,
            .penalty_k         = 1.0e5f,
            .beta_ramp         = 8.0f,
        },
    };
    const float g_earth[3] = { 0.0f, 0.0f, -9.81f };
    for (uint32_t step = 0u; step < 8u; ++step) {
        avbd_warm_apply(&warm, stack_cons, 2u, 0.85f);
        (void)avbd_solve_step(stack, 2u, stack_cons, 2u, 0.008f, g_earth, 8u);
        avbd_warm_store(&warm, stack_cons, 2u);
    }
    if (stack[0].pos[2] < 0.495f || (stack[1].pos[2] - stack[0].pos[2]) < 0.990f) {
        return false;
    }

    // 3. Verify 3D anchor pin convergence
    AvbdVertex pin_v = { .pos = { 4.0f, -3.0f, 2.0f }, .inv_mass = 1.0f };
    AvbdConstraint pin_c = {
        .kind             = AVBD_CON_ANCHOR_3D,
        .vert_a           = 0u,
        .vert_b           = UINT32_MAX,
        .hash_key         = 201u,
        .normal_or_target = { 0.0f, 0.0f, 0.0f },
        .stiffness_min    = 1.0e6f,
        .stiffness_max    = 1.0e9f,
        .penalty_k        = 1.0e6f,
        .beta_ramp        = 4.0f,
    };
    float viol = avbd_solve_step(&pin_v, 1u, &pin_c, 1u, 0.01f, g_earth, 4u);
    return viol < 0.01f && fabsf(pin_v.pos[0]) < 0.01f;
}
