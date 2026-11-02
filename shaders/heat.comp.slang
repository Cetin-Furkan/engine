#include "face_grid.slang"
#include "camera.slang"

static const float k_rate       = 3.0;
static const float k_spring     = 140.0; // Topological mesh tension stiffness
static const float k_restore    = 45.0;  // Shape-memory rest-pose stiffness
static const float k_damping    = 4.5;   // Velocity damping per second
static const float k_edge_px    = 240.0;
static const float k_spot_inner = 18.0;
static const float k_spot_outer = 64.0;

struct Push {
    uint64_t src;
    uint64_t dst;
    uint64_t verts;
    uint dt_ms;
    uint seed;
    uint poke;
    float poke_x;
    float poke_y;
    float poke_z;
    float yaw_deg;
    uint pad;
};
[[vk::push_constant]] Push pc;

#include "heat_nbr.slang"

bool upper_half(uint id) {
    return (k_hot_bits[id >> 5u] & (1u << (id & 31u))) != 0u;
}

bool seeded(uint id) {
    if (id < k_fillet_verts) {
        return upper_half(id);
    }
    uint face, i, j;
    grid_decode(id, face, i, j);
    return face_point(face, i, j).y > 0.0;
}

MeshVertex rest_vert(uint id) {
    return ((MeshVertex*)pc.verts)[id];
}

float3 placed_3d(float3 in_pos, float yaw_deg) {
    const float ax = radians(28.0);
    float ay = radians(yaw_deg);
    return rotate_pose(in_pos, ax, ay) * (k_edge_px * 0.5);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group_id : SV_DispatchThreadID) {
    uint id = group_id.x;
    if (id >= k_grid_verts) {
        return;
    }
    SoftVertexState* out_field = (SoftVertexState*)pc.dst;
    if (pc.seed != 0u) {
        SoftVertexState init_s;
        init_s.disp = float3(0.0);
        init_s.temp = seeded(id) ? 1.0 : 0.0;
        init_s.vel  = float3(0.0);
        init_s.pad  = 0.0;
        out_field[id] = init_s;
        return;
    }

    SoftVertexState* in_field = (SoftVertexState*)pc.src;
    SoftVertexState cur = in_field[id];
    MeshVertex mv = rest_vert(id);

    float  sum_t    = 0.0;
    float3 sum_disp = float3(0.0);
    uint   n        = 0u;

    if (id < k_fillet_verts) {
        uint begin = k_off[id];
        uint end = k_off[id + 1u];
        n = end - begin;
        for (uint s = begin; s < end; ++s) {
            SoftVertexState nb = in_field[k_nbr[s]];
            sum_t    += nb.temp;
            sum_disp += nb.disp;
        }
        uint face, gi, gj;
        if (face_corner_at(mv.pos, face, gi, gj)) {
            uint ni = gi == 0u ? 1u : k_grid - 1u;
            uint nj = gj == 0u ? 1u : k_grid - 1u;
            SoftVertexState nb0 = in_field[grid_vertex(face, ni, gj)];
            SoftVertexState nb1 = in_field[grid_vertex(face, gi, nj)];
            sum_t    += nb0.temp + nb1.temp;
            sum_disp += nb0.disp + nb1.disp;
            n += 2u;
        }
    } else {
        uint face, i, j;
        grid_decode(id, face, i, j);
        if (i > 0u)     { SoftVertexState nb = in_field[grid_vertex(face, i - 1u, j)]; sum_t += nb.temp; sum_disp += nb.disp; n++; }
        if (i < k_grid) { SoftVertexState nb = in_field[grid_vertex(face, i + 1u, j)]; sum_t += nb.temp; sum_disp += nb.disp; n++; }
        if (j > 0u)     { SoftVertexState nb = in_field[grid_vertex(face, i, j - 1u)]; sum_t += nb.temp; sum_disp += nb.disp; n++; }
        if (j < k_grid) { SoftVertexState nb = in_field[grid_vertex(face, i, j + 1u)]; sum_t += nb.temp; sum_disp += nb.disp; n++; }
    }

    // One issued quantum is 8 ms (FRAME_SIM_STEP_MS). The host splits a
    // callback into steps of that size, so this clamp is that quantum.
    float dt = clamp(float(pc.dt_ms) * 0.001, 0.0, 0.008);
    float next_t = cur.temp;
    float3 avg_disp = cur.disp;
    if (n > 0u) {
        float inv_n = 1.0 / float(n);
        float blend = clamp(dt * k_rate, 0.0, 1.0);
        next_t   = lerp(cur.temp, sum_t * inv_n, blend);
        avg_disp = sum_disp * inv_n;
    }

    // 3D elastic membrane Laplacian + shape-memory restoring force + thermal swell
    float3 target_rest = mv.nrm * (next_t * 0.045);
    float3 accel = (avg_disp - cur.disp) * k_spring + (target_rest - cur.disp) * k_restore;
    float3 next_vel = (cur.vel + accel * dt) * exp(-k_damping * dt);
    float3 next_disp = cur.disp + next_vel * dt;

    // 3D ray-hit physical impact dent + thermal injection
    if (pc.poke != 0u) {
        float3 p3 = placed_3d(mv.pos + next_disp, pc.yaw_deg);
        float d = distance(p3, float3(pc.poke_x, pc.poke_y, pc.poke_z));
        float spot = 1.0 - smoothstep(k_spot_inner, k_spot_outer, d);
        if (spot > 0.0) {
            next_t = max(next_t, spot);
            // Inward normal impulse at the impact center, outward ring wave around it
            float ring = smoothstep(k_spot_inner * 0.5, k_spot_outer, d) * spot;
            next_vel += mv.nrm * (-3.2 * spot + 1.8 * ring);
            next_disp += mv.nrm * (-0.18 * spot);
        }
    }

    // Clamp extreme deformation to prevent self-inversion
    next_disp = clamp(next_disp, float3(-0.45), float3(0.45));

    SoftVertexState next_s;
    next_s.disp = next_disp;
    next_s.temp = next_t;
    next_s.vel  = next_vel;
    next_s.pad  = 0.0;
    out_field[id] = next_s;
}
