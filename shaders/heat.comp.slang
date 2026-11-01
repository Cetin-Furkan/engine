#include "face_grid.slang"
#include "camera.slang"

static const float k_rate = 3.0;

struct Push {
    uint64_t src;
    uint64_t dst;
    uint64_t verts;
    uint dt_ms;
    uint seed;
    uint poke;
    float poke_x;
    float poke_y;
    float yaw_deg;
};
[[vk::push_constant]] Push pc;

#include "heat_nbr.slang"

static const float k_edge_px = 240.0;
static const float k_spot_inner = 16.0;
static const float k_spot_outer = 48.0;

bool upper_half(uint id) {
    return (k_hot_bits[id >> 5u] & (1u << (id & 31u))) != 0u;
}

bool seeded(uint id) {
    if (id < k_fillet_verts) {
        return upper_half(id);
    }
    uint face;
    uint i;
    uint j;
    grid_decode(id, face, i, j);
    return face_point(face, i, j).y > 0.0;
}

float3 rest_pos(uint id) {
    float* mesh = (float*)pc.verts;
    uint o = id * 9u;
    return float3(mesh[o], mesh[o + 1u], mesh[o + 2u]);
}

float2 placed(float3 in_pos, float yaw_deg) {
    const float ax = radians(28.0);
    float ay = radians(yaw_deg);
    float3 p = rotate_pose(in_pos, ax, ay) * (k_edge_px * 0.5);
    return p.xy;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group_id : SV_DispatchThreadID) {
    uint id = group_id.x;
    if (id >= k_grid_verts) {
        return;
    }
    float* out_field = (float*)pc.dst;
    if (pc.seed != 0u) {
        out_field[id] = seeded(id) ? 1.0 : 0.0;
        return;
    }
    float* in_field = (float*)pc.src;
    float cur = in_field[id];
    float sum = 0.0;
    uint n = 0u;
    if (id < k_fillet_verts) {
        uint begin = k_off[id];
        uint end = k_off[id + 1u];
        n = end - begin;
        for (uint s = begin; s < end; ++s) {
            sum += in_field[k_nbr[s]];
        }
        uint face;
        uint gi;
        uint gj;
        if (face_corner_at(rest_pos(id), face, gi, gj)) {
            uint ni = gi == 0u ? 1u : k_grid - 1u;
            uint nj = gj == 0u ? 1u : k_grid - 1u;
            sum += in_field[grid_vertex(face, ni, gj)];
            sum += in_field[grid_vertex(face, gi, nj)];
            n += 2u;
        }
    } else {
        uint face;
        uint i;
        uint j;
        grid_decode(id, face, i, j);
        if (i > 0u) {
            sum += in_field[grid_vertex(face, i - 1u, j)];
            n += 1u;
        }
        if (i < k_grid) {
            sum += in_field[grid_vertex(face, i + 1u, j)];
            n += 1u;
        }
        if (j > 0u) {
            sum += in_field[grid_vertex(face, i, j - 1u)];
            n += 1u;
        }
        if (j < k_grid) {
            sum += in_field[grid_vertex(face, i, j + 1u)];
            n += 1u;
        }
    }
    float next = cur;
    if (n > 0u) {
        float avg = sum / float(n);
        float blend = clamp(float(pc.dt_ms) * (k_rate / 1000.0), 0.0, 1.0);
        next = (1.0 - blend) * cur + blend * avg;
    }
    if (pc.poke != 0u) {
        float2 p = placed(rest_pos(id), pc.yaw_deg);
        float d = distance(p, float2(pc.poke_x, pc.poke_y));
        float spot = 1.0 - smoothstep(k_spot_inner, k_spot_outer, d);
        next = max(next, spot);
    }
    out_field[id] = next;
}
