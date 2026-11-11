#include "backend/internal.h"
#include <ctype.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Survey Image Loader (PNG via libpng + PPM/PGM), CSV Point Cloud & Manifest
// ============================================================================

static void resolve_relative_asset(const char *manifest_path,
                                   const char *raw_path,
                                   char       *out_path,
                                   size_t      out_cap) {
    if (raw_path == nullptr || out_path == nullptr || out_cap == 0u) {
        return;
    }
    if (access(raw_path, R_OK) == 0 || raw_path[0] == '/') {
        snprintf(out_path, out_cap, "%s", raw_path);
        return;
    }
    if (manifest_path != nullptr && manifest_path[0] != '\0') {
        char dir_buf[1024];
        snprintf(dir_buf, sizeof dir_buf, "%s", manifest_path);
        for (uint32_t up = 0u; up < 5u; ++up) {
            char *slash = strrchr(dir_buf, '/');
            if (slash == nullptr) {
                break;
            }
            *slash = '\0';
            int w = snprintf(out_path, out_cap, "%s/%s", dir_buf, raw_path);
            if (w > 0 && (size_t)w < out_cap && access(out_path, R_OK) == 0) {
                return;
            }
        }
    }
    snprintf(out_path, out_cap, "%s", raw_path);
}

static int load_pnm_stream(FILE      *fp,
                           uint32_t **out_rgba,
                           uint32_t  *out_w,
                           uint32_t  *out_h) {
    if (fp == nullptr) {
        return -1;
    }
    char magic[4] = {0};
    if (fscanf(fp, "%3s", magic) != 1) {
        return -1;
    }
    bool is_ppm_bin = (strcmp(magic, "P6") == 0);
    bool is_pgm_bin = (strcmp(magic, "P5") == 0);
    bool is_ppm_asc = (strcmp(magic, "P3") == 0);
    bool is_pgm_asc = (strcmp(magic, "P2") == 0);
    if (!is_ppm_bin && !is_pgm_bin && !is_ppm_asc && !is_pgm_asc) {
        return -1;
    }

    int vals[3] = {0};
    int got     = 0;
    while (got < 3) {
        int c = fgetc(fp);
        if (c == EOF) {
            break;
        }
        if (isspace(c)) {
            continue;
        }
        if (c == '#') {
            while ((c = fgetc(fp)) != EOF && c != '\n') {
            }
            continue;
        }
        ungetc(c, fp);
        if (fscanf(fp, "%d", &vals[got]) != 1) {
            break;
        }
        got++;
    }
    if (got != 3 || vals[0] <= 0 || vals[1] <= 0 || vals[2] <= 0 ||
        vals[0] > 16384 || vals[1] > 16384) {
        return -1;
    }
    // Consume single whitespace after maxval
    (void)fgetc(fp);

    uint32_t w     = (uint32_t)vals[0];
    uint32_t h     = (uint32_t)vals[1];
    float    scale = 255.0f / (float)vals[2];
    size_t   total = (size_t)w * (size_t)h;
    uint32_t *rgba = malloc(total * sizeof(uint32_t));
    if (rgba == nullptr) {
        return -1;
    }

    if (is_ppm_bin) {
        bool is_16bit = (vals[2] > 255);
        for (size_t i = 0u; i < total; ++i) {
            uint32_t R = 0u, G = 0u, B = 0u;
            if (is_16bit) {
                uint8_t buf[6] = {0};
                if (fread(buf, 1u, 6u, fp) != 6u) {
                    free(rgba);
                    return -1;
                }
                uint32_t r16 = ((uint32_t)buf[0] << 8u) | (uint32_t)buf[1];
                uint32_t g16 = ((uint32_t)buf[2] << 8u) | (uint32_t)buf[3];
                uint32_t b16 = ((uint32_t)buf[4] << 8u) | (uint32_t)buf[5];
                R = (uint32_t)lrintf(fminf(fmaxf((float)r16 * scale, 0.0f), 255.0f));
                G = (uint32_t)lrintf(fminf(fmaxf((float)g16 * scale, 0.0f), 255.0f));
                B = (uint32_t)lrintf(fminf(fmaxf((float)b16 * scale, 0.0f), 255.0f));
            } else {
                uint8_t rgb[3] = {0};
                if (fread(rgb, 1u, 3u, fp) != 3u) {
                    free(rgba);
                    return -1;
                }
                if (vals[2] == 255) {
                    R = rgb[0];
                    G = rgb[1];
                    B = rgb[2];
                } else {
                    R = (uint32_t)lrintf(fminf(fmaxf((float)rgb[0] * scale, 0.0f), 255.0f));
                    G = (uint32_t)lrintf(fminf(fmaxf((float)rgb[1] * scale, 0.0f), 255.0f));
                    B = (uint32_t)lrintf(fminf(fmaxf((float)rgb[2] * scale, 0.0f), 255.0f));
                }
            }
            rgba[i] = R | (G << 8u) | (B << 16u) | 0xFF000000u;
        }
    } else if (is_pgm_bin) {
        bool is_16bit = (vals[2] > 255);
        for (size_t i = 0u; i < total; ++i) {
            uint32_t v = 0u;
            if (is_16bit) {
                uint8_t buf[2] = {0};
                if (fread(buf, 1u, 2u, fp) != 2u) {
                    free(rgba);
                    return -1;
                }
                uint32_t g16 = ((uint32_t)buf[0] << 8u) | (uint32_t)buf[1];
                v = (uint32_t)lrintf(fminf(fmaxf((float)g16 * scale, 0.0f), 255.0f));
            } else {
                int g = fgetc(fp);
                if (g == EOF) {
                    free(rgba);
                    return -1;
                }
                v = (vals[2] == 255)
                    ? (uint32_t)(g & 0xFF)
                    : (uint32_t)lrintf(fminf(fmaxf((float)(g & 0xFF) * scale, 0.0f), 255.0f));
            }
            rgba[i] = v | (v << 8u) | (v << 16u) | 0xFF000000u;
        }
    } else if (is_ppm_asc) {
        for (size_t i = 0u; i < total; ++i) {
            int r = 0, g = 0, b = 0;
            if (fscanf(fp, "%d %d %d", &r, &g, &b) != 3) {
                free(rgba);
                return -1;
            }
            uint32_t R = (uint32_t)lrintf(fminf(fmaxf((float)r * scale, 0.0f), 255.0f));
            uint32_t G = (uint32_t)lrintf(fminf(fmaxf((float)g * scale, 0.0f), 255.0f));
            uint32_t B = (uint32_t)lrintf(fminf(fmaxf((float)b * scale, 0.0f), 255.0f));
            rgba[i] = R | (G << 8u) | (B << 16u) | 0xFF000000u;
        }
    } else {
        for (size_t i = 0u; i < total; ++i) {
            int g = 0;
            if (fscanf(fp, "%d", &g) != 1) {
                free(rgba);
                return -1;
            }
            uint32_t V = (uint32_t)lrintf(fminf(fmaxf((float)g * scale, 0.0f), 255.0f));
            rgba[i] = V | (V << 8u) | (V << 16u) | 0xFF000000u;
        }
    }

    *out_rgba = rgba;
    *out_w    = w;
    *out_h    = h;
    return 0;
}

static int load_pnm_file(const char *path,
                         uint32_t  **out_rgba,
                         uint32_t   *out_w,
                         uint32_t   *out_h) {
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr) {
        return -1;
    }
    int rc = load_pnm_stream(fp, out_rgba, out_w, out_h);
    fclose(fp);
    return rc;
}

static int load_jpeg_file(const char *path,
                          uint32_t  **out_rgba,
                          uint32_t   *out_w,
                          uint32_t   *out_h) {
    FILE *chk = fopen(path, "rb");
    if (chk == nullptr) {
        return -1;
    }
    uint8_t sig[2] = {0};
    size_t nr = fread(sig, 1u, 2u, chk);
    fclose(chk);
    if (nr != 2u || sig[0] != 0xFFu || sig[1] != 0xD8u) {
        return -1;
    }
    // Reject paths containing shell metacharacters
    if (strpbrk(path, "'\"`$;&|<>\\\n\r") != nullptr) {
        return -1;
    }
    char cmd[1280];
    snprintf(cmd, sizeof cmd, "djpeg -pnm '%s' 2>/dev/null", path);
    FILE *pipe_fp = popen(cmd, "r");
    if (pipe_fp == nullptr) {
        return -1;
    }
    int rc = load_pnm_stream(pipe_fp, out_rgba, out_w, out_h);
    pclose(pipe_fp);
    return rc;
}

int backend_survey_load_image_file(const char *path,
                                   uint32_t  **out_rgba,
                                   uint32_t   *out_w,
                                   uint32_t   *out_h) {
    if (path == nullptr || path[0] == '\0' ||
        out_rgba == nullptr || out_w == nullptr || out_h == nullptr) {
        return -1;
    }
    *out_rgba = nullptr;
    *out_w    = 0u;
    *out_h    = 0u;

    // Try PNM (PPM/PGM) first if extension matches
    size_t len = strlen(path);
    if (len > 4u && (strcasecmp(path + len - 4u, ".ppm") == 0 ||
                     strcasecmp(path + len - 4u, ".pgm") == 0 ||
                     strcasecmp(path + len - 4u, ".pnm") == 0)) {
        if (load_pnm_file(path, out_rgba, out_w, out_h) == 0) {
            return 0;
        }
    }

    // Try JPEG if magic bytes or extension match
    if (load_jpeg_file(path, out_rgba, out_w, out_h) == 0) {
        return 0;
    }

    // Load PNG via libpng 1.6 simplified png_image API
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path) != 0) {
        image.format = PNG_FORMAT_RGBA;
        size_t bytes = PNG_IMAGE_SIZE(image);
        uint32_t *buf = malloc(bytes);
        if (buf == nullptr) {
            png_image_free(&image);
            return -1;
        }
        if (png_image_finish_read(&image, nullptr, buf, 0, nullptr) != 0) {
            *out_rgba = buf;
            *out_w    = image.width;
            *out_h    = image.height;
            png_image_free(&image);
            return 0;
        }
        free(buf);
        png_image_free(&image);
    }

    // Fallback to PNM loader for extensionless or raw PNM files
    return load_pnm_file(path, out_rgba, out_w, out_h);
}

int backend_survey_save_png_file(const char     *path,
                                 const uint32_t *rgba,
                                 uint32_t        width,
                                 uint32_t        height) {
    if (path == nullptr || path[0] == '\0' || rgba == nullptr ||
        width == 0u || height == 0u) {
        return -1;
    }
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    image.width   = width;
    image.height  = height;
    image.format  = PNG_FORMAT_RGBA;
    if (png_image_write_to_file(&image, path, 0, rgba, 0, nullptr) == 0) {
        png_image_free(&image);
        return -1;
    }
    png_image_free(&image);
    return 0;
}

static void trim_line(char *s) {
    bool in_quotes = false;
    for (char *c = s; *c != '\0'; ++c) {
        if (*c == '"') {
            in_quotes = !in_quotes;
        } else if (*c == '#' && !in_quotes) {
            *c = '\0';
            break;
        }
    }
    size_t n = strlen(s);
    while (n > 0u && (s[n - 1u] == '\n' || s[n - 1u] == '\r' ||
                      s[n - 1u] == ' '  || s[n - 1u] == '\t')) {
        s[--n] = '\0';
    }
}

int backend_survey_load_csv_points(TerrainSurveyDataset       *ds,
                                   const GpuMathPageDirectory *dir,
                                   const char                 *csv_path,
                                   float                       default_radius_m,
                                   float                       default_weight) {
    if (ds == nullptr || csv_path == nullptr || csv_path[0] == '\0') {
        return -1;
    }
    FILE *fp = fopen(csv_path, "r");
    if (fp == nullptr) {
        return -1;
    }

    uint32_t added = 0u;
    char line[512];
    while (fgets(line, sizeof line, fp) != nullptr) {
        trim_line(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || isalpha((unsigned char)*p)) {
            continue;
        }
        // Replace commas and semicolons with spaces for uniform numeric parsing
        for (char *c = p; *c != '\0'; ++c) {
            if (*c == ',' || *c == ';') {
                *c = ' ';
            }
        }
        double v0 = 0.0, v1 = 0.0;
        float  elev = 0.0f;
        float  rad  = default_radius_m > 0.0f ? default_radius_m : 60.0f;
        float  wt   = default_weight > 0.0f ? default_weight : 0.90f;
        int    r = -1, g = -1, b = -1;

        int n = sscanf(p, "%lf %lf %f %f %f %d %d %d",
                       &v0, &v1, &elev, &rad, &wt, &r, &g, &b);
        if (n < 3) {
            continue;
        }
        // Auto-detect WGS84 (lat in [35..42], lon in [25..35]) vs local metric (wx, wy)
        bool is_wgs84 = (v0 >= 34.0 && v0 <= 43.0 && v1 >= 25.0 && v1 <= 36.0);
        TerrainSurveyPoint pt = {
            .latitude_deg  = is_wgs84 ? v0 : 0.0,
            .longitude_deg = is_wgs84 ? v1 : 0.0,
            .world_x       = is_wgs84 ? 0.0f : (float)v0,
            .world_y       = is_wgs84 ? 0.0f : (float)v1,
            .elevation_m   = elev,
            .radius_m      = rad,
            .weight        = wt,
            .r             = (n >= 8 && r >= 0 && r <= 255) ? (uint8_t)r : 0u,
            .g             = (n >= 8 && g >= 0 && g <= 255) ? (uint8_t)g : 0u,
            .b             = (n >= 8 && b >= 0 && b <= 255) ? (uint8_t)b : 0u,
            .has_color     = (n >= 8 && r >= 0 && g >= 0 && b >= 0),
            .use_wgs84     = is_wgs84,
        };
        if (backend_survey_add_point(ds, dir, &pt) >= 0) {
            added++;
        }
    }
    fclose(fp);
    return (int)added;
}

static char *next_token_quoted(char **cursor) {
    char *s = *cursor;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        *cursor = s;
        return nullptr;
    }
    char *start = s;
    while (*s != '\0' && *s != ' ' && *s != '\t') {
        if (*s == '"') {
            s++;
            while (*s != '\0' && *s != '"') {
                s++;
            }
            if (*s == '"') {
                s++;
            }
        } else {
            s++;
        }
    }
    if (*s != '\0') {
        *s = '\0';
        s++;
    }
    *cursor = s;
    return start;
}

static void strip_quotes(char *val) {
    size_t n = strlen(val);
    if (n >= 2u && val[0] == '"' && val[n - 1u] == '"') {
        memmove(val, val + 1, n - 2u);
        val[n - 2u] = '\0';
    }
}

static void parse_photo_line(TerrainSurveyDataset       *ds,
                             const GpuMathPageDirectory *dir,
                             const char                 *manifest_path,
                             char                       *rest) {
    char *img_tok = next_token_quoted(&rest);
    if (img_tok == nullptr) {
        return;
    }
    strip_quotes(img_tok);

    char resolved_img[512];
    resolve_relative_asset(manifest_path, img_tok, resolved_img, sizeof resolved_img);

    uint32_t *rgba = nullptr;
    uint32_t  w    = 0u;
    uint32_t  h    = 0u;
    if (backend_survey_load_image_file(resolved_img, &rgba, &w, &h) != 0) {
        fprintf(stderr, "[Survey] Could not load photo image '%s'\n", resolved_img);
        return;
    }

    TerrainSurveyPhoto spec;
    memset(&spec, 0, sizeof spec);
    snprintf(spec.name, sizeof spec.name, "%s", img_tok);
    snprintf(spec.image_path, sizeof spec.image_path, "%.*s", (int)(sizeof spec.image_path - 1u), resolved_img);
    spec.intrinsics.width_px         = w;
    spec.intrinsics.height_px        = h;
    spec.intrinsics.sensor_width_mm  = 36.0f;
    spec.intrinsics.sensor_height_mm = 24.0f;
    spec.intrinsics.focal_mm         = 24.0f;
    spec.pose.pitch_deg              = -90.0f;
    spec.pose.altitude_msl_m         = 0.0f;
    spec.pose.height_agl_m           = 120.0f;
    spec.pose.use_agl                = true;
    spec.blend_weight                = 1.0f;
    spec.enable_mvs                  = true;
    spec.rgba_pixels                 = rgba;

    TerrainSensorPreset matched_preset;
    memset(&matched_preset, 0, sizeof matched_preset);
    bool has_preset     = false;
    bool has_name       = false;
    bool has_sensor_w   = false;
    bool has_sensor_h   = false;
    bool has_focal_mm   = false;
    bool has_focal_35mm = false;
    bool has_k1         = false;
    bool has_k2         = false;
    bool has_alt_msl    = false;
    bool has_agl        = false;

    char *tok = nullptr;
    while ((tok = next_token_quoted(&rest)) != nullptr) {
        char *eq = strchr(tok, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        const char *key = tok;
        char       *val = eq + 1;
        strip_quotes(val);

        if (strcasecmp(key, "preset") == 0 || strcasecmp(key, "phone") == 0 ||
            strcasecmp(key, "drone") == 0  || strcasecmp(key, "camera") == 0 ||
            strcasecmp(key, "model") == 0) {
            if (backend_survey_find_sensor_preset(val, &matched_preset)) {
                has_preset = true;
            }
            if (!has_name) {
                snprintf(spec.name, sizeof spec.name, "%s", val);
            }
        } else if (strcasecmp(key, "name") == 0) {
            snprintf(spec.name, sizeof spec.name, "%s", val);
            has_name = true;
        } else if (strcasecmp(key, "sensor_w") == 0 || strcasecmp(key, "sensor_width_mm") == 0) {
            spec.intrinsics.sensor_width_mm = strtof(val, nullptr);
            has_sensor_w = true;
        } else if (strcasecmp(key, "sensor_h") == 0 || strcasecmp(key, "sensor_height_mm") == 0) {
            spec.intrinsics.sensor_height_mm = strtof(val, nullptr);
            has_sensor_h = true;
        } else if (strcasecmp(key, "focal_mm") == 0 || strcasecmp(key, "lens_mm") == 0) {
            spec.intrinsics.focal_mm = strtof(val, nullptr);
            has_focal_mm = true;
        } else if (strcasecmp(key, "focal_35mm") == 0 || strcasecmp(key, "equiv_mm") == 0) {
            spec.intrinsics.focal_35mm_mm = strtof(val, nullptr);
            has_focal_35mm = true;
        } else if (strcasecmp(key, "cx") == 0) {
            spec.intrinsics.cx_px = strtof(val, nullptr);
        } else if (strcasecmp(key, "cy") == 0) {
            spec.intrinsics.cy_px = strtof(val, nullptr);
        } else if (strcasecmp(key, "k1") == 0) {
            spec.intrinsics.k1 = strtof(val, nullptr);
            has_k1 = true;
        } else if (strcasecmp(key, "k2") == 0) {
            spec.intrinsics.k2 = strtof(val, nullptr);
            has_k2 = true;
        } else if (strcasecmp(key, "k3") == 0) {
            spec.intrinsics.k3 = strtof(val, nullptr);
        } else if (strcasecmp(key, "p1") == 0) {
            spec.intrinsics.p1 = strtof(val, nullptr);
        } else if (strcasecmp(key, "p2") == 0) {
            spec.intrinsics.p2 = strtof(val, nullptr);
        } else if (strcasecmp(key, "lat") == 0 || strcasecmp(key, "latitude") == 0) {
            spec.pose.latitude_deg = strtod(val, nullptr);
            spec.pose.use_wgs84    = true;
        } else if (strcasecmp(key, "lon") == 0 || strcasecmp(key, "longitude") == 0) {
            spec.pose.longitude_deg = strtod(val, nullptr);
            spec.pose.use_wgs84     = true;
        } else if (strcasecmp(key, "wx") == 0 || strcasecmp(key, "world_x") == 0) {
            spec.pose.world_x   = strtof(val, nullptr);
            spec.pose.use_wgs84 = false;
        } else if (strcasecmp(key, "wy") == 0 || strcasecmp(key, "world_y") == 0) {
            spec.pose.world_y   = strtof(val, nullptr);
            spec.pose.use_wgs84 = false;
        } else if (strcasecmp(key, "alt") == 0 || strcasecmp(key, "altitude") == 0 ||
                   strcasecmp(key, "alt_msl") == 0) {
            spec.pose.altitude_msl_m = strtof(val, nullptr);
            has_alt_msl = true;
        } else if (strcasecmp(key, "agl") == 0 || strcasecmp(key, "height_agl") == 0) {
            spec.pose.height_agl_m = strtof(val, nullptr);
            has_agl = true;
        } else if (strcasecmp(key, "yaw") == 0 || strcasecmp(key, "azimuth") == 0 ||
                   strcasecmp(key, "heading") == 0) {
            spec.pose.yaw_deg = strtof(val, nullptr);
        } else if (strcasecmp(key, "pitch") == 0 || strcasecmp(key, "tilt") == 0) {
            spec.pose.pitch_deg = strtof(val, nullptr);
        } else if (strcasecmp(key, "roll") == 0) {
            spec.pose.roll_deg = strtof(val, nullptr);
        } else if (strcasecmp(key, "weight") == 0) {
            spec.blend_weight = strtof(val, nullptr);
        } else if (strcasecmp(key, "mvs") == 0) {
            spec.enable_mvs = (atoi(val) != 0);
        }
    }

    if (has_preset) {
        if (!has_sensor_w) {
            spec.intrinsics.sensor_width_mm = matched_preset.sensor_width_mm;
        }
        if (!has_sensor_h) {
            spec.intrinsics.sensor_height_mm = matched_preset.sensor_height_mm;
        }
        if (!has_focal_mm && !has_focal_35mm) {
            spec.intrinsics.focal_mm      = matched_preset.default_focal_mm;
            spec.intrinsics.focal_35mm_mm = matched_preset.focal_35mm_equiv;
        }
        if (!has_k1) {
            spec.intrinsics.k1 = matched_preset.k1;
        }
        if (!has_k2) {
            spec.intrinsics.k2 = matched_preset.k2;
        }
    }
    if (!has_focal_mm && has_focal_35mm) {
        spec.intrinsics.focal_mm = 0.0f;
    }
    if (has_alt_msl && spec.pose.altitude_msl_m > 0.0f) {
        spec.pose.use_agl = false;
    } else if (has_agl) {
        spec.pose.use_agl        = true;
        spec.pose.altitude_msl_m = 0.0f;
    }

    (void)backend_survey_add_photo(ds, dir, &spec);
    free(rgba);
}

static void parse_ortho_overlay_line(TerrainSurveyDataset       *ds,
                                     const GpuMathPageDirectory *dir,
                                     const char                 *manifest_path,
                                     char                       *p) {
    char *img_tok = next_token_quoted(&p);
    if (img_tok == nullptr) {
        return;
    }
    strip_quotes(img_tok);
    char resolved_img[1024];
    resolve_relative_asset(manifest_path, img_tok, resolved_img, sizeof resolved_img);

    uint32_t *rgba = nullptr;
    uint32_t  w    = 0u;
    uint32_t  h    = 0u;
    if (backend_survey_load_image_file(resolved_img, &rgba, &w, &h) != 0) {
        return;
    }

    TerrainOrthoOverlay ov = {
        .rgba_pixels  = rgba,
        .width_px     = w,
        .height_px    = h,
        .center_x     = 1595.0f,
        .center_y     = -650.0f,
        .half_span_x  = 600.0f,
        .half_span_y  = 600.0f * ((float)h / (float)(w > 0u ? w : 1u)),
        .blend_weight = 1.0f,
    };
    snprintf(ov.image_path, sizeof ov.image_path, "%.*s",
             (int)(sizeof ov.image_path - 1u), resolved_img);

    double lat_deg = 0.0;
    double lon_deg = 0.0;
    bool   use_wgs = false;
    char *tok = nullptr;
    while ((tok = next_token_quoted(&p)) != nullptr) {
        char *eq = strchr(tok, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        const char *key = tok;
        char       *val = eq + 1;
        strip_quotes(val);
        if (strcasecmp(key, "wx") == 0 || strcasecmp(key, "world_x") == 0 || strcasecmp(key, "cx") == 0) {
            ov.center_x = strtof(val, nullptr);
            use_wgs     = false;
        } else if (strcasecmp(key, "wy") == 0 || strcasecmp(key, "world_y") == 0 || strcasecmp(key, "cy") == 0) {
            ov.center_y = strtof(val, nullptr);
            use_wgs     = false;
        } else if (strcasecmp(key, "lat") == 0 || strcasecmp(key, "latitude") == 0) {
            lat_deg = strtod(val, nullptr);
            use_wgs = true;
        } else if (strcasecmp(key, "lon") == 0 || strcasecmp(key, "longitude") == 0) {
            lon_deg = strtod(val, nullptr);
            use_wgs = true;
        } else if (strcasecmp(key, "span_x") == 0 || strcasecmp(key, "width_m") == 0) {
            ov.half_span_x = 0.5f * fmaxf(strtof(val, nullptr), 1.0f);
        } else if (strcasecmp(key, "span_y") == 0 || strcasecmp(key, "height_m") == 0) {
            ov.half_span_y = 0.5f * fmaxf(strtof(val, nullptr), 1.0f);
        } else if (strcasecmp(key, "hx") == 0 || strcasecmp(key, "half_x") == 0) {
            ov.half_span_x = fmaxf(strtof(val, nullptr), 0.5f);
        } else if (strcasecmp(key, "hy") == 0 || strcasecmp(key, "half_y") == 0) {
            ov.half_span_y = fmaxf(strtof(val, nullptr), 0.5f);
        } else if (strcasecmp(key, "weight") == 0 || strcasecmp(key, "opacity") == 0) {
            ov.blend_weight = strtof(val, nullptr);
        }
    }
    if (use_wgs && dir != nullptr) {
        backend_survey_wgs84_to_world(dir, lat_deg, lon_deg, &ov.center_x, &ov.center_y);
    }
    (void)backend_survey_add_ortho_overlay(ds, &ov);
    free(rgba);
}

int backend_survey_load_manifest(TerrainSurveyDataset       *ds,
                                 const GpuMathPageDirectory *dir,
                                 const char                 *manifest_path) {
    if (ds == nullptr || manifest_path == nullptr || manifest_path[0] == '\0') {
        return -1;
    }
    FILE *fp = fopen(manifest_path, "r");
    if (fp == nullptr) {
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof line, fp) != nullptr) {
        trim_line(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            continue;
        }

        char *kw = next_token_quoted(&p);
        if (kw == nullptr) {
            continue;
        }

        if (strcasecmp(kw, "mvs_config") == 0) {
            float range = 20.0f, step = 0.5f, min_ncc = 0.55f;
            if (sscanf(p, "%f %f %f", &range, &step, &min_ncc) >= 2) {
                ds->mvs_search_range_m = range;
                ds->mvs_step_m         = step;
                ds->mvs_min_ncc        = min_ncc;
            }
        } else if (strcasecmp(kw, "persist_gmpack") == 0) {
            int flag = 0;
            if (sscanf(p, "%d", &flag) == 1) {
                ds->persist_gmpack = (flag != 0);
            }
        } else if (strcasecmp(kw, "point_wgs84") == 0) {
            double lat = 0.0, lon = 0.0;
            float  elev = 0.0f, rad = 80.0f, wt = 0.95f;
            int    r = -1, g = -1, b = -1;
            int n = sscanf(p, "%lf %lf %f %f %f %d %d %d",
                           &lat, &lon, &elev, &rad, &wt, &r, &g, &b);
            if (n >= 3) {
                TerrainSurveyPoint pt = {
                    .latitude_deg  = lat,
                    .longitude_deg = lon,
                    .elevation_m   = elev,
                    .radius_m      = rad,
                    .weight        = wt,
                    .r             = (n >= 8 && r >= 0 && r <= 255) ? (uint8_t)r : 0u,
                    .g             = (n >= 8 && g >= 0 && g <= 255) ? (uint8_t)g : 0u,
                    .b             = (n >= 8 && b >= 0 && b <= 255) ? (uint8_t)b : 0u,
                    .has_color     = (n >= 8 && r >= 0 && g >= 0 && b >= 0),
                    .use_wgs84     = true,
                };
                (void)backend_survey_add_point(ds, dir, &pt);
            }
        } else if (strcasecmp(kw, "point_world") == 0) {
            float wx = 0.0f, wy = 0.0f, elev = 0.0f, rad = 80.0f, wt = 0.95f;
            int   r = -1, g = -1, b = -1;
            int n = sscanf(p, "%f %f %f %f %f %d %d %d",
                           &wx, &wy, &elev, &rad, &wt, &r, &g, &b);
            if (n >= 3) {
                TerrainSurveyPoint pt = {
                    .world_x     = wx,
                    .world_y     = wy,
                    .elevation_m = elev,
                    .radius_m    = rad,
                    .weight      = wt,
                    .r           = (n >= 8 && r >= 0 && r <= 255) ? (uint8_t)r : 0u,
                    .g           = (n >= 8 && g >= 0 && g <= 255) ? (uint8_t)g : 0u,
                    .b           = (n >= 8 && b >= 0 && b <= 255) ? (uint8_t)b : 0u,
                    .has_color   = (n >= 8 && r >= 0 && g >= 0 && b >= 0),
                    .use_wgs84   = false,
                };
                (void)backend_survey_add_point(ds, dir, &pt);
            }
        } else if (strcasecmp(kw, "breakline_wgs84") == 0) {
            double lat_a = 0.0, lon_a = 0.0, lat_b = 0.0, lon_b = 0.0;
            float  elev_a = 0.0f, elev_b = 0.0f, rad = 60.0f, wt = 0.90f;
            if (sscanf(p, "%lf %lf %f %lf %lf %f %f %f",
                       &lat_a, &lon_a, &elev_a, &lat_b, &lon_b, &elev_b, &rad, &wt) >= 6) {
                float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
                backend_survey_wgs84_to_world(dir, lat_a, lon_a, &ax, &ay);
                backend_survey_wgs84_to_world(dir, lat_b, lon_b, &bx, &by);
                TerrainSurveyBreakline bl = {
                    .world_ax = ax, .world_ay = ay, .elev_a_m = elev_a,
                    .world_bx = bx, .world_by = by, .elev_b_m = elev_b,
                    .radius_m = rad, .weight = wt,
                };
                (void)backend_survey_add_breakline(ds, &bl);
            }
        } else if (strcasecmp(kw, "breakline_world") == 0) {
            float ax = 0.0f, ay = 0.0f, elev_a = 0.0f;
            float bx = 0.0f, by = 0.0f, elev_b = 0.0f;
            float rad = 60.0f, wt = 0.90f;
            if (sscanf(p, "%f %f %f %f %f %f %f %f",
                       &ax, &ay, &elev_a, &bx, &by, &elev_b, &rad, &wt) >= 6) {
                TerrainSurveyBreakline bl = {
                    .world_ax = ax, .world_ay = ay, .elev_a_m = elev_a,
                    .world_bx = bx, .world_by = by, .elev_b_m = elev_b,
                    .radius_m = rad, .weight = wt,
                };
                (void)backend_survey_add_breakline(ds, &bl);
            }
        } else if (strcasecmp(kw, "survey_csv") == 0) {
            char *csv_tok = next_token_quoted(&p);
            if (csv_tok != nullptr) {
                strip_quotes(csv_tok);
                float rad = 60.0f, wt = 0.90f;
                (void)sscanf(p, "%f %f", &rad, &wt);
                char resolved_csv[1024];
                resolve_relative_asset(manifest_path, csv_tok, resolved_csv, sizeof resolved_csv);
                (void)backend_survey_load_csv_points(ds, dir, resolved_csv, rad, wt);
            }
        } else if (strcasecmp(kw, "gcp_list") == 0) {
            char *gcp_tok = next_token_quoted(&p);
            if (gcp_tok != nullptr) {
                strip_quotes(gcp_tok);
                char resolved_gcp[1024];
                resolve_relative_asset(manifest_path, gcp_tok, resolved_gcp, sizeof resolved_gcp);
                (void)backend_survey_load_gcp_list(ds, dir, resolved_gcp);
            }
        } else if (strcasecmp(kw, "tiepoint") == 0) {
            char *r_tok = next_token_quoted(&p);
            char *w_tok = next_token_quoted(&p);
            if (r_tok != nullptr && w_tok != nullptr) {
                TerrainStereoTiePoint tp;
                memset(&tp, 0, sizeof tp);
                tp.radius_m = strtof(r_tok, nullptr);
                tp.weight   = strtof(w_tok, nullptr);
                char *obs_tok = nullptr;
                while ((obs_tok = next_token_quoted(&p)) != nullptr &&
                       tp.view_count < k_survey_max_views_per_tie) {
                    uint32_t pidx = 0u;
                    float    u = 0.0f, v = 0.0f;
                    if (sscanf(obs_tok, "%u:%f:%f", &pidx, &u, &v) == 3) {
                        tp.views[tp.view_count++] = (TerrainStereoObservation){
                            .photo_index = pidx,
                            .u_px        = u,
                            .v_px        = v,
                        };
                    }
                }
                if (tp.view_count >= 2u) {
                    (void)backend_survey_add_tiepoint(ds, &tp);
                }
            }
        } else if (strcasecmp(kw, "photo") == 0) {
            parse_photo_line(ds, dir, manifest_path, p);
        } else if (strcasecmp(kw, "ortho_overlay") == 0) {
            parse_ortho_overlay_line(ds, dir, manifest_path, p);
        } else if (strcasecmp(kw, "save_photo_png") == 0) {
            char *idx_tok = next_token_quoted(&p);
            char *png_tok = next_token_quoted(&p);
            if (idx_tok != nullptr && png_tok != nullptr) {
                uint32_t pidx = (uint32_t)strtoul(idx_tok, nullptr, 10);
                strip_quotes(png_tok);
                if (pidx < ds->photo_count && ds->photos[pidx].rgba_pixels != nullptr) {
                    char resolved_png[1024];
                    resolve_relative_asset(manifest_path, png_tok, resolved_png, sizeof resolved_png);
                    (void)backend_survey_save_png_file(resolved_png,
                                                       ds->photos[pidx].rgba_pixels,
                                                       ds->photos[pidx].intrinsics.width_px,
                                                       ds->photos[pidx].intrinsics.height_px);
                }
            }
        }
    }

    fclose(fp);

    if (ds->tiepoint_count > 0u) {
        (void)backend_survey_solve_tiepoints(ds, dir);
    }
    return 0;
}

int backend_survey_load_gcp_list(TerrainSurveyDataset       *ds,
                                 const GpuMathPageDirectory *dir,
                                 const char                 *gcp_path) {
    if (ds == nullptr || gcp_path == nullptr || gcp_path[0] == '\0') {
        return -1;
    }
    FILE *fp = fopen(gcp_path, "r");
    if (fp == nullptr) {
        return -1;
    }
    uint32_t utm_zone  = 0u;
    bool     utm_north = true;
    bool     header_checked = false;
    int      added = 0;

    char line[1024];
    while (fgets(line, sizeof line, fp) != nullptr) {
        trim_line(line);
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            continue;
        }
        if (!header_checked) {
            header_checked = true;
            // Check for "WGS84 UTM 17N" or "+proj=utm +zone=17"
            const char *utm_pos = strstr(p, "UTM");
            if (utm_pos == nullptr) {
                utm_pos = strstr(p, "utm");
            }
            if (utm_pos != nullptr) {
                const char *z_ptr = utm_pos + 3;
                while (*z_ptr != '\0' && !isdigit((unsigned char)*z_ptr)) {
                    z_ptr++;
                }
                if (isdigit((unsigned char)*z_ptr)) {
                    utm_zone = (uint32_t)strtoul(z_ptr, nullptr, 10);
                    if (strchr(z_ptr, 'S') != nullptr || strchr(z_ptr, 's') != nullptr) {
                        utm_north = false;
                    }
                }
                continue;
            }
        }

        double x_coord = 0.0;
        double y_coord = 0.0;
        float  elev_m  = 0.0f;
        float  u_px    = 0.0f;
        float  v_px    = 0.0f;
        char   img_name[256] = {0};
        if (sscanf(p, "%lf %lf %f %f %f %255s",
                   &x_coord, &y_coord, &elev_m, &u_px, &v_px, img_name) < 6) {
            continue;
        }

        double lat_deg = y_coord;
        double lon_deg = x_coord;
        if (utm_zone > 0u) {
            if (!backend_survey_utm_to_wgs84(utm_zone, utm_north, x_coord, y_coord,
                                             &lat_deg, &lon_deg)) {
                continue;
            }
        }

        TerrainSurveyPoint pt = {
            .latitude_deg  = lat_deg,
            .longitude_deg = lon_deg,
            .elevation_m   = elev_m,
            .radius_m      = 38.0f,
            .weight        = 0.96f,
            .use_wgs84     = true,
        };
        if (backend_survey_add_point(ds, dir, &pt) >= 0) {
            added++;
        }

        float gcp_wx = 0.0f;
        float gcp_wy = 0.0f;
        backend_survey_wgs84_to_world(dir, lat_deg, lon_deg, &gcp_wx, &gcp_wy);

        // Bundle-adjust (anchor) the matching photo's camera station so the ray through (u_px, v_px)
        // passes directly through the surveyed 3D GCP (gcp_wx, gcp_wy, elev_m).
        const char *base_img = strrchr(img_name, '/');
        base_img = (base_img != nullptr) ? (base_img + 1) : img_name;
        for (uint32_t i = 0u; i < ds->photo_count; ++i) {
            TerrainSurveyPhoto *ph = &ds->photos[i];
            const char *ph_base = strrchr(ph->image_path, '/');
            ph_base = (ph_base != nullptr) ? (ph_base + 1) : ph->image_path;
            if (strcasecmp(ph->name, base_img) == 0 || strcasecmp(ph_base, base_img) == 0) {
                float u_obs = u_px;
                float v_obs = v_px;
                if (u_obs >= (float)ph->intrinsics.width_px || v_obs >= (float)ph->intrinsics.height_px) {
                    u_obs *= (float)ph->intrinsics.width_px / 4048.0f;
                    v_obs *= (float)ph->intrinsics.height_px / 3048.0f;
                }
                float orig[3] = {0};
                float rdir[3] = {0};
                if (backend_survey_pixel_to_world_ray(&ph->intrinsics, &ph->pose,
                                                      u_obs, v_obs, orig, rdir) &&
                    rdir[2] < -1.0e-3f) {
                    float t_hit = (elev_m - orig[2]) / rdir[2];
                    if (t_hit > 5.0f && t_hit < 2500.0f) {
                        ph->pose.world_x   = gcp_wx - rdir[0] * t_hit;
                        ph->pose.world_y   = gcp_wy - rdir[1] * t_hit;
                        ph->pose.use_wgs84 = false;
                        backend_survey_init_pose(dir, &ph->pose);
                    }
                }
                break;
            }
        }
    }
    fclose(fp);
    return added;
}

int backend_survey_ensure_default_manifest(const char                 *manifest_path,
                                           const GpuMathPageDirectory *dir) {
    (void)dir;
    if (manifest_path == nullptr || manifest_path[0] == '\0') {
        return -1;
    }
    struct stat st = {0};
    if (stat(manifest_path, &st) == 0 && st.st_size > 0) {
        return 0;
    }

    char dir_buf[1024];
    snprintf(dir_buf, sizeof dir_buf, "%s", manifest_path);
    char *slash = strrchr(dir_buf, '/');
    if (slash != nullptr) {
        *slash = '\0';
        (void)mkdir(dir_buf, 0755);
    }

    FILE *fp = fopen(manifest_path, "w");
    if (fp == nullptr) {
        return -1;
    }
    fprintf(fp,
            "# ============================================================================\n"
            "# Baklan & Beşparmak Dağı — Photogrammetric & Geodetic Survey Manifest\n"
            "# ============================================================================\n"
            "# Editing this file while World 8 is open automatically hot-reloads the 3D\n"
            "# terrain via io_uring inotify (or press L at any time).\n"
            "#\n"
            "# Add your own georeferenced phone/drone photos, RTK-GNSS GCP lists, or\n"
            "# stereo tie-points below using:\n"
            "#   photo <file.png|jpg> preset=\"iPhone 15 Pro Main\" lat=<deg> lon=<deg> alt=<m> yaw=<deg> pitch=<deg> roll=<deg> weight=0.90 mvs=1\n"
            "#   gcp_list <file.txt> [utm_zone=35] [north=1] [radius=25.0] [weight=0.95]\n"
            "#   point_wgs84 <lat> <lon> <elev_m> [radius_m] [weight]\n"
            "#   breakline_wgs84 <latA> <lonA> <elevA> <latB> <lonB> <elevB> [radius_m] [weight]\n"
            "\n"
            "mvs_config 18.0 0.40 0.55\n"
            "persist_gmpack 0\n");
    fclose(fp);
    return 0;
}
