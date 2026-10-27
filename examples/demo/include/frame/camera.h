#ifndef ENGINE_FRAME_CAMERA_H
#define ENGINE_FRAME_CAMERA_H

#include <stdint.h>

// The pose will not zoom past these. Hit tests and the plot ribbons use
// the same floor, so a grab and a stroke keep their screen size out to
// the far end of the range.
static const float k_camera_zoom_min = 0.0001f;
static const float k_camera_zoom_max = 10000.0f;

// The view of the plane. Identity pan, zoom 1, and a unit quaternion
// reproduce the picture the window had before the camera existed:
// one world pixel is one screen pixel, +X right, +Y up.
// A step is an exponential map on the quaternion, then the roll is
// removed so the plane keeps a stable up. Zoom multiplies by an
// exponential of the key axis. Move follows the camera's right and up,
// depth included, so an arrow stays on the screen axis. The step is
// scaled by 1/zoom. The clock is the wl_surface.frame stamp. Nothing
// here starts the cube's revolution.

typedef struct ViewPush {
    uint64_t materials;
    uint32_t mat_index;
    uint32_t mat_pad;
    float    qx;
    float    qy;
    float    qz;
    float    qw;
    float    cam_x;
    float    cam_y;
    float    cam_z;
    float    zoom;
} ViewPush;

void camera_reset(void);

// world_draw chooses the pose the shaders read. A live frame uses the
// pose just integrated. A resize uses the pose already on screen.
void camera_bind(bool live);

// The pose camera_bind selected. The clipmap snaps to this point.
void camera_bound_xy(float *x, float *y);

// Fills the tail every draw push ends with. mat_index names one record
// in the shared table. The address is 0 until that table exists.
void view_push_fill(ViewPush *out, uint32_t mat_index);

// One compositor stamp. True when the pose differs from the image on screen.
bool camera_advance(uint32_t time_ms);

bool camera_busy(void);
void camera_commit(void);

// True while a move, yaw, pitch, or zoom key is held.
bool camera_motion_down(void);

// Identity quaternion and camera z = 0. x, y, and zoom stay.
// True when the pose changed and a frame should be presented.
bool camera_snap_front(void);

// Shown zoom. Clicks use this so a grab stays a few screen pixels.
float camera_zoom_shown(void);

// One pixel on the shown picture. screen_x is right of center, screen_y is
// up. The ray starts on the camera side of that pixel and travels with the
// look direction, so the first hit is the front face. plane_x and plane_y
// are where that ray crosses z = 0. At the identity pose that crossing
// equals screen_x, screen_y.
typedef struct CameraRay {
    float ox;
    float oy;
    float oz;
    float dx;
    float dy;
    float dz;
    float plane_x;
    float plane_y;
} CameraRay;

void camera_click_ray(float screen_x, float screen_y, CameraRay *out);

// The z = 0 crossing of camera_click_ray. width and height are unused.
// Kept so a caller that only wants the plane point stays short.
void camera_unproject(float ox, float oy, uint32_t width, uint32_t height,
                      float *wx, float *wy);

#endif /* ENGINE_FRAME_CAMERA_H */
