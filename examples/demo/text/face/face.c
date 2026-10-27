#include "face.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb.h>

#include <stdio.h>
#include <string.h>

// The sealed bytes sit next to this file. A runtime path cannot replace them.
static const unsigned char k_sealed[] = {
#embed "bundled/InterVariable.ttf"
};

static_assert(sizeof k_sealed > 100000u, "sealed Inter face");

enum : uint32_t {
    k_sdf_slots = 2048,
    k_sdf_bytes = 4u * 1024u * 1024u,
    k_family    = 64,
    k_path      = 384,
    k_line      = 512,
};

enum : uint8_t { k_empty = 0, k_used = 1, k_tomb = 2 };

typedef struct FaceSlot {
    FT_Face    ft;
    hb_font_t *hb;
    bool       alive;
    char       family[k_family];
} FaceSlot;

typedef struct SdfSlot {
    uint32_t face;
    uint32_t glyph;
    uint32_t offset;
    uint16_t width;
    uint16_t rows;
    int16_t  left;
    int16_t  top;
    uint8_t  state;
} SdfSlot;

static FT_Library s_lib = nullptr;
static hb_buffer_t *s_buf = nullptr;
static FaceSlot s_face[TEXT_FACE_COUNT];
static uint32_t s_epoch[TEXT_FACE_COUNT];
static SdfSlot s_sdf[k_sdf_slots];
static uint8_t s_slab[k_sdf_bytes];
static uint32_t s_slab_used = 0;
static bool s_sdf_warned = false;
static bool s_ready = false;

static const char *k_role_name[TEXT_FACE_COUNT] = {
    "sealed", "system", "terminal", "known-sans", "known-mono",
};

static void copy_family(FaceSlot *slot) {
    const char *name = "unknown";
    if (slot->ft != nullptr && slot->ft->family_name != nullptr && slot->ft->family_name[0] != '\0') {
        name = slot->ft->family_name;
    }
    snprintf(slot->family, sizeof slot->family, "%s", name);
}

static void drop_face(FaceSlot *slot) {
    if (slot->hb != nullptr) {
        hb_font_destroy(slot->hb);
        slot->hb = nullptr;
    }
    if (slot->ft != nullptr) {
        FT_Done_Face(slot->ft);
        slot->ft = nullptr;
    }
    slot->alive = false;
}

static bool adopt(FaceSlot *slot, FT_Face ft) {
    drop_face(slot);
    if (FT_Set_Pixel_Sizes(ft, 0, TEXT_SDF_PX) != 0) {
        FT_Done_Face(ft);
        return false;
    }
    hb_font_t *font = hb_ft_font_create(ft, nullptr);
    if (font == nullptr) {
        FT_Done_Face(ft);
        return false;
    }
    hb_ft_font_set_load_flags(font, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT);
    slot->ft = ft;
    slot->hb = font;
    slot->alive = true;
    copy_family(slot);
    return true;
}

static bool open_memory(FaceSlot *slot) {
    FT_Face ft = nullptr;
    if (FT_New_Memory_Face(s_lib, k_sealed, (FT_Long)sizeof k_sealed, 0, &ft) != 0) {
        return false;
    }
    return adopt(slot, ft);
}

static bool open_file(FaceSlot *slot, const char *path) {
    FT_Face ft = nullptr;
    if (FT_New_Face(s_lib, path, 0, &ft) != 0) {
        return false;
    }
    return adopt(slot, ft);
}

static bool open_pattern(FaceSlot *slot, const char *pattern) {
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (pat == nullptr) {
        return false;
    }
    if (!FcConfigSubstitute(nullptr, pat, FcMatchPattern)) {
        FcPatternDestroy(pat);
        return false;
    }
    FcDefaultSubstitute(pat);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(nullptr, pat, &result);
    FcPatternDestroy(pat);
    if (match == nullptr) {
        return false;
    }
    FcChar8 *file = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || file == nullptr) {
        FcPatternDestroy(match);
        return false;
    }
    int index = 0;
    (void)FcPatternGetInteger(match, FC_INDEX, 0, &index);
    FT_Face ft = nullptr;
    FT_Error err = FT_New_Face(s_lib, (const char *)file, (FT_Long)index, &ft);
    FcPatternDestroy(match);
    if (err != 0 || ft == nullptr) {
        return false;
    }
    return adopt(slot, ft);
}

static bool open_role(uint32_t role, bool from_file, const char *value) {
    FaceSlot *slot = &s_face[role];
    bool ok = false;
    if (role == TEXT_FACE_SEALED) {
        ok = open_memory(slot);
    } else if (from_file) {
        ok = open_file(slot, value);
    } else {
        ok = open_pattern(slot, value);
    }
    if (ok) {
        s_epoch[role] += 1u;
        if (s_epoch[role] == 0u) {
            s_epoch[role] = 1u;
        }
        return true;
    }
    fprintf(stderr, "[Text] %s face failed to open. Using the embedded face.\n", k_role_name[role]);
    if (!open_memory(slot)) {
        fprintf(stderr, "[Text Error] Embedded face did not open\n");
        return false;
    }
    s_epoch[role] += 1u;
    if (s_epoch[role] == 0u) {
        s_epoch[role] = 1u;
    }
    return true;
}

static void tombstone_face(uint32_t face) {
    for (uint32_t i = 0; i < k_sdf_slots; ++i) {
        if (s_sdf[i].state == k_used && s_sdf[i].face == face) {
            s_sdf[i].state = k_tomb;
        }
    }
}

static bool skip_line(const char *line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '\0' || *line == '#';
}

bool text_face_apply_line(const char *line) {
    if (line == nullptr) {
        return false;
    }
    char buf[k_line];
    snprintf(buf, sizeof buf, "%s", line);
    char *hash = strchr(buf, '#');
    if (hash != nullptr) {
        *hash = '\0';
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }
    char *p = buf;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return true;
    }
    char *role = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p == '\0') {
        fprintf(stderr, "[Text] Face line needs a role, a kind, and a value\n");
        return false;
    }
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *kind = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p == '\0') {
        fprintf(stderr, "[Text] Face line needs a kind and a value\n");
        return false;
    }
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        fprintf(stderr, "[Text] Face line needs a value\n");
        return false;
    }
    if (strcmp(role, "sealed") == 0) {
        fprintf(stderr, "[Text] sealed face is embedded and cannot be retargeted\n");
        return false;
    }
    uint32_t id = TEXT_FACE_COUNT;
    for (uint32_t i = 1; i < TEXT_FACE_COUNT; ++i) {
        if (strcmp(role, k_role_name[i]) == 0) {
            id = i;
            break;
        }
    }
    if (id == TEXT_FACE_COUNT) {
        fprintf(stderr, "[Text] Unknown face role '%s'\n", role);
        return false;
    }
    bool from_file = false;
    if (strcmp(kind, "file") == 0) {
        from_file = true;
    } else if (strcmp(kind, "pattern") != 0) {
        fprintf(stderr, "[Text] Face kind '%s' is not pattern or file\n", kind);
        return false;
    }
    if (!s_ready) {
        fprintf(stderr, "[Text] Face line arrived before the library was up\n");
        return false;
    }
    tombstone_face(id);
    return open_role(id, from_file, p);
}

static void load_dynamic(void) {
    const char *path = "examples/demo/text/face/dynamic.faces";
    FILE *file = fopen(path, "r");
    if (file == nullptr) {
        return;
    }
    char line[k_line];
    while (fgets(line, (int)sizeof line, file) != nullptr) {
        if (skip_line(line)) {
            continue;
        }
        (void)text_face_apply_line(line);
    }
    fclose(file);
}

static void log_faces(void) {
    printf("[Text] Sealed face is %s, embedded.\n", s_face[TEXT_FACE_SEALED].family);
    printf("[Text] System face is %s.\n", s_face[TEXT_FACE_SYSTEM].family);
    printf("[Text] Terminal face is %s.\n", s_face[TEXT_FACE_TERMINAL].family);
    printf("[Text] Known sans is %s.\n", s_face[TEXT_FACE_KNOWN_SANS].family);
    printf("[Text] Known mono is %s.\n", s_face[TEXT_FACE_KNOWN_MONO].family);
    fflush(stdout);
}

int text_face_init(void) {
    if (s_ready) {
        return 0;
    }
    if (FT_Init_FreeType(&s_lib) != 0) {
        fprintf(stderr, "[Text Error] FreeType did not start\n");
        return -1;
    }
    FT_Int spread = (FT_Int)TEXT_SDF_SPREAD;
    if (FT_Property_Set(s_lib, "sdf", "spread", &spread) != 0) {
        fprintf(stderr, "[Text Error] SDF spread was not set\n");
        FT_Done_FreeType(s_lib);
        s_lib = nullptr;
        return -1;
    }
    if (!FcInit()) {
        fprintf(stderr, "[Text Error] fontconfig did not start\n");
        FT_Done_FreeType(s_lib);
        s_lib = nullptr;
        return -1;
    }
    s_buf = hb_buffer_create();
    if (s_buf == nullptr || !hb_buffer_allocation_successful(s_buf)) {
        fprintf(stderr, "[Text Error] HarfBuzz buffer was not created\n");
        text_face_shutdown();
        return -1;
    }
    if (!open_role(TEXT_FACE_SEALED, false, nullptr) ||
        !open_role(TEXT_FACE_SYSTEM, false, "sans") ||
        !open_role(TEXT_FACE_TERMINAL, false, "monospace") ||
        !open_role(TEXT_FACE_KNOWN_SANS, true, "examples/demo/text/face/bundled/InterVariable.ttf") ||
        !open_role(TEXT_FACE_KNOWN_MONO, true, "examples/demo/text/face/bundled/JetBrainsMono-Regular.ttf")) {
        text_face_shutdown();
        return -1;
    }
    s_ready = true;
    load_dynamic();
    log_faces();
    return 0;
}

void text_face_shutdown(void) {
    for (uint32_t i = 0; i < TEXT_FACE_COUNT; ++i) {
        drop_face(&s_face[i]);
        s_face[i].family[0] = '\0';
    }
    if (s_buf != nullptr) {
        hb_buffer_destroy(s_buf);
        s_buf = nullptr;
    }
    if (s_lib != nullptr) {
        FT_Done_FreeType(s_lib);
        s_lib = nullptr;
    }
    memset(s_sdf, 0, sizeof s_sdf);
    memset(s_epoch, 0, sizeof s_epoch);
    s_slab_used = 0;
    s_sdf_warned = false;
    s_ready = false;
}

const char *text_face_family(uint32_t face) {
    if (face >= TEXT_FACE_COUNT) {
        return "";
    }
    return s_face[face].family;
}

uint32_t text_face_epoch(uint32_t face) {
    if (face >= TEXT_FACE_COUNT) {
        return 0;
    }
    return s_epoch[face];
}

bool text_face_metrics(uint32_t face, TextFaceMetrics *out) {
    if (out == nullptr || face >= TEXT_FACE_COUNT || !s_face[face].alive) {
        return false;
    }
    FT_Size_Metrics *m = &s_face[face].ft->size->metrics;
    float asc = (float)m->ascender / 64.0f;
    float desc = (float)m->descender / 64.0f;
    if (desc < 0.0f) {
        desc = -desc;
    }
    if (asc < 1.0f) {
        asc = (float)TEXT_SDF_PX * 0.80f;
    }
    if (desc < 1.0f) {
        desc = (float)TEXT_SDF_PX * 0.20f;
    }
    out->ascender = asc;
    out->descender = desc;
    return true;
}

static uint32_t sdf_hash(uint32_t face, uint32_t glyph) {
    uint32_t h = face * 0x9E3779B1u ^ (glyph + 0x7F4A7C15u);
    h ^= h >> 16;
    return h;
}

static SdfSlot *sdf_find(uint32_t face, uint32_t glyph, bool insert) {
    uint32_t index = sdf_hash(face, glyph) & (k_sdf_slots - 1u);
    SdfSlot *tomb = nullptr;
    for (uint32_t n = 0; n < k_sdf_slots; ++n) {
        SdfSlot *slot = &s_sdf[index];
        if (slot->state == k_used && slot->face == face && slot->glyph == glyph) {
            return slot;
        }
        if (slot->state == k_tomb && tomb == nullptr) {
            tomb = slot;
        }
        if (slot->state == k_empty) {
            if (!insert) {
                return nullptr;
            }
            return tomb != nullptr ? tomb : slot;
        }
        index = (index + 1u) & (k_sdf_slots - 1u);
    }
    return insert ? tomb : nullptr;
}

static bool copy_bitmap(const FT_Bitmap *bitmap, uint32_t *offset, uint16_t *width, uint16_t *rows) {
    if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || bitmap->width == 0 || bitmap->rows == 0) {
        *width = 0;
        *rows = 0;
        *offset = 0;
        return true;
    }
    if (bitmap->width > 512 || bitmap->rows > 512) {
        return false;
    }
    uint32_t w = bitmap->width;
    uint32_t h = bitmap->rows;
    uint32_t bytes = w * h;
    if (bytes > k_sdf_bytes - s_slab_used) {
        if (!s_sdf_warned) {
            fprintf(stderr, "[Text Error] SDF cache is full\n");
            s_sdf_warned = true;
        }
        return false;
    }
    uint32_t pitch = bitmap->pitch < 0 ? (uint32_t)(-bitmap->pitch) : (uint32_t)bitmap->pitch;
    const uint8_t *base = bitmap->buffer;
    if (bitmap->pitch < 0) {
        base += (h - 1u) * pitch;
    }
    uint8_t *dst = s_slab + s_slab_used;
    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t *src = bitmap->pitch < 0 ? base - row * pitch : base + row * pitch;
        memcpy(dst + row * w, src, w);
    }
    *offset = s_slab_used;
    *width = (uint16_t)w;
    *rows = (uint16_t)h;
    s_slab_used += bytes;
    return true;
}

bool text_face_glyph(uint32_t face, uint32_t glyph, TextSdf *out) {
    if (out == nullptr || face >= TEXT_FACE_COUNT || !s_face[face].alive) {
        return false;
    }
    *out = (TextSdf){0};
    if (glyph == 0) {
        return true;
    }
    SdfSlot *cached = sdf_find(face, glyph, false);
    if (cached != nullptr) {
        out->bytes = cached->width == 0 ? nullptr : s_slab + cached->offset;
        out->width = cached->width;
        out->rows = cached->rows;
        out->left = cached->left;
        out->top = cached->top;
        return true;
    }
    FT_Face ft = s_face[face].ft;
    FT_Error err = FT_Load_Glyph(ft, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT);
    if (err != 0) {
        return false;
    }
    err = FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_SDF);
    if (err != 0 || ft->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        err = FT_Load_Glyph(ft, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT);
        if (err != 0) {
            return false;
        }
        err = FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL);
        if (err != 0) {
            return false;
        }
        err = FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_SDF);
        if (err != 0 || ft->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
            return false;
        }
    }
    SdfSlot *slot = sdf_find(face, glyph, true);
    if (slot == nullptr) {
        return false;
    }
    uint32_t offset = 0;
    uint16_t width = 0;
    uint16_t rows = 0;
    if (!copy_bitmap(&ft->glyph->bitmap, &offset, &width, &rows)) {
        return false;
    }
    slot->face = face;
    slot->glyph = glyph;
    slot->offset = offset;
    slot->width = width;
    slot->rows = rows;
    slot->left = (int16_t)ft->glyph->bitmap_left;
    slot->top = (int16_t)ft->glyph->bitmap_top;
    slot->state = k_used;
    out->bytes = width == 0 ? nullptr : s_slab + offset;
    out->width = width;
    out->rows = rows;
    out->left = slot->left;
    out->top = slot->top;
    return true;
}

bool text_face_shape(uint32_t face, const char *utf8, uint32_t begin, uint32_t end,
                     TextShapedGlyph *out, uint32_t cap, uint32_t *count) {
    if (count == nullptr || face >= TEXT_FACE_COUNT || !s_face[face].alive || s_buf == nullptr) {
        return false;
    }
    *count = 0;
    if (utf8 == nullptr || begin > end) {
        return false;
    }
    if (begin == end) {
        return true;
    }
    hb_buffer_clear_contents(s_buf);
    hb_buffer_add_utf8(s_buf, utf8 + begin, (int)(end - begin), 0, (int)(end - begin));
    hb_buffer_guess_segment_properties(s_buf);
    hb_shape(s_face[face].hb, s_buf, nullptr, 0);
    unsigned n = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(s_buf, &n);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(s_buf, &n);
    if (info == nullptr || pos == nullptr) {
        return false;
    }
    if (n > cap || out == nullptr) {
        return false;
    }
    for (unsigned i = 0; i < n; ++i) {
        out[i] = (TextShapedGlyph){
            .glyph     = info[i].codepoint,
            .byte      = begin + info[i].cluster,
            .x_advance = (float)pos[i].x_advance / 64.0f,
            .y_advance = (float)pos[i].y_advance / 64.0f,
            .x_offset  = (float)pos[i].x_offset / 64.0f,
            .y_offset  = (float)pos[i].y_offset / 64.0f,
        };
    }
    *count = n;
    return true;
}

bool text_face_sdf_check(void) {
    if (!s_ready || !s_face[TEXT_FACE_SEALED].alive) {
        return false;
    }
    FT_UInt glyph = FT_Get_Char_Index(s_face[TEXT_FACE_SEALED].ft, (FT_ULong)'O');
    TextSdf sdf = {0};
    if (glyph == 0 || !text_face_glyph(TEXT_FACE_SEALED, glyph, &sdf) || sdf.bytes == nullptr ||
        sdf.width < 3u || sdf.rows < 3u) {
        return false;
    }
    uint8_t corner = sdf.bytes[0];
    uint8_t peak = 0;
    bool edge = false;
    uint32_t n = sdf.width * sdf.rows;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t v = sdf.bytes[i];
        if (v > peak) {
            peak = v;
        }
        int d = (int)v - 128;
        if (d < 0) {
            d = -d;
        }
        if (d <= 12) {
            edge = true;
        }
    }
    // The counter of O is outside the stroke, so the bitmap center is not
    // the ink. The stroke is the peak. 128 is the outline.
    return peak > 160 && corner < 100 && peak > corner && edge;
}
