struct PlotPoint {
    float x;
    float y;
    float z;
    float brk;
};

struct BakePush {
    uint64_t addr;
    uint mode;
    uint count;
};
[[vk::push_constant]] BakePush pc;

void write_point(PlotPoint* buf, uint i, float3 p, float brk) {
    buf[i].x = p.x;
    buf[i].y = p.y;
    buf[i].z = p.z;
    buf[i].brk = brk;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    uint total = 6u + pc.count;
    if (i >= total || pc.count < 2u) {
        return;
    }
    PlotPoint* buf = (PlotPoint*)pc.addr;
    const float2 c = float2(-300.0, -15.0);
    const float2 h = float2(100.0, 55.0);
    if (i == 0u) {
        write_point(buf, i, float3(c.x - h.x, c.y, 0.0), 1.0);
        return;
    }
    if (i == 1u) {
        write_point(buf, i, float3(c.x + h.x, c.y, 0.0), 0.0);
        return;
    }
    if (i == 2u) {
        write_point(buf, i, float3(c.x, c.y - h.y, 0.0), 1.0);
        return;
    }
    if (i == 3u) {
        write_point(buf, i, float3(c.x, c.y + h.y, 0.0), 0.0);
        return;
    }
    if (i == 4u) {
        write_point(buf, i, float3(c, -40.0), 1.0);
        return;
    }
    if (i == 5u) {
        write_point(buf, i, float3(c, 40.0), 0.0);
        return;
    }
    uint s = i - 6u;
    float t = float(s) / float(pc.count - 1u);
    float u = t * 2.0 - 1.0;
    float x = 0.0;
    float y = 0.0;
    if (pc.mode == 2u) {
        x = u * 12.0;
        y = abs(x) < 1.0e-4 ? 1.0 : sin(x) / x;
    } else if (pc.mode == 3u) {
        x = u * 3.0;
        y = exp(-0.5 * x * x);
    } else {
        x = u * 6.28318530718;
        y = sin(x);
    }
    write_point(buf, i, float3(c.x + u * h.x, c.y + y * h.y, 0.0), s == 0u ? 1.0 : 0.0);
}
