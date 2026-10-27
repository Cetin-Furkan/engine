#ifndef ENGINE_FRAME_PLOT_FILE_H
#define ENGINE_FRAME_PLOT_FILE_H

#include "ring/ring.h"
#include <stdint.h>

// One sample from plots/data.csv or an OBJ polyline. brk above 0.5 starts a
// new polyline. An OBJ face is not a chart sample. A .blend file is
// Blender's database and is not parsed.

typedef struct PlotSample {
    float x;
    float y;
    float z;
    float brk;
} PlotSample;

static_assert(sizeof(PlotSample) == 16u, "plot sample");

// Writes raw samples into dst, CSV first, then OBJ. Logs each file.
// *found is true when a file was read. *capped is true when a cap stopped
// the read. on_cqe receives completions that are not the file read.
uint32_t plot_file_load(PlotSample *dst, uint32_t cap, bool *found, bool *capped,
                        RingCqeFn on_cqe, void *on_user);

// One OBJ corner. Position and normal are still in file space. The mesh
// fits the positions after this returns. Albedo is the clay the draw uses.
typedef struct ObjCorner {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float r;
    float g;
    float b;
} ObjCorner;

static_assert(sizeof(ObjCorner) == 36u, "obj corner");

// Triangle cap for one OBJ. A larger file keeps the first faces.
enum { k_obj_tri_cap = 4096u };

// Writes expanded corners, three per triangle. Returns the corner count,
// or -1 when the read fails. *found is true when the file was read.
// *capped is the triangle cap. *crossed is a face the clip refused.
// The chart load does not call this. The two reads stay sequential.
int plot_file_load_mesh(ObjCorner *dst, uint32_t corner_cap, bool *found, bool *capped,
                        bool *crossed, RingCqeFn on_cqe, void *on_user);

// One CCW triangle, one convex quad, one bowtie. False when the clip
// does not match. The caller logs that once and does not abort.
bool plot_file_mesh_check(void);

#endif /* ENGINE_FRAME_PLOT_FILE_H */
