#include "backend/internal.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

// ============================================================================
// Photogrammetric Camera Model, Sensor Presets & Multi-View Ray Triangulation
// ============================================================================

static constexpr float k_deg_to_rad = 0.01745329251994329577f;
static constexpr float k_rad_to_deg = 57.2957795130823208768f;

static const TerrainSensorPreset k_sensor_presets[] = {
    { "iPhone 16 Pro Main",      9.80f,  7.35f,  6.765f, 24.0f, -0.028f,  0.006f },
    { "iPhone 15 Pro Main",      9.80f,  7.35f,  6.765f, 24.0f, -0.028f,  0.006f },
    { "iPhone 15 Pro UltraWide", 4.80f,  3.60f,  2.220f, 13.0f, -0.065f,  0.018f },
    { "Samsung S24 Ultra Main",  9.60f,  7.20f,  6.300f, 23.0f, -0.026f,  0.005f },
    { "Xiaomi 14 Ultra 1-inch", 13.20f,  8.80f,  8.720f, 23.0f, -0.022f,  0.004f },
    { "Google Pixel 9 Pro Main", 9.80f,  7.35f,  6.900f, 24.0f, -0.025f,  0.005f },
    { "DJI Mavic 3 Enterprise", 17.30f, 13.00f, 12.290f, 24.0f, -0.014f,  0.002f },
    { "DJI Mini 4 Pro",          9.60f,  7.20f,  6.720f, 24.0f, -0.024f,  0.004f },
    { "DJI Air 3 Wide",          9.60f,  7.20f,  6.720f, 24.0f, -0.024f,  0.004f },
    { "DJI Phantom 4 RTK",      13.20f,  8.80f,  8.800f, 24.0f, -0.012f,  0.002f },
    { "Canon PowerShot S110",    7.44f,  5.58f,  5.200f, 24.0f, -0.018f,  0.003f },
    { "Sony DSC-WX220",          6.17f,  4.55f,  4.450f, 25.0f, -0.020f,  0.004f },
    { "Sony A7R V FullFrame",   35.70f, 23.80f, 35.000f, 35.0f, -0.008f,  0.001f },
    { "Custom",                 36.00f, 24.00f, 24.000f, 24.0f,  0.000f,  0.000f },
};

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p) {
        size_t i = 0u;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

bool backend_survey_find_sensor_preset(const char          *model_name,
                                       TerrainSensorPreset *out_preset) {
    if (out_preset == nullptr) {
        return false;
    }
    constexpr size_t k_count = sizeof(k_sensor_presets) / sizeof(k_sensor_presets[0]);
    if (model_name == nullptr || model_name[0] == '\0') {
        *out_preset = k_sensor_presets[k_count - 1u];
        return false;
    }
    // 1. Check if model_name contains a full preset name
    for (size_t i = 0u; i < k_count; ++i) {
        if (str_contains_ci(model_name, k_sensor_presets[i].name)) {
            *out_preset = k_sensor_presets[i];
            return true;
        }
    }
    // 2. Check specific lens modifiers before broad family aliases
    if (str_contains_ci(model_name, "ultrawide") || str_contains_ci(model_name, "ultra wide") ||
        str_contains_ci(model_name, "0.5x")) {
        *out_preset = k_sensor_presets[2];
        return true;
    }
    // 3. Check substring match against preset names only for non-trivial queries (>= 4 chars)
    if (strlen(model_name) >= 4u) {
        for (size_t i = 0u; i < k_count; ++i) {
            if (str_contains_ci(k_sensor_presets[i].name, model_name)) {
                *out_preset = k_sensor_presets[i];
                return true;
            }
        }
    }
    // 4. Common shorthand aliases
    if (str_contains_ci(model_name, "iphone 16") || str_contains_ci(model_name, "iphone16")) {
        *out_preset = k_sensor_presets[0];
        return true;
    }
    if (str_contains_ci(model_name, "iphone")) {
        *out_preset = k_sensor_presets[1];
        return true;
    }
    if (str_contains_ci(model_name, "s24") || str_contains_ci(model_name, "samsung") ||
        str_contains_ci(model_name, "galaxy")) {
        *out_preset = k_sensor_presets[3];
        return true;
    }
    if (str_contains_ci(model_name, "xiaomi") || str_contains_ci(model_name, "1-inch")) {
        *out_preset = k_sensor_presets[4];
        return true;
    }
    if (str_contains_ci(model_name, "pixel")) {
        *out_preset = k_sensor_presets[5];
        return true;
    }
    if (str_contains_ci(model_name, "mavic")) {
        *out_preset = k_sensor_presets[6];
        return true;
    }
    if (str_contains_ci(model_name, "mini")) {
        *out_preset = k_sensor_presets[7];
        return true;
    }
    if (str_contains_ci(model_name, "air")) {
        *out_preset = k_sensor_presets[8];
        return true;
    }
    if (str_contains_ci(model_name, "phantom") || str_contains_ci(model_name, "rtk")) {
        *out_preset = k_sensor_presets[9];
        return true;
    }
    if (str_contains_ci(model_name, "s110") || str_contains_ci(model_name, "canon") ||
        str_contains_ci(model_name, "ebee") || str_contains_ci(model_name, "sensefly")) {
        *out_preset = k_sensor_presets[10];
        return true;
    }
    if (str_contains_ci(model_name, "wx220")) {
        *out_preset = k_sensor_presets[11];
        return true;
    }
    if (str_contains_ci(model_name, "sony") || str_contains_ci(model_name, "fullframe")) {
        *out_preset = k_sensor_presets[12];
        return true;
    }
    *out_preset = k_sensor_presets[k_count - 1u];
    return false;
}

void backend_survey_wgs84_to_world(const GpuMathPageDirectory *dir,
                                   double                      lat_deg,
                                   double                      lon_deg,
                                   float                      *out_wx,
                                   float                      *out_wy) {
    double lat_min = (dir != nullptr && dir->lat_max_deg > dir->lat_min_deg)
        ? dir->lat_min_deg : k_besparmak_lat_min;
    double lat_max = (dir != nullptr && dir->lat_max_deg > dir->lat_min_deg)
        ? dir->lat_max_deg : k_besparmak_lat_max;
    double lon_min = (dir != nullptr && dir->lon_max_deg > dir->lon_min_deg)
        ? dir->lon_min_deg : k_besparmak_lon_min;
    double lon_max = (dir != nullptr && dir->lon_max_deg > dir->lon_min_deg)
        ? dir->lon_max_deg : k_besparmak_lon_max;
    float half = (dir != nullptr && dir->world_half_extent > 1.0f)
        ? dir->world_half_extent : k_terrain_world_half;

    double u = (lon_deg - lon_min) / (lon_max - lon_min);
    double v = (lat_deg - lat_min) / (lat_max - lat_min);
    if (out_wx != nullptr) {
        *out_wx = (float)(-half + u * (2.0 * (double)half));
    }
    if (out_wy != nullptr) {
        *out_wy = (float)(-half + v * (2.0 * (double)half));
    }
}

void backend_survey_world_to_wgs84(const GpuMathPageDirectory *dir,
                                   float                       wx,
                                   float                       wy,
                                   double                     *out_lat_deg,
                                   double                     *out_lon_deg) {
    double lat_min = (dir != nullptr && dir->lat_max_deg > dir->lat_min_deg)
        ? dir->lat_min_deg : k_besparmak_lat_min;
    double lat_max = (dir != nullptr && dir->lat_max_deg > dir->lat_min_deg)
        ? dir->lat_max_deg : k_besparmak_lat_max;
    double lon_min = (dir != nullptr && dir->lon_max_deg > dir->lon_min_deg)
        ? dir->lon_min_deg : k_besparmak_lon_min;
    double lon_max = (dir != nullptr && dir->lon_max_deg > dir->lon_min_deg)
        ? dir->lon_max_deg : k_besparmak_lon_max;
    float half = (dir != nullptr && dir->world_half_extent > 1.0f)
        ? dir->world_half_extent : k_terrain_world_half;

    double u = ((double)wx + (double)half) / (2.0 * (double)half);
    double v = ((double)wy + (double)half) / (2.0 * (double)half);
    if (out_lon_deg != nullptr) {
        *out_lon_deg = lon_min + u * (lon_max - lon_min);
    }
    if (out_lat_deg != nullptr) {
        *out_lat_deg = lat_min + v * (lat_max - lat_min);
    }
}

bool backend_survey_utm_to_wgs84(uint32_t zone,
                                 bool     northern_hemisphere,
                                 double   easting_m,
                                 double   northing_m,
                                 double  *out_lat_deg,
                                 double  *out_lon_deg) {
    if (zone < 1u || zone > 60u || !isfinite(easting_m) || !isfinite(northing_m)) {
        return false;
    }
    constexpr double a  = 6378137.0;
    constexpr double f  = 1.0 / 298.257223563;
    constexpr double e2 = f * (2.0 - f);
    constexpr double ep2 = e2 / (1.0 - e2);
    constexpr double k0 = 0.9996;

    double x = easting_m - 500000.0;
    double y = northern_hemisphere ? northing_m : (northing_m - 10000000.0);
    double M = y / k0;
    double mu = M / (a * (1.0 - e2 / 4.0 - 3.0 * e2 * e2 / 64.0 - 5.0 * e2 * e2 * e2 / 256.0));
    double e1 = (1.0 - sqrt(1.0 - e2)) / (1.0 + sqrt(1.0 - e2));
    double phi1 = mu +
                  (1.5 * e1 - 27.0 * e1 * e1 * e1 / 32.0) * sin(2.0 * mu) +
                  (21.0 * e1 * e1 / 16.0 - 55.0 * e1 * e1 * e1 * e1 / 32.0) * sin(4.0 * mu) +
                  (151.0 * e1 * e1 * e1 / 96.0) * sin(6.0 * mu);
    double s_phi1 = sin(phi1);
    double c_phi1 = cos(phi1);
    double t_phi1 = tan(phi1);
    double N1 = a / sqrt(1.0 - e2 * s_phi1 * s_phi1);
    double T1 = t_phi1 * t_phi1;
    double C1 = ep2 * c_phi1 * c_phi1;
    double R1 = a * (1.0 - e2) / pow(1.0 - e2 * s_phi1 * s_phi1, 1.5);
    double D  = x / (N1 * k0);

    double lat_rad = phi1 - (N1 * t_phi1 / R1) *
                            (D * D / 2.0 -
                             (5.0 + 3.0 * T1 + 10.0 * C1 - 4.0 * C1 * C1 - 9.0 * ep2) *
                             (D * D * D * D) / 24.0);
    double lon0_deg = (double)((int32_t)zone * 6 - 183);
    double lon_deg  = lon0_deg + ((D - (1.0 + 2.0 * T1 + C1) * (D * D * D) / 6.0) / c_phi1) *
                                 (180.0 / 3.14159265358979323846);
    double lat_deg  = lat_rad * (180.0 / 3.14159265358979323846);
    if (out_lat_deg != nullptr) {
        *out_lat_deg = lat_deg;
    }
    if (out_lon_deg != nullptr) {
        *out_lon_deg = lon_deg;
    }
    return true;
}

bool backend_survey_init_intrinsics(TerrainCameraIntrinsics *intr) {
    if (intr == nullptr || intr->width_px == 0u || intr->height_px == 0u) {
        return false;
    }
    float aspect = (float)intr->width_px / (float)intr->height_px;
    if (!isfinite(intr->sensor_width_mm) || intr->sensor_width_mm <= 0.0f) {
        intr->sensor_width_mm = 36.0f;
    }
    if (!isfinite(intr->sensor_height_mm) || intr->sensor_height_mm <= 0.0f) {
        intr->sensor_height_mm = intr->sensor_width_mm / fmaxf(aspect, 0.25f);
    }
    if (!isfinite(intr->focal_mm) || intr->focal_mm <= 0.0f) {
        if (isfinite(intr->focal_35mm_mm) && intr->focal_35mm_mm > 0.0f) {
            intr->focal_mm = intr->focal_35mm_mm * (intr->sensor_width_mm / 36.0f);
        } else {
            intr->focal_mm      = 24.0f * (intr->sensor_width_mm / 36.0f);
            intr->focal_35mm_mm = 24.0f;
        }
    }
    intr->focal_35mm_mm = intr->focal_mm * (36.0f / intr->sensor_width_mm);

    intr->fx_px = (intr->focal_mm / intr->sensor_width_mm) * (float)intr->width_px;
    intr->fy_px = (intr->focal_mm / intr->sensor_height_mm) * (float)intr->height_px;
    if (!isfinite(intr->fx_px) || !isfinite(intr->fy_px) ||
        intr->fx_px < 1.0f || intr->fy_px < 1.0f) {
        return false;
    }
    if (!isfinite(intr->cx_px) || intr->cx_px <= 0.0f || intr->cx_px >= (float)intr->width_px) {
        intr->cx_px = 0.5f * (float)(intr->width_px - 1u);
    }
    if (!isfinite(intr->cy_px) || intr->cy_px <= 0.0f || intr->cy_px >= (float)intr->height_px) {
        intr->cy_px = 0.5f * (float)(intr->height_px - 1u);
    }
    if (!isfinite(intr->k1)) intr->k1 = 0.0f;
    if (!isfinite(intr->k2)) intr->k2 = 0.0f;
    if (!isfinite(intr->k3)) intr->k3 = 0.0f;
    if (!isfinite(intr->p1)) intr->p1 = 0.0f;
    if (!isfinite(intr->p2)) intr->p2 = 0.0f;

    intr->hfov_deg = 2.0f * atanf(0.5f * (float)intr->width_px / intr->fx_px) * k_rad_to_deg;
    intr->vfov_deg = 2.0f * atanf(0.5f * (float)intr->height_px / intr->fy_px) * k_rad_to_deg;
    return true;
}

void backend_survey_init_pose(const GpuMathPageDirectory *dir,
                              TerrainCameraPose          *pose) {
    if (pose == nullptr) {
        return;
    }
    if (!isfinite(pose->latitude_deg))   pose->latitude_deg   = 0.0;
    if (!isfinite(pose->longitude_deg))  pose->longitude_deg  = 0.0;
    if (!isfinite(pose->world_x))        pose->world_x        = 0.0f;
    if (!isfinite(pose->world_y))        pose->world_y        = 0.0f;
    if (!isfinite(pose->altitude_msl_m)) pose->altitude_msl_m = 0.0f;
    if (!isfinite(pose->height_agl_m))   pose->height_agl_m   = 0.0f;
    if (!isfinite(pose->yaw_deg))        pose->yaw_deg        = 0.0f;
    if (!isfinite(pose->pitch_deg))      pose->pitch_deg      = 0.0f;
    if (!isfinite(pose->roll_deg))       pose->roll_deg       = 0.0f;

    if (pose->use_wgs84) {
        backend_survey_wgs84_to_world(dir, pose->latitude_deg, pose->longitude_deg,
                                      &pose->world_x, &pose->world_y);
    } else {
        backend_survey_world_to_wgs84(dir, pose->world_x, pose->world_y,
                                      &pose->latitude_deg, &pose->longitude_deg);
    }

    if ((pose->altitude_msl_m <= 0.0f || pose->use_agl) && pose->height_agl_m > 0.0f) {
        pose->use_agl = true;
        float ground_elev_m = backend_eval_besparmak_elevation_m(pose->latitude_deg,
                                                                 pose->longitude_deg);
        pose->altitude_msl_m = ground_elev_m + pose->height_agl_m;
    }

    // Yaw: clockwise from True North (+Y). 0° = +Y, 90° = +X (East), 180° = -Y (South), 270° = -X (West)
    // Pitch: tilt from horizontal. 0° = horizon, -90° = nadir straight down (-Z)
    // Roll: bank around forward optical axis. 0° = level horizon, +clockwise
    float psi   = pose->yaw_deg * k_deg_to_rad;
    float theta = pose->pitch_deg * k_deg_to_rad;
    float phi   = pose->roll_deg * k_deg_to_rad;

    float sin_psi   = sinf(psi);
    float cos_psi   = cosf(psi);
    float sin_theta = sinf(theta);
    float cos_theta = cosf(theta);
    float sin_phi   = sinf(phi);
    float cos_phi   = cosf(phi);

    // Forward optical axis in ENU world coordinates
    float fx = cos_theta * sin_psi;
    float fy = cos_theta * cos_psi;
    float fz = sin_theta;

    // Unrolled right (r1) and up (u1) basis vectors
    float r1x =  cos_psi;
    float r1y = -sin_psi;
    float r1z =  0.0f;

    float u1x = -sin_theta * sin_psi;
    float u1y = -sin_theta * cos_psi;
    float u1z =  cos_theta;

    // Apply roll phi around forward axis
    pose->right[0]   = cos_phi * r1x - sin_phi * u1x;
    pose->right[1]   = cos_phi * r1y - sin_phi * u1y;
    pose->right[2]   = cos_phi * r1z - sin_phi * u1z;

    pose->up[0]      = sin_phi * r1x + cos_phi * u1x;
    pose->up[1]      = sin_phi * r1y + cos_phi * u1y;
    pose->up[2]      = sin_phi * r1z + cos_phi * u1z;

    pose->forward[0] = fx;
    pose->forward[1] = fy;
    pose->forward[2] = fz;
}

bool backend_survey_project_world_to_pixel(const TerrainCameraIntrinsics *intr,
                                           const TerrainCameraPose       *pose,
                                           float                          wx,
                                           float                          wy,
                                           float                          elev_m,
                                           float                         *out_u_px,
                                           float                         *out_v_px,
                                           float                         *out_depth_m) {
    if (intr == nullptr || pose == nullptr || intr->fx_px <= 0.0f || intr->fy_px <= 0.0f ||
        !isfinite(wx) || !isfinite(wy) || !isfinite(elev_m)) {
        return false;
    }
    float vx = wx - pose->world_x;
    float vy = wy - pose->world_y;
    float vz = elev_m - pose->altitude_msl_m;

    float xc = vx * pose->right[0]   + vy * pose->right[1]   + vz * pose->right[2];
    float yc = vx * pose->up[0]      + vy * pose->up[1]      + vz * pose->up[2];
    float zc = vx * pose->forward[0] + vy * pose->forward[1] + vz * pose->forward[2];

    if (out_depth_m != nullptr) {
        *out_depth_m = zc;
    }
    if (!isfinite(zc) || zc <= 0.1f) {
        return false;
    }

    float xn = xc / zc;
    float yn = yc / zc;
    float r2 = xn * xn + yn * yn;
    // Prevent extreme off-axis points from folding back under radial polynomial distortion
    if (!isfinite(r2) || r2 > 12.0f) {
        return false;
    }
    float r4 = r2 * r2;
    float r6 = r4 * r2;

    float radial = 1.0f + intr->k1 * r2 + intr->k2 * r4 + intr->k3 * r6;
    if (!isfinite(radial) || radial <= 0.15f) {
        return false;
    }
    float xd = xn * radial + 2.0f * intr->p1 * xn * yn + intr->p2 * (r2 + 2.0f * xn * xn);
    float yd = yn * radial + intr->p1 * (r2 + 2.0f * yn * yn) + 2.0f * intr->p2 * xn * yn;

    float u_px = intr->fx_px * xd + intr->cx_px;
    float v_px = intr->cy_px - intr->fy_px * yd;
    if (!isfinite(u_px) || !isfinite(v_px)) {
        return false;
    }

    if (out_u_px != nullptr) {
        *out_u_px = u_px;
    }
    if (out_v_px != nullptr) {
        *out_v_px = v_px;
    }
    return (u_px >= 0.0f && u_px <= (float)(intr->width_px - 1u) &&
            v_px >= 0.0f && v_px <= (float)(intr->height_px - 1u));
}

bool backend_survey_pixel_to_world_ray(const TerrainCameraIntrinsics *intr,
                                       const TerrainCameraPose       *pose,
                                       float                          u_px,
                                       float                          v_px,
                                       float                          out_origin[3],
                                       float                          out_dir[3]) {
    if (intr == nullptr || pose == nullptr || out_origin == nullptr || out_dir == nullptr ||
        intr->fx_px <= 0.0f || intr->fy_px <= 0.0f || !isfinite(u_px) || !isfinite(v_px)) {
        return false;
    }
    float xd = (u_px - intr->cx_px) / intr->fx_px;
    float yd = (intr->cy_px - v_px) / intr->fy_px;
    if (!isfinite(xd) || !isfinite(yd)) {
        return false;
    }

    // Iterative Brown-Conrady undistortion
    float xn = xd;
    float yn = yd;
    for (int iter = 0; iter < 10; ++iter) {
        float r2 = xn * xn + yn * yn;
        if (!isfinite(r2) || r2 > 12.0f) {
            break;
        }
        float r4 = r2 * r2;
        float r6 = r4 * r2;
        float radial = 1.0f + intr->k1 * r2 + intr->k2 * r4 + intr->k3 * r6;
        if (!isfinite(radial) || radial <= 0.15f) {
            break;
        }
        float dx = 2.0f * intr->p1 * xn * yn + intr->p2 * (r2 + 2.0f * xn * xn);
        float dy = intr->p1 * (r2 + 2.0f * yn * yn) + 2.0f * intr->p2 * xn * yn;
        xn = (xd - dx) / radial;
        yn = (yd - dy) / radial;
    }

    float rx = pose->forward[0] + xn * pose->right[0] + yn * pose->up[0];
    float ry = pose->forward[1] + xn * pose->right[1] + yn * pose->up[1];
    float rz = pose->forward[2] + xn * pose->right[2] + yn * pose->up[2];
    float len = sqrtf(rx * rx + ry * ry + rz * rz);
    if (!isfinite(len) || len < 1.0e-6f) {
        return false;
    }

    out_origin[0] = pose->world_x;
    out_origin[1] = pose->world_y;
    out_origin[2] = pose->altitude_msl_m;

    out_dir[0] = rx / len;
    out_dir[1] = ry / len;
    out_dir[2] = rz / len;
    return true;
}

bool backend_survey_triangulate_rays(uint32_t     ray_count,
                                     const float (*origins)[3],
                                     const float (*dirs)[3],
                                     float        out_point[3],
                                     float       *out_rmse_m,
                                     float       *out_max_parallax_deg) {
    if (ray_count < 2u || origins == nullptr || dirs == nullptr || out_point == nullptr) {
        return false;
    }

    double a[3][4] = {{0.0}};
    float  max_parallax_deg   = 0.0f;
    float  max_line_angle_deg = 0.0f;
    float  max_baseline_m     = 0.0f;

    for (uint32_t i = 0u; i < ray_count; ++i) {
        if (!isfinite(origins[i][0]) || !isfinite(origins[i][1]) || !isfinite(origins[i][2]) ||
            !isfinite(dirs[i][0])    || !isfinite(dirs[i][1])    || !isfinite(dirs[i][2])) {
            return false;
        }
        double dx = dirs[i][0];
        double dy = dirs[i][1];
        double dz = dirs[i][2];
        double dn = sqrt(dx * dx + dy * dy + dz * dz);
        if (dn < 1.0e-7) {
            return false;
        }
        dx /= dn;
        dy /= dn;
        dz /= dn;

        double ox = origins[i][0];
        double oy = origins[i][1];
        double oz = origins[i][2];

        for (uint32_t j = i + 1u; j < ray_count; ++j) {
            double jx = dirs[j][0];
            double jy = dirs[j][1];
            double jz = dirs[j][2];
            double jn = sqrt(jx * jx + jy * jy + jz * jz);
            if (jn > 1.0e-7) {
                double dot_ij = (dx * jx + dy * jy + dz * jz) / jn;
                if (dot_ij >  1.0) dot_ij =  1.0;
                if (dot_ij < -1.0) dot_ij = -1.0;
                float ray_ang_deg  = (float)(acos(dot_ij) * (double)k_rad_to_deg);
                float line_ang_deg = (float)(acos(fmin(fabs(dot_ij), 1.0)) * (double)k_rad_to_deg);
                if (ray_ang_deg > max_parallax_deg) {
                    max_parallax_deg = ray_ang_deg;
                }
                if (line_ang_deg > max_line_angle_deg) {
                    max_line_angle_deg = line_ang_deg;
                }
                double bx = ox - (double)origins[j][0];
                double by = oy - (double)origins[j][1];
                double bz = oz - (double)origins[j][2];
                float baseline_m = (float)sqrt(bx * bx + by * by + bz * bz);
                if (baseline_m > max_baseline_m) {
                    max_baseline_m = baseline_m;
                }
            }
        }

        // Projection matrix onto plane orthogonal to ray i: P_i = I - d_i * d_i^T
        double p00 = 1.0 - dx * dx;
        double p01 = -dx * dy;
        double p02 = -dx * dz;
        double p11 = 1.0 - dy * dy;
        double p12 = -dy * dz;
        double p22 = 1.0 - dz * dz;

        a[0][0] += p00;
        a[0][1] += p01;
        a[0][2] += p02;
        a[0][3] += p00 * ox + p01 * oy + p02 * oz;

        a[1][0] += p01;
        a[1][1] += p11;
        a[1][2] += p12;
        a[1][3] += p01 * ox + p11 * oy + p12 * oz;

        a[2][0] += p02;
        a[2][1] += p12;
        a[2][2] += p22;
        a[2][3] += p02 * ox + p12 * oy + p22 * oz;
    }

    if (out_max_parallax_deg != nullptr) {
        *out_max_parallax_deg = max_parallax_deg;
    }
    if (max_line_angle_deg < 0.25f || max_baseline_m < 0.05f) {
        return false;
    }

    // Solve 3x3 linear system via Gaussian elimination with partial pivoting
    for (int col = 0; col < 3; ++col) {
        int    pivot = col;
        double best  = fabs(a[col][col]);
        for (int row = col + 1; row < 3; ++row) {
            if (fabs(a[row][col]) > best) {
                best  = fabs(a[row][col]);
                pivot = row;
            }
        }
        if (best < 1.0e-8) {
            return false;
        }
        if (pivot != col) {
            for (int k = col; k < 4; ++k) {
                double tmp    = a[col][k];
                a[col][k]     = a[pivot][k];
                a[pivot][k]   = tmp;
            }
        }
        double inv_diag = 1.0 / a[col][col];
        for (int k = col; k < 4; ++k) {
            a[col][k] *= inv_diag;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == col) {
                continue;
            }
            double factor = a[row][col];
            for (int k = col; k < 4; ++k) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }

    float px = (float)a[0][3];
    float py = (float)a[1][3];
    float pz = (float)a[2][3];
    if (!isfinite(px) || !isfinite(py) || !isfinite(pz)) {
        return false;
    }

    // Verify the triangulated 3D point lies in front of all observing rays and compute 3D RMSE
    double sum_sq_err = 0.0;
    for (uint32_t i = 0u; i < ray_count; ++i) {
        double dx = dirs[i][0];
        double dy = dirs[i][1];
        double dz = dirs[i][2];
        double dn = sqrt(dx * dx + dy * dy + dz * dz);
        dx /= dn;
        dy /= dn;
        dz /= dn;

        double vx = (double)px - (double)origins[i][0];
        double vy = (double)py - (double)origins[i][1];
        double vz = (double)pz - (double)origins[i][2];
        double along = vx * dx + vy * dy + vz * dz;
        if (along <= 0.05) {
            return false;
        }
        double rx = vx - along * dx;
        double ry = vy - along * dy;
        double rz = vz - along * dz;
        sum_sq_err += rx * rx + ry * ry + rz * rz;
    }

    out_point[0] = px;
    out_point[1] = py;
    out_point[2] = pz;
    if (out_rmse_m != nullptr) {
        *out_rmse_m = (float)sqrt(sum_sq_err / (double)ray_count);
    }
    return true;
}

