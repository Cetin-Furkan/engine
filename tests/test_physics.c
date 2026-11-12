#include "physics/physics.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // 1. Built-in self-check: 1e18 LDL^T conditioning, 1000:1 mass-ratio stack, 3D anchor pin
    if (!physics_self_check()) {
        fprintf(stderr, "FAIL: physics_self_check failed\n");
        return 1;
    }

    // 2. CSR incidence + greedy conflict graph coloring on an 8x8 structural+shear+bend grid
    enum {
        k_cols = 8,
        k_rows = 8,
        k_vcap = k_cols * k_rows,
        k_ccap = 512,
    };
    AvbdVertex     grid_v[k_vcap];
    AvbdConstraint grid_c[k_ccap];
    uint32_t       con_n = 0u;
    memset(grid_v, 0, sizeof grid_v);
    memset(grid_c, 0, sizeof grid_c);

    for (uint32_t r = 0u; r < k_rows; ++r) {
        for (uint32_t c = 0u; c < k_cols; ++c) {
            uint32_t idx         = r * k_cols + c;
            grid_v[idx].pos[0]   = (float)c * 0.5f;
            grid_v[idx].pos[1]   = (float)r * 0.5f;
            grid_v[idx].pos[2]   = 0.0f;
            grid_v[idx].inv_mass = (r == 0u && (c == 0u || c == k_cols - 1u)) ? 0.0f : 1.0f;
        }
    }

    for (uint32_t r = 0u; r < k_rows; ++r) {
        for (uint32_t c = 0u; ++c <= k_cols;) {
            uint32_t col = c - 1u;
            uint32_t i   = r * k_cols + col;
            if (col + 1u < k_cols) {
                grid_c[con_n++] = (AvbdConstraint){
                    .kind = AVBD_CON_SPRING_3D, .vert_a = i, .vert_b = i + 1u,
                    .rest_or_offset = 0.5f, .stiffness_min = 400.0f, .stiffness_max = 1.0e6f, .penalty_k = 400.0f
                };
            }
            if (r + 1u < k_rows) {
                grid_c[con_n++] = (AvbdConstraint){
                    .kind = AVBD_CON_SPRING_3D, .vert_a = i, .vert_b = i + k_cols,
                    .rest_or_offset = 0.5f, .stiffness_min = 400.0f, .stiffness_max = 1.0e6f, .penalty_k = 400.0f
                };
            }
            if (col + 1u < k_cols && r + 1u < k_rows) {
                grid_c[con_n++] = (AvbdConstraint){
                    .kind = AVBD_CON_SPRING_3D, .vert_a = i, .vert_b = i + k_cols + 1u,
                    .rest_or_offset = 0.70710678f, .stiffness_min = 250.0f, .stiffness_max = 1.0e6f, .penalty_k = 250.0f
                };
                grid_c[con_n++] = (AvbdConstraint){
                    .kind = AVBD_CON_SPRING_3D, .vert_a = i + 1u, .vert_b = i + k_cols,
                    .rest_or_offset = 0.70710678f, .stiffness_min = 250.0f, .stiffness_max = 1.0e6f, .penalty_k = 250.0f
                };
            }
            if (col + 2u < k_cols) {
                grid_c[con_n++] = (AvbdConstraint){
                    .kind = AVBD_CON_SPRING_3D, .vert_a = i, .vert_b = i + 2u,
                    .rest_or_offset = 1.0f, .stiffness_min = 120.0f, .stiffness_max = 1.0e6f, .penalty_k = 120.0f
                };
            }
            if (r + 2u < k_rows) {
                grid_c[con_n++] = (AvbdConstraint){
                    .kind = AVBD_CON_SPRING_3D, .vert_a = i, .vert_b = i + 2u * k_cols,
                    .rest_or_offset = 1.0f, .stiffness_min = 120.0f, .stiffness_max = 1.0e6f, .penalty_k = 120.0f
                };
            }
        }
    }

    uint32_t csr_row[k_vcap + 1];
    uint32_t csr_col[K_AVBD_MAX_CSR_EDGES];
    uint32_t nnz = avbd_build_csr(k_vcap, grid_c, con_n, csr_row, csr_col, K_AVBD_MAX_CSR_EDGES);
    if (nnz != con_n * 2u) {
        fprintf(stderr, "FAIL: CSR incidence count mismatch (%u vs %u)\n", nnz, con_n * 2u);
        return 1;
    }

    uint32_t color_verts[k_vcap];
    uint32_t color_start[K_AVBD_MAX_COLORS];
    uint32_t color_count[K_AVBD_MAX_COLORS];
    uint32_t num_colors = avbd_color_graph(grid_v, k_vcap, grid_c, con_n,
                                           csr_row, csr_col,
                                           color_verts, color_start, color_count);
    if (num_colors == 0u || num_colors > K_AVBD_MAX_COLORS) {
        fprintf(stderr, "FAIL: invalid graph color count %u\n", num_colors);
        return 1;
    }

    uint32_t color_of[k_vcap];
    uint8_t  seen[k_vcap] = {0};
    uint32_t total_colored = 0u;
    for (uint32_t c = 0u; c < num_colors; ++c) {
        for (uint32_t k = 0u; k < color_count[c]; ++k) {
            uint32_t v = color_verts[color_start[c] + k];
            if (v >= k_vcap || seen[v] != 0u || grid_v[v].inv_mass <= 0.0f) {
                fprintf(stderr, "FAIL: graph coloring vertex bijection broken at v=%u\n", v);
                return 1;
            }
            seen[v]     = 1u;
            color_of[v] = c;
            total_colored++;
        }
    }
    if (total_colored != (uint32_t)(k_vcap - 2)) {
        fprintf(stderr, "FAIL: colored %u / %u dynamic vertices\n", total_colored, (uint32_t)(k_vcap - 2));
        return 1;
    }

    for (uint32_t ci = 0u; ci < con_n; ++ci) {
        uint32_t a = grid_c[ci].vert_a;
        uint32_t b = grid_c[ci].vert_b;
        if (grid_v[a].inv_mass > 0.0f && grid_v[b].inv_mass > 0.0f && color_of[a] == color_of[b]) {
            fprintf(stderr, "FAIL: monochromatic conflict edge (%u, %u) in color %u\n",
                    a, b, color_of[a]);
            return 1;
        }
    }

    // 3. Rockafellar-Wets unilateral contact release + AvbdWarmCache persistence check
    AvbdWarmCache warm;
    avbd_warm_cache_reset(&warm);
    AvbdVertex uv = {
        .pos = {0.0f, 0.0f, 0.35f},
        .vel = {0.0f, 0.0f, -2.0f},
        .inv_mass = 0.1f,
        .radius = 0.5f,
    };
    AvbdConstraint uc = {
        .kind             = AVBD_CON_CONTACT_PLANE,
        .vert_a           = 0u,
        .vert_b           = UINT32_MAX,
        .hash_key         = 0xCAFE1234u,
        .normal_or_target = {0.0f, 0.0f, 1.0f},
        .rest_or_offset   = 0.5f,
        .stiffness_min    = 1.0e3f,
        .stiffness_max    = 1.0e8f,
        .penalty_k        = 5.0e4f,
        .beta_ramp        = 4.0f,
    };
    const float grav_down[3] = {0.0f, 0.0f, -9.81f};
    float cold_res = avbd_solve_step(&uv, 1u, &uc, 1u, 1.0f / 60.0f, grav_down, 4u);
    avbd_warm_store(&warm, &uc, 1u);
    if (uc.lambda[0] >= 0.0f || uv.pos[2] < 0.495f || !isfinite(cold_res)) {
        fprintf(stderr, "FAIL: unilateral contact failed to resolve penetration (z=%.4f, lambda=%.4f)\n",
                (double)uv.pos[2], (double)uc.lambda[0]);
        return 1;
    }

    // Apply upward impulse: unilateral Rockafellar-Wets contact MUST release cleanly to 0
    uv.vel[2]    = 6.0f;
    uc.lambda[0] = 0.0f;
    avbd_warm_apply(&warm, &uc, 1u, 0.90f);
    const float grav_zero[3] = {0.0f, 0.0f, 0.0f};
    (void)avbd_solve_step(&uv, 1u, &uc, 1u, 1.0f / 60.0f, grav_zero, 4u);
    if (uv.pos[2] <= 0.55f || uc.lambda[0] != 0.0f) {
        fprintf(stderr, "FAIL: unilateral Rockafellar-Wets contact stuck on separation (z=%.4f, lambda=%.4f)\n",
                (double)uv.pos[2], (double)uc.lambda[0]);
        return 1;
    }

    // 4. 2D deformable soft-body mass-spring patch stability & GPU export
    enum { s_cols = 6, s_rows = 4, s_n = s_cols * s_rows };
    float sx[s_n], sy[s_n], svx[s_n], svy[s_n];
    for (uint32_t r = 0u; r < s_rows; ++r) {
        for (uint32_t c = 0u; c < s_cols; ++c) {
            uint32_t i = r * s_cols + c;
            sx[i]  = (float)c * 24.0f;
            sy[i]  = (float)r * 24.0f;
            svx[i] = 0.0f;
            svy[i] = 0.0f;
        }
    }
    AvbdVertex     exp_v[K_AVBD_MAX_VERTS];
    AvbdConstraint exp_c[K_AVBD_MAX_CONS];
    uint32_t       exp_vn = 0u, exp_cn = 0u;
    for (uint32_t step = 0u; step < 30u; ++step) {
        avbd_step_soft_grid_2d(sx, sy, svx, svy, s_cols, s_rows, 24.0f,
                               180.0f, 3.0f, 0.0f, 120.0f, 1.0f / 60.0f, 4u,
                               exp_v, &exp_vn, exp_c, &exp_cn);
    }
    if (exp_vn != s_n || exp_cn < 60u || !isfinite(sx[s_n - 1]) || !isfinite(sy[s_n - 1])) {
        fprintf(stderr, "FAIL: soft_grid_2d export/stability failed (vn=%u, cn=%u)\n", exp_vn, exp_cn);
        return 1;
    }

    printf("test_physics: ok (AVBD 3x3 LDL^T 1e18 + 1000:1 mass stack + %u-color CSR graph [%u verts, %u cons] + Rockafellar-Wets contact + 2D soft patch)\n",
           num_colors, (uint32_t)k_vcap, con_n);
    return 0;
}
