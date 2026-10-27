#include "shape.h"

#include <math.h>
#include <string.h>

enum : uint32_t { k_span = 512 };

static TextShapedGlyph s_span[k_span];

static float pad_of(float px) {
    float pad = px * 0.20f;
    if (pad < 2.0f) {
        pad = 2.0f;
    }
    return pad;
}

static bool is_break(const char *utf8, uint32_t n, uint32_t byte) {
    return byte < n && (utf8[byte] == ' ' || utf8[byte] == '\t');
}

static uint32_t utf8_step(const char *utf8, uint32_t n, uint32_t i) {
    if (i >= n) {
        return n;
    }
    i++;
    while (i < n && ((unsigned char)utf8[i] & 0xC0u) == 0x80u) {
        i++;
    }
    return i;
}

static uint32_t emit_span(uint32_t face, const TextShapedGlyph *glyphs, uint32_t count,
                          uint32_t begin, uint32_t end, uint32_t caret, float baseline,
                          float scale, float pad, float asc, float desc, float box_h,
                          TextGlyphQuad *out, uint32_t cap, uint32_t written,
                          float *caret_x, float *caret_y, bool *caret_set) {
    float pen_x = 0.0f;
    float pen_y = 0.0f;
    float min_x = 0.0f;
    float pens_x[k_span];
    float pens_y[k_span];
    for (uint32_t i = 0; i < count; ++i) {
        if (pen_x < min_x) {
            min_x = pen_x;
        }
        // HarfBuzz advances are at the SDF pixel size. Draw size is a scale.
        pens_x[i] = pen_x + glyphs[i].x_offset * scale;
        pens_y[i] = pen_y + glyphs[i].y_offset * scale;
        pen_x += glyphs[i].x_advance * scale;
        pen_y += glyphs[i].y_advance * scale;
        if (pen_x < min_x) {
            min_x = pen_x;
        }
    }
    float shift = pad - min_x;
    float end_x = pen_x + shift;
    bool draw = (baseline + asc) > -box_h;
    for (uint32_t i = 0; i < count; ++i) {
        float glyph_pen_x = (pens_x[i] - glyphs[i].x_offset * scale) + shift;
        float glyph_pen_y = baseline + (pens_y[i] - glyphs[i].y_offset * scale);
        if (!*caret_set && glyphs[i].byte >= caret) {
            *caret_x = glyph_pen_x;
            *caret_y = glyph_pen_y;
            *caret_set = true;
        }
        if (!draw || written >= cap) {
            continue;
        }
        TextGlyphQuad quad = {
            .pen_x = glyph_pen_x,
            .pen_y = glyph_pen_y,
            .glyph = glyphs[i].glyph,
            .byte  = glyphs[i].byte,
        };
        TextSdf sdf = {0};
        if (glyphs[i].glyph != 0 && text_face_glyph(face, glyphs[i].glyph, &sdf) &&
            sdf.bytes != nullptr && sdf.width > 0u && sdf.rows > 0u) {
            float left = (pens_x[i] + shift) + (float)sdf.left * scale;
            float top = (baseline + pens_y[i]) + (float)sdf.top * scale;
            quad.x0 = left;
            quad.y1 = top;
            quad.x1 = left + (float)sdf.width * scale;
            quad.y0 = top - (float)sdf.rows * scale;
            quad.visible = true;
        }
        out[written++] = quad;
    }
    if (!*caret_set && caret >= begin && caret <= end) {
        *caret_x = end_x;
        *caret_y = baseline;
        *caret_set = true;
    }
    (void)desc;
    return written;
}

// Places one logical line, wrapping onto extra baselines. `baseline` advances.
static uint32_t layout_line(uint32_t face, const char *utf8, uint32_t begin, uint32_t end,
                            uint32_t caret, float *baseline, float scale, float pad, float inner,
                            float asc, float desc, float line_h, float box_h,
                            TextGlyphQuad *out, uint32_t cap, uint32_t written,
                            float *caret_x, float *caret_y, bool *caret_set) {
    if (begin == end) {
        if (!*caret_set && caret == begin) {
            *caret_x = pad;
            *caret_y = *baseline;
            *caret_set = true;
        }
        *baseline -= line_h;
        return written;
    }
    uint32_t cursor = begin;
    while (cursor < end && written < cap) {
        uint32_t count = 0;
        if (!text_face_shape(face, utf8, cursor, end, s_span, k_span, &count) || count == 0u) {
            cursor = utf8_step(utf8, end, cursor);
            *baseline -= line_h;
            continue;
        }
        float pen = 0.0f;
        uint32_t cut = count;
        uint32_t resume = end;
        uint32_t prefix = end;
        for (uint32_t g = 0; g < count; ++g) {
            float next = pen + s_span[g].x_advance * scale;
            if (g > 0u && fabsf(next) > inner) {
                uint32_t space = count;
                for (uint32_t s = g; s > 0u; --s) {
                    if (is_break(utf8, end, s_span[s - 1u].byte)) {
                        space = s - 1u;
                        break;
                    }
                }
                if (space < count) {
                    prefix = s_span[space].byte;
                    resume = (space + 1u < count) ? s_span[space + 1u].byte : end;
                    cut = space;
                } else {
                    prefix = s_span[g].byte;
                    resume = prefix;
                    cut = g;
                }
                if (resume <= cursor) {
                    resume = utf8_step(utf8, end, cursor);
                    prefix = resume;
                    cut = count;
                }
                break;
            }
            pen = next;
        }
        bool placed = false;
        if (cut == count) {
            written = emit_span(face, s_span, count, cursor, end, caret, *baseline, scale, pad,
                                asc, desc, box_h, out, cap, written, caret_x, caret_y, caret_set);
            cursor = end;
            placed = true;
        } else if (prefix > cursor) {
            uint32_t prefix_count = 0;
            if (!text_face_shape(face, utf8, cursor, prefix, s_span, k_span, &prefix_count)) {
                cursor = resume;
            } else {
                written = emit_span(face, s_span, prefix_count, cursor, prefix, caret, *baseline,
                                    scale, pad, asc, desc, box_h, out, cap, written, caret_x,
                                    caret_y, caret_set);
                cursor = resume;
                placed = true;
            }
        } else {
            cursor = resume;
        }
        if (placed) {
            *baseline -= line_h;
        }
    }
    return written;
}

uint32_t text_shape_box(uint32_t face, const char *utf8, uint32_t nbytes,
                        float box_w, float box_h, float px, uint32_t caret,
                        TextGlyphQuad *out, uint32_t cap,
                        float *caret_x, float *caret_y,
                        float *ascender, float *descender) {
    if (caret_x != nullptr) {
        *caret_x = 0.0f;
    }
    if (caret_y != nullptr) {
        *caret_y = 0.0f;
    }
    TextFaceMetrics metrics = {0};
    if (out == nullptr || cap == 0u || caret_x == nullptr || caret_y == nullptr ||
        ascender == nullptr || descender == nullptr || px < 1.0f || box_w < 1.0f || box_h < 1.0f ||
        !text_face_metrics(face, &metrics)) {
        return 0;
    }
    if (utf8 == nullptr) {
        utf8 = "";
        nbytes = 0;
    }
    if (caret > nbytes) {
        caret = nbytes;
    }
    float scale = px / (float)TEXT_SDF_PX;
    float asc = metrics.ascender * scale;
    float desc = metrics.descender * scale;
    float pad = pad_of(px);
    float line_h = px * 1.35f;
    float inner = box_w - pad * 2.0f;
    if (inner < 1.0f) {
        inner = 1.0f;
    }
    *ascender = asc;
    *descender = desc;
    float baseline = -(pad + asc);
    bool caret_set = false;
    uint32_t written = 0;
    if (nbytes == 0u) {
        *caret_x = pad;
        *caret_y = baseline;
        return 0;
    }
    uint32_t i = 0;
    while (i < nbytes && written < cap) {
        uint32_t end = i;
        while (end < nbytes && utf8[end] != '\n') {
            end++;
        }
        written = layout_line(face, utf8, i, end, caret, &baseline, scale, pad, inner, asc, desc,
                              line_h, box_h, out, cap, written, caret_x, caret_y, &caret_set);
        if (end >= nbytes) {
            break;
        }
        i = end + 1u;
        // A trailing newline leaves the caret on the blank line layout_line
        // already stepped to.
        if (i == nbytes && !caret_set && caret == nbytes) {
            *caret_x = pad;
            *caret_y = baseline;
            caret_set = true;
        }
    }
    if (!caret_set) {
        *caret_x = pad;
        *caret_y = -(pad + asc);
    }
    return written;
}

float text_shape_advance(uint32_t face, const char *utf8, float px) {
    if (utf8 == nullptr || utf8[0] == '\0' || px < 1.0f) {
        return 0.0f;
    }
    TextGlyphQuad quads[64];
    float caret_x = 0.0f;
    float caret_y = 0.0f;
    float asc = 0.0f;
    float desc = 0.0f;
    uint32_t n = (uint32_t)strlen(utf8);
    (void)text_shape_box(face, utf8, n, 10000.0f, 800.0f, px, n, quads, 64,
                         &caret_x, &caret_y, &asc, &desc);
    float pad = pad_of(px);
    float adv = caret_x - pad;
    if (adv < 0.0f) {
        return 0.0f;
    }
    return adv;
}
