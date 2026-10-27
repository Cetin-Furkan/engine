#ifndef ENGINE_TEXT_FACE_H
#define ENGINE_TEXT_FACE_H

#include <stdbool.h>
#include <stdint.h>

// Five roles. Sealed is the Inter file compiled into the binary. The other
// four open at boot and may be retargeted from face/dynamic.faces. A line
// that names sealed is refused, so an outside file cannot swap the mark.

enum : uint32_t {
    TEXT_FACE_SEALED = 0,
    TEXT_FACE_SYSTEM = 1,
    TEXT_FACE_TERMINAL = 2,
    TEXT_FACE_KNOWN_SANS = 3,
    TEXT_FACE_KNOWN_MONO = 4,
    TEXT_FACE_COUNT = 5,
    // One SDF size for every role. Draw size is a scale of these texels.
    TEXT_SDF_PX = 48,
    // FreeType sdf spread, in texels. 128 is the outline, higher is inside.
    TEXT_SDF_SPREAD = 8,
};

typedef struct TextSdf {
    const uint8_t *bytes;
    uint32_t       width;
    uint32_t       rows;
    int32_t        left;
    int32_t        top;
} TextSdf;

typedef struct TextShapedGlyph {
    uint32_t glyph;
    uint32_t byte;
    float    x_advance;
    float    y_advance;
    float    x_offset;
    float    y_offset;
} TextShapedGlyph;

typedef struct TextFaceMetrics {
    float ascender;
    float descender;
} TextFaceMetrics;

[[nodiscard]] int text_face_init(void);
void text_face_shutdown(void);

// One dynamic.faces record. Blank lines and # comments succeed and do
// nothing. A sealed record is logged and refused.
[[nodiscard]] bool text_face_apply_line(const char *line);

const char *text_face_family(uint32_t face);

// Bumps when that role is opened again. An atlas entry from an older
// epoch must not be reused.
uint32_t text_face_epoch(uint32_t face);

[[nodiscard]] bool text_face_metrics(uint32_t face, TextFaceMetrics *out);

// Rasterizes one glyph into the SDF cache. An empty bitmap (a space) is
// success with a null pointer. Glyph 0 is not drawn.
[[nodiscard]] bool text_face_glyph(uint32_t face, uint32_t glyph, TextSdf *out);

// Shapes bytes [begin, end) at the SDF pixel size. `byte` is an index into
// the full string that `utf8` points at. Advances are SDF pixels.
[[nodiscard]] bool text_face_shape(uint32_t face, const char *utf8, uint32_t begin, uint32_t end,
                                   TextShapedGlyph *out, uint32_t cap, uint32_t *count);

// The sealed 'O' must be darker in the stroke than in the corner.
[[nodiscard]] bool text_face_sdf_check(void);

#endif /* ENGINE_TEXT_FACE_H */
