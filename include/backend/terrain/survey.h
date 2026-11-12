#ifndef ENGINE_BACKEND_TERRAIN_SURVEY_H
#define ENGINE_BACKEND_TERRAIN_SURVEY_H

#include "backend/terrain/pack.h"
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Photogrammetric Camera, Multi-View Stereo (MVS) & Geodetic Survey Fusion API
// ============================================================================
// Fuses user-supplied ground/aerial/drone photographs (with phone/drone sensor
// specs, lens focal length in mm, Brown-Conrady distortion, and gimbal yaw/pitch/roll),
// multi-view stereo (MVS) overlapping image pairs, and map-engineer geodetic
// control points / breaklines directly onto the Baklan & Beşparmak Dağı .gmpack
// quadtree tiles (LOD 0..4) in GPU BDA memory and on SSD.

enum : uint32_t {
    k_survey_max_photos         = 32u,
    k_survey_max_ortho_overlays = 32u,
    k_survey_max_breaklines     = 256u,
    k_survey_max_tiepoints      = 512u,
    k_survey_max_views_per_tie  = 8u,
};

typedef struct TerrainSensorPreset {
    const char *name;             // e.g. "iPhone 15 Pro Main", "DJI Mavic 3 Enterprise"
    float       sensor_width_mm;  // Active physical sensor width (mm)
    float       sensor_height_mm; // Active physical sensor height (mm)
    float       default_focal_mm; // Default physical lens focal length (mm)
    float       focal_35mm_equiv; // 35mm full-frame equivalent focal length (mm)
    float       k1;               // Default radial distortion k1
    float       k2;               // Default radial distortion k2
} TerrainSensorPreset;

typedef struct TerrainCameraIntrinsics {
    uint32_t width_px;         // Image width in pixels
    uint32_t height_px;        // Image height in pixels
    float    sensor_width_mm;  // Physical sensor width (mm)
    float    sensor_height_mm; // Physical sensor height (mm)
    float    focal_mm;         // Physical focal length (mm)
    float    focal_35mm_mm;    // 35mm equivalent focal length (mm; used if focal_mm <= 0)
    float    fx_px;            // Horizontal focal length in pixels
    float    fy_px;            // Vertical focal length in pixels
    float    cx_px;            // Principal point X in pixels (0 = center)
    float    cy_px;            // Principal point Y in pixels (0 = center)
    float    k1;               // Brown-Conrady radial distortion k1
    float    k2;               // Brown-Conrady radial distortion k2
    float    k3;               // Brown-Conrady radial distortion k3
    float    p1;               // Brown-Conrady tangential distortion p1
    float    p2;               // Brown-Conrady tangential distortion p2
    float    hfov_deg;         // Computed horizontal field of view (degrees)
    float    vfov_deg;         // Computed vertical field of view (degrees)
} TerrainCameraIntrinsics;

typedef struct TerrainCameraPose {
    double latitude_deg;   // WGS84 latitude (degrees North)
    double longitude_deg;  // WGS84 longitude (degrees East)
    float  altitude_msl_m; // Camera altitude above Mean Sea Level (meters)
    float  height_agl_m;   // Optional height above ground level (meters; used if altitude_msl_m <= 0)
    float  world_x;        // Metric East coordinate in world space (meters, [-12000..+12000])
    float  world_y;        // Metric North coordinate in world space (meters, [-12000..+12000])
    float  yaw_deg;        // Gimbal compass heading: 0=North(+Y), 90=East(+X), 180=South(-Y), 270=West(-X)
    float  pitch_deg;      // Gimbal pitch from horizon: 0=horizontal, -90=nadir straight down(-Z)
    float  roll_deg;       // Gimbal roll around optical axis: 0=level, +clockwise
    bool   use_wgs84;      // True to derive (world_x, world_y) from (latitude_deg, longitude_deg)
    bool   use_agl;        // True when altitude_msl_m is derived from ground DEM + height_agl_m
    float  right[3];       // Orthonormal camera +X (sensor right) in ENU world coordinates
    float  up[3];          // Orthonormal camera +Y (sensor up) in ENU world coordinates
    float  forward[3];     // Orthonormal camera +Z (optical look axis) in ENU world coordinates
} TerrainCameraPose;

typedef struct TerrainSurveyPhoto {
    char                    name[128];
    char                    image_path[512];
    TerrainCameraIntrinsics intrinsics;
    TerrainCameraPose       pose;
    uint32_t               *rgba_pixels;  // Owned RGBA8 texel array (width_px * height_px)
    float                   blend_weight; // Radiometric blend weight [0..1]
    bool                    enable_mvs;   // Participate in multi-view stereo DEM elevation refinement
} TerrainSurveyPhoto;

typedef struct TerrainSurveyPoint {
    double  latitude_deg;
    double  longitude_deg;
    float   world_x;
    float   world_y;
    float   elevation_m;
    float   radius_m;   // Spatial influence radius (meters)
    float   weight;     // Confidence weight [0..1]
    uint8_t r;
    uint8_t g;
    uint8_t b;
    bool    has_color;
    bool    use_wgs84;
} TerrainSurveyPoint;

typedef struct TerrainSurveyBreakline {
    float world_ax;
    float world_ay;
    float elev_a_m;
    float world_bx;
    float world_by;
    float elev_b_m;
    float radius_m;
    float weight;
} TerrainSurveyBreakline;

typedef struct TerrainStereoObservation {
    uint32_t photo_index;
    float    u_px;
    float    v_px;
} TerrainStereoObservation;

typedef struct TerrainStereoTiePoint {
    TerrainStereoObservation views[k_survey_max_views_per_tie];
    uint32_t                 view_count;
    float                    radius_m;
    float                    weight;
    float                    triangulated_x;
    float                    triangulated_y;
    float                    triangulated_elev_m;
    float                    reproj_rmse_m;
    bool                     valid;
} TerrainStereoTiePoint;

typedef struct TerrainSurveyStats {
    uint32_t photos_loaded;
    uint32_t survey_points_loaded;
    uint32_t breaklines_loaded;
    uint32_t tiepoints_triangulated;
    uint32_t mvs_elev_posts_refined;
    uint32_t tiles_updated;
    uint32_t texels_orthorectified;
    uint32_t dem_posts_refined;
    float    max_elev_delta_m;
    float    mean_mvs_ncc;
} TerrainSurveyStats;

typedef struct TerrainOrthoOverlay {
    char      image_path[512];
    uint32_t *rgba_pixels;  // Owned RGBA8 texel array (width_px * height_px)
    uint32_t  width_px;
    uint32_t  height_px;
    float     center_x;     // Bird's-eye center World X (meters, East = +X)
    float     center_y;     // Bird's-eye center World Y (meters, North = +Y)
    float     half_span_x;  // Half-width in World X (meters)
    float     half_span_y;  // Half-height in World Y (meters)
    float     blend_weight; // Radiometric blend weight [0..1]
} TerrainOrthoOverlay;

typedef struct TerrainSurveyDataset {
    TerrainSurveyPhoto     photos[k_survey_max_photos];
    uint32_t               photo_count;
    TerrainOrthoOverlay    ortho_overlays[k_survey_max_ortho_overlays];
    uint32_t               ortho_overlay_count;
    TerrainSurveyPoint    *points;
    uint32_t               point_count;
    uint32_t               point_cap;
    TerrainSurveyBreakline breaklines[k_survey_max_breaklines];
    uint32_t               breakline_count;
    TerrainStereoTiePoint  tiepoints[k_survey_max_tiepoints];
    uint32_t               tiepoint_count;
    float                  mvs_search_range_m; // Elevation search window +-meters around prior DEM
    float                  mvs_step_m;         // Elevation sweep step (meters)
    float                  mvs_min_ncc;        // Minimum ZNCC correlation threshold
    bool                   persist_gmpack;     // True if manifest requests writing fused tiles to .gmpack
    uint8_t                node_stats_counted[k_gmpack_node_count];
    TerrainSurveyStats     last_stats;
} TerrainSurveyDataset;

// Coordinate & Camera Photogrammetry Math (camera_model.c)
void backend_survey_wgs84_to_world(const GpuMathPageDirectory *dir,
                                   double                      lat_deg,
                                   double                      lon_deg,
                                   float                      *out_wx,
                                   float                      *out_wy);
void backend_survey_world_to_wgs84(const GpuMathPageDirectory *dir,
                                   float                       wx,
                                   float                       wy,
                                   double                     *out_lat_deg,
                                   double                     *out_lon_deg);
bool backend_survey_utm_to_wgs84(uint32_t zone,
                                 bool     northern_hemisphere,
                                 double   easting_m,
                                 double   northing_m,
                                 double  *out_lat_deg,
                                 double  *out_lon_deg);
bool backend_survey_find_sensor_preset(const char          *model_name,
                                       TerrainSensorPreset *out_preset);
bool backend_survey_init_intrinsics(TerrainCameraIntrinsics *intr);
void backend_survey_init_pose(const GpuMathPageDirectory *dir,
                              TerrainCameraPose          *pose);
bool backend_survey_project_world_to_pixel(const TerrainCameraIntrinsics *intr,
                                           const TerrainCameraPose       *pose,
                                           float                          wx,
                                           float                          wy,
                                           float                          elev_m,
                                           float                         *out_u_px,
                                           float                         *out_v_px,
                                           float                         *out_depth_m);
bool backend_survey_pixel_to_world_ray(const TerrainCameraIntrinsics *intr,
                                       const TerrainCameraPose       *pose,
                                       float                          u_px,
                                       float                          v_px,
                                       float                          out_origin[3],
                                       float                          out_dir[3]);
bool backend_survey_triangulate_rays(uint32_t     ray_count,
                                     const float (*origins)[3],
                                     const float (*dirs)[3],
                                     float        out_point[3],
                                     float       *out_rmse_m,
                                     float       *out_max_parallax_deg);

// Dataset Management, Multi-View Stereo & Tile Fusion (mvs_fusion.c)
void backend_survey_dataset_init(TerrainSurveyDataset *ds);
void backend_survey_dataset_free(TerrainSurveyDataset *ds);
int  backend_survey_add_photo(TerrainSurveyDataset       *ds,
                              const GpuMathPageDirectory *dir,
                              const TerrainSurveyPhoto   *photo_spec);
int  backend_survey_add_ortho_overlay(TerrainSurveyDataset      *ds,
                                      const TerrainOrthoOverlay *ov_spec);
uint32_t backend_survey_bake_ortho_tile(const GpuMathQuadNode *node,
                                        GpuMathTileBlock      *tile,
                                        const uint32_t        *rgba_pixels,
                                        uint32_t               width_px,
                                        uint32_t               height_px,
                                        float                  center_x,
                                        float                  center_y,
                                        float                  half_span_x,
                                        float                  half_span_y,
                                        float                  blend_weight);
int  backend_survey_add_point(TerrainSurveyDataset       *ds,
                              const GpuMathPageDirectory *dir,
                              const TerrainSurveyPoint   *pt);
int  backend_survey_add_breakline(TerrainSurveyDataset         *ds,
                                  const TerrainSurveyBreakline *bl);
int  backend_survey_add_tiepoint(TerrainSurveyDataset        *ds,
                                 const TerrainStereoTiePoint *tp);
int  backend_survey_solve_tiepoints(TerrainSurveyDataset       *ds,
                                    const GpuMathPageDirectory *dir);
bool backend_survey_tile_may_overlap(const TerrainSurveyDataset *ds,
                                     const GpuMathQuadNode      *node);
bool backend_survey_fuse_tile(TerrainSurveyDataset *ds,
                              GpuMathArchiveHeader *hdr,
                              uint32_t              node_index,
                              GpuMathTileBlock     *tile,
                              TerrainSurveyStats   *io_stats);
int  backend_survey_apply_to_pack_file(TerrainSurveyDataset *ds,
                                       const char           *pack_path,
                                       TerrainSurveyStats   *out_stats);

// Image, CSV Point Cloud & Manifest I/O (manifest_io.c)
int  backend_survey_load_image_file(const char *path,
                                    uint32_t  **out_rgba,
                                    uint32_t   *out_w,
                                    uint32_t   *out_h);
int  backend_survey_save_png_file(const char     *path,
                                  const uint32_t *rgba,
                                  uint32_t        width,
                                  uint32_t        height);
int  backend_survey_load_csv_points(TerrainSurveyDataset       *ds,
                                    const GpuMathPageDirectory *dir,
                                    const char                 *csv_path,
                                    float                       default_radius_m,
                                    float                       default_weight);
int  backend_survey_load_gcp_list(TerrainSurveyDataset       *ds,
                                  const GpuMathPageDirectory *dir,
                                  const char                 *gcp_path);
int  backend_survey_load_manifest(TerrainSurveyDataset       *ds,
                                  const GpuMathPageDirectory *dir,
                                  const char                 *manifest_path);
int  backend_survey_ensure_default_manifest(const char                 *manifest_path,
                                            const GpuMathPageDirectory *dir);

// ============================================================================
// Engine v0.2 Milestone 5: Oriented 2D Gaussian Splatting (2DGS) Geodetic Surfels
// Reference: Huang et al., "2D Gaussian Splatting for Geometrically Accurate
//            Radiance Fields" (SIGGRAPH 2024–2026) + Energy-Conserving Mip-2DGS
// ============================================================================

enum : uint32_t {
    k_surfel_cap_uma       = 16384u, // 512 KiB on UMA_IGPU (fits single 40 MiB slab)
    k_surfel_cap_dgpu      = 65536u, // 2.00 MiB on DISCRETE_DIRECT / HYBRID_MUXLESS
    k_surfel_flag_cliff    = 1u << 0u,
    k_surfel_flag_gcp      = 1u << 1u,
    k_surfel_flag_tiepoint = 1u << 2u,
    k_surfel_flag_breakln  = 1u << 3u,
};

// 32-byte cache-line-packed Oriented 2D Gaussian Splatting (2DGS) Geodetic Surfel
// (exact 2 surfels per 64B cache line; 0 affine EWA warping error)
typedef struct TerrainSurfel2DGS {
    alignas(32) float center_x; // Metric East world X (meters)
    float    center_y;          // Metric North world Y (meters)
    float    elev_m;            // Geodetic MSL elevation (meters)
    uint32_t nrm_tan_packed;    // Low 16b: Oct-SNORM8 normal N; High 16b: UNORM16 tangent angle phi
    uint32_t scale_uv_f16;      // Low 16b: fp16 sigma_u (meters); High 16b: fp16 sigma_v (meters)
    uint32_t rgba_opa_packed;   // RGBA8: sRGB rock/survey albedo (R,G,B) + base opacity (A)
    uint32_t flags_node_id;     // Low 16b: quadtree node_id; High 16b: surfel kind flags
    uint32_t reserved_pad;      // Exact 32-byte alignment stride
} TerrainSurfel2DGS;

static_assert(sizeof(TerrainSurfel2DGS) == 32u, "TerrainSurfel2DGS must be 32 bytes");

typedef struct TerrainSurfelRayHit {
    float t_ray;     // Ray distance along world ray O + t * D
    float u_norm;    // Normalized tangent-plane coordinate u / sigma_u
    float v_norm;    // Normalized tangent-plane coordinate v / sigma_v
    float alpha;     // Evaluated Mip-2DGS energy-conserving Gaussian opacity in [0..1]
    bool  valid;
} TerrainSurfelRayHit;

void backend_surfel_2dgs_pack(TerrainSurfel2DGS *out,
                              float              wx,
                              float              wy,
                              float              elev_m,
                              const float        normal[3],
                              float              tangent_angle_rad,
                              float              sigma_u_m,
                              float              sigma_v_m,
                              uint8_t            r,
                              uint8_t            g,
                              uint8_t            b,
                              uint8_t            opacity_u8,
                              uint16_t           node_id,
                              uint16_t           flags);

void backend_surfel_2dgs_unpack_basis(const TerrainSurfel2DGS *surfel,
                                      float                    out_nrm[3],
                                      float                    out_tan_u[3],
                                      float                    out_tan_v[3],
                                      float                   *out_sigma_u_m,
                                      float                   *out_sigma_v_m);

bool backend_surfel_2dgs_eval_ray(const TerrainSurfel2DGS *surfel,
                                  float                    elev_base_m,
                                  float                    vertical_scale,
                                  const float              ray_origin[3],
                                  const float              ray_dir[3],
                                  float                    filter_sigma_m,
                                  TerrainSurfelRayHit     *out_hit);

uint32_t backend_surfel_2dgs_extract_tile(const GpuMathQuadNode *node,
                                          uint32_t               node_index,
                                          const EpackTileBlock  *tile,
                                          float                  vertical_scale,
                                          uint32_t               sample_stride,
                                          TerrainSurfel2DGS     *out_surfels,
                                          uint32_t               max_surfels);

uint32_t backend_surfel_2dgs_extract_survey(const TerrainSurveyDataset *ds,
                                            TerrainSurfel2DGS          *out_surfels,
                                            uint32_t                    max_surfels);

[[nodiscard]] bool backend_surfel_2dgs_self_check(void);

#endif /* ENGINE_BACKEND_TERRAIN_SURVEY_H */
