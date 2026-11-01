struct Push {
    uint64_t src;
    uint64_t dst;
    uint dt_ms;
    uint seed;
    uint n;
    uint pad;
};
[[vk::push_constant]] Push pc;

static const uint BX = 8u;
static const uint BY = 8u;
static const uint SX = BX + 2u;
static const uint SY = BY + 2u;
static const uint SZ = 3u;

groupshared half tile[SX * SY * SZ];

uint tile_at(uint lx, uint ly, uint lz) {
    return lx + ly * SX + lz * SX * SY;
}

uint cell_at(uint x, uint y, uint z, uint n) {
    return x + y * n + z * n * n;
}

half ball(int x, int y, int z, uint n) {
    float mid = float(n) * 0.5 - 0.5;
    float dx = float(x) - mid;
    float dy = float(y) - mid;
    float dz = float(z) - mid;
    float radius = 10.0 * float(n) / 128.0;
    return (dx * dx + dy * dy + dz * dz) < radius * radius ? (half)1.0 : (half)0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 group_id : SV_GroupID, uint3 thread_id : SV_GroupThreadID, uint lid : SV_GroupIndex) {
    uint n = pc.n;
    uint wx = group_id.x * BX;
    uint wy = group_id.y * BY;
    uint wz = group_id.z;
    uint tile_n = SX * SY * SZ;
    half* src_v = (half*)pc.src;
    half* dst_v = (half*)pc.dst;

    for (uint i = lid; i < tile_n; i += BX * BY) {
        uint lx = i % SX;
        uint ly = (i / SX) % SY;
        uint lz = i / (SX * SY);
        int x = int(wx) + int(lx) - 1;
        int y = int(wy) + int(ly) - 1;
        int z = int(wz) + int(lz) - 1;
        half stored = (half)0.0;
        if (n > 0u && x >= 0 && y >= 0 && z >= 0 &&
            uint(x) < n && uint(y) < n && uint(z) < n) {
            if (pc.seed != 0u) {
                stored = ball(x, y, z, n);
            } else if (pc.src != 0ul) {
                stored = src_v[cell_at(uint(x), uint(y), uint(z), n)];
            }
        }
        tile[i] = stored;
    }
    GroupMemoryBarrierWithGroupSync();

    uint x = wx + thread_id.x;
    uint y = wy + thread_id.y;
    uint z = wz;
    if (n == 0u || pc.dst == 0ul || x >= n || y >= n || z >= n) {
        return;
    }
    uint center = tile_at(thread_id.x + 1u, thread_id.y + 1u, 1u);
    if (pc.seed != 0u) {
        dst_v[cell_at(x, y, z, n)] = tile[center];
        return;
    }
    float cur = float(tile[center]);
    float sum = 0.0;
    uint count = 0u;
    static const int ox[6] = { -1, 1, 0, 0, 0, 0 };
    static const int oy[6] = { 0, 0, -1, 1, 0, 0 };
    static const int oz[6] = { 0, 0, 0, 0, -1, 1 };
    for (uint s = 0u; s < 6u; ++s) {
        int nx = int(x) + ox[s];
        int ny = int(y) + oy[s];
        int nz = int(z) + oz[s];
        if (nx < 0 || ny < 0 || nz < 0 || uint(nx) >= n || uint(ny) >= n || uint(nz) >= n) {
            continue;
        }
        int lx = int(thread_id.x) + 1 + ox[s];
        int ly = int(thread_id.y) + 1 + oy[s];
        int lz = 1 + oz[s];
        sum += float(tile[tile_at(uint(lx), uint(ly), uint(lz))]);
        count += 1u;
    }
    float avg = count > 0u ? sum / float(count) : cur;
    float alpha = min(float(pc.dt_ms) * 0.02, 1.0);
    float next = clamp(lerp(cur, avg, alpha), 0.0, 1.0);
    dst_v[cell_at(x, y, z, n)] = (half)next;
}
