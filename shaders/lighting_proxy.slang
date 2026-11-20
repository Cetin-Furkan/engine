// Analytical OBB / Ellipsoid Proxy Ray Intersection & Studio Sky Environment Radiance.

float3 lighting_proxy_albedo(LightingProxyGpu p) {
    return float3(
        f16tof32(p.alb_rg & 0xFFFFu),
        f16tof32((p.alb_rg >> 16u) & 0xFFFFu),
        f16tof32(p.alb_b_flags & 0xFFFFu)
    );
}

float3 lighting_proxy_emissive(LightingProxyGpu p) {
    return float3(
        f16tof32(p.emi_rg & 0xFFFFu),
        f16tof32((p.emi_rg >> 16u) & 0xFFFFu),
        f16tof32(p.emi_b_rad & 0xFFFFu)
    );
}

float lighting_proxy_bound_radius(LightingProxyGpu p) {
    float r = f16tof32((p.emi_b_rad >> 16u) & 0xFFFFu);
    return r > 1.0e-4 ? r : (length(p.extents) * 1.02);
}

// Fast exact analytical OBB / Ellipsoid proxy ray intersection (zero SDF march loops)
bool lighting_ray_proxy(
    float3           ro,
    float3           rd,
    float            t_min,
    float            t_max,
    LightingProxyGpu proxy,
    out float        out_t,
    out float3       out_world_nrm)
{
    out_t         = t_max;
    out_world_nrm = float3(0.0, 0.0, 1.0);

    float3 rel   = ro - proxy.center;
    float  r_sph = lighting_proxy_bound_radius(proxy);
    float  b_sph = dot(rel, rd);
    float  c_sph = dot(rel, rel) - r_sph * r_sph;
    if (c_sph > 0.0 && b_sph > 0.0) {
        return false;
    }
    if (b_sph * b_sph - c_sph < 0.0) {
        return false;
    }
    if (-b_sph > t_max + r_sph || -b_sph < t_min - r_sph) {
        return false;
    }

    float sx = f16tof32(proxy.trig_pitch & 0xFFFFu);
    float cx = f16tof32((proxy.trig_pitch >> 16u) & 0xFFFFu);
    float sy = f16tof32(proxy.trig_yaw   & 0xFFFFu);
    float cy = f16tof32((proxy.trig_yaw   >> 16u) & 0xFFFFu);

    float3 lo = lighting_pose_to_local_fast(rel, sx, cx, sy, cy);
    float3 ld = lighting_pose_to_local_fast(rd,  sx, cx, sy, cy);

    if (proxy.kind == k_lighting_proxy_obb) {
        float t0 = t_min;
        float t1 = t_max;
        int   hit_axis = -1;
        float hit_sign = 1.0;

        [unroll]
        for (int a = 0; a < 3; ++a) {
            float ext = max(proxy.extents[a], 1.0e-4);
            if (abs(ld[a]) < 1.0e-7) {
                if (lo[a] < -ext || lo[a] > ext) {
                    return false;
                }
            } else {
                float inv_d = 1.0 / ld[a];
                float s0    = (-ext - lo[a]) * inv_d;
                float s1    = ( ext - lo[a]) * inv_d;
                float sgn   = -1.0;
                if (s0 > s1) {
                    float tmp = s0; s0 = s1; s1 = tmp;
                    sgn = 1.0;
                }
                if (s0 > t0) {
                    t0       = s0;
                    hit_axis = a;
                    hit_sign = sgn;
                }
                t1 = min(t1, s1);
                if (t0 > t1) {
                    return false;
                }
            }
        }
        if (t0 < t_min || t0 > t_max) {
            return false;
        }
        float3 ln;
        if (hit_axis == 0)      ln = float3(hit_sign, 0.0, 0.0);
        else if (hit_axis == 1) ln = float3(0.0, hit_sign, 0.0);
        else if (hit_axis == 2) ln = float3(0.0, 0.0, hit_sign);
        else {
            float3 hp  = lo + ld * t0;
            float3 rem = abs(proxy.extents - abs(hp));
            if (rem.x <= rem.y && rem.x <= rem.z)      ln = float3(hp.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
            else if (rem.y <= rem.z)                   ln = float3(0.0, hp.y >= 0.0 ? 1.0 : -1.0, 0.0);
            else                                       ln = float3(0.0, 0.0, hp.z >= 0.0 ? 1.0 : -1.0);
        }
        out_t         = t0;
        out_world_nrm = normalize(lighting_local_to_pose_fast(ln, sx, cx, sy, cy));
        return true;
    }

    float3 inv_r = 1.0 / max(proxy.extents, float3(1.0e-4));
    float3 so    = lo * inv_r;
    float3 sd    = ld * inv_r;
    float  qa    = dot(sd, sd);
    if (qa < 1.0e-10) {
        return false;
    }
    float qb   = dot(so, sd);
    float qc   = dot(so, so) - 1.0;
    float disc = qb * qb - qa * qc;
    if (disc < 0.0) {
        return false;
    }
    float root  = sqrt(disc);
    float t_hit = (-qb - root) / qa;
    if (t_hit < t_min || t_hit > t_max) {
        t_hit = (-qb + root) / qa;
        if (t_hit < t_min || t_hit > t_max) {
            return false;
        }
    }
    float3 hp     = lo + ld * t_hit;
    float3 ln     = normalize(hp * inv_r * inv_r);
    out_t         = t_hit;
    out_world_nrm = normalize(lighting_local_to_pose_fast(ln, sx, cx, sy, cy));
    return true;
}

// Studio atmospheric / sky dome radiance (linear HDR) for backdrop, reflections, and HRC sky
float3 lighting_eval_sky_studio(float3 world_dir, float3 sun_dir, float3 sky_top,
                                float3 sky_horizon, float3 sky_ground, float roughness) {
    float z_up = world_dir.z;
    float3 dome;
    if (z_up >= 0.0) {
        float t = pow(clamp(z_up, 0.0, 1.0), lerp(0.58, 1.0, roughness));
        dome = lerp(sky_horizon, sky_top, t);
    } else {
        float t = pow(clamp(-z_up, 0.0, 1.0), lerp(0.55, 1.0, roughness));
        dome = lerp(sky_horizon * 0.68, sky_ground, t);
    }
    float sun_dot = max(dot(world_dir, sun_dir), 0.0);
    float sun_exp = lerp(110.0, 6.0, roughness);
    float sun_amp = lerp(1.45, 0.28, roughness);
    float halo    = pow(sun_dot, 7.0) * 0.22;
    return dome + float3(1.00, 0.90, 0.72) * (pow(sun_dot, sun_exp) * sun_amp + halo);
}

float3 lighting_eval_sky_env_hdr(LightingFrameHeader hdr, float3 world_dir) {
    if (hdr.terrain_dir_bda != 0ull) {
        float3 sun_dir = lighting_terrain_sun_dir();
        float  up      = clamp(world_dir.z * 0.5 + 0.5, 0.0, 1.0);
        float  sun     = pow(max(dot(world_dir, sun_dir), 0.0), 8.0);
        float3 sky     = lerp(float3(0.18, 0.16, 0.14), float3(0.36, 0.46, 0.62), up);
        return sky + float3(0.55, 0.48, 0.36) * sun;
    }
    return lighting_eval_sky_studio(
        world_dir,
        lighting_world_sun_dir(),
        float3(0.34, 0.52, 0.84),
        float3(0.62, 0.70, 0.80),
        float3(0.16, 0.19, 0.25),
        0.55);
}
