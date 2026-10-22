#include "chrome/layer.h"
#include <string.h>

static uint32_t s_buf_w = 0;
static uint32_t s_buf_h = 0;

typedef struct Box {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} Box;

static uint32_t edge_shape(uint32_t edge) {
    switch (edge) {
    case 1:  return 19; // top -> n_resize
    case 2:  return 22; // bottom -> s_resize
    case 4:  return 25; // left -> w_resize
    case 5:  return 21; // top_left -> nw_resize
    case 6:  return 24; // bottom_left -> sw_resize
    case 8:  return 18; // right -> e_resize
    case 9:  return 20; // top_right -> ne_resize
    case 10: return 23; // bottom_right -> se_resize
    default: return 0;
    }
}

static uint32_t scale_px(uint32_t px, uint32_t from, uint32_t to) {
    if (px == 0 || from == 0) {
        return 0;
    }
    if (from == to) {
        return px;
    }
    uint64_t v = ((uint64_t)px * (uint64_t)to) / (uint64_t)from;
    if (v == 0) {
        return 1;
    }
    if (v > 0x7fffffffu) {
        return 0x7fffffffu;
    }
    return (uint32_t)v;
}

static bool inside(const Box *b, int32_t x, int32_t y) {
    return b->w > 0 && b->h > 0 &&
           x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

static bool layer_box(const ChromeLayerSpec *spec,
                      uint32_t buf_w, uint32_t buf_h,
                      uint32_t surf_w, uint32_t surf_h,
                      Box *box) {
    if (spec->thickness == 0 || spec->thickness > buf_h ||
        buf_w == 0 || buf_h == 0 || surf_w == 0 || surf_h == 0 ||
        surf_w > 0x7fffffffu || surf_h > 0x7fffffffu) {
        return false;
    }
    uint32_t th = scale_px(spec->thickness, buf_h, surf_h);
    if (th > surf_h) {
        th = surf_h;
    }
    box->x = 0;
    box->w = (int32_t)surf_w;
    box->h = (int32_t)th;
    box->y = (strcmp(spec->edge, "bottom") == 0)
        ? (int32_t)surf_h - (int32_t)th
        : 0;
    return box->w > 0 && box->h > 0;
}

static bool control_box(const ChromeControlSpec *ctl, const Box *layer,
                        uint32_t buf_w, uint32_t surf_w, Box *box) {
    if (ctl->span == 0 || ctl->span > buf_w || layer->w <= 0) {
        return false;
    }
    uint32_t span = scale_px(ctl->span, buf_w, surf_w);
    if (span > (uint32_t)layer->w) {
        span = (uint32_t)layer->w;
    }
    box->y = layer->y;
    box->h = layer->h;
    box->w = (int32_t)span;
    box->x = (strcmp(ctl->align, "end") == 0)
        ? layer->x + layer->w - box->w
        : layer->x;
    return box->w > 0 && box->h > 0;
}

static const ChromeLayerSpec *find_layer(const ChromeSpec *spec, const char *name) {
    for (uint32_t i = 0; i < spec->nlayers; ++i) {
        if (strcmp(spec->layers[i].name, name) == 0) {
            return &spec->layers[i];
        }
    }
    return nullptr;
}

void chrome_set_buffer_size(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        s_buf_w = 0;
        s_buf_h = 0;
        return;
    }
    s_buf_w = width;
    s_buf_h = height;
}

static uint32_t sat_add(uint32_t a, uint32_t b) {
    uint32_t sum = a + b;
    return (sum < a) ? UINT32_MAX : sum;
}

void chrome_min_size(uint32_t *width, uint32_t *height) {
    uint32_t min_w = 1;
    uint32_t min_h = 1;
    const ChromeSpec *spec = chrome_spec();
    if (spec != nullptr) {
        uint32_t cross = 0;
        for (uint32_t i = 0; i < spec->nlayers; ++i) {
            cross = sat_add(cross, spec->layers[i].thickness);
        }
        if (cross > min_h) {
            min_h = cross;
        }
        for (uint32_t i = 0; i < spec->nlayers; ++i) {
            uint32_t along = 0;
            for (uint32_t c = 0; c < spec->ncontrols; ++c) {
                if (strcmp(spec->controls[c].layer, spec->layers[i].name) == 0) {
                    along = sat_add(along, spec->controls[c].span);
                }
            }
            if (along > min_w) {
                min_w = along;
            }
        }
    }
    if (width != nullptr) {
        *width = min_w;
    }
    if (height != nullptr) {
        *height = min_h;
    }
}

void chrome_clamp_size(uint32_t *width, uint32_t *height) {
    uint32_t min_w = 1;
    uint32_t min_h = 1;
    chrome_min_size(&min_w, &min_h);
    if (width != nullptr && *width > 0 && *width < min_w) {
        *width = min_w;
    }
    if (height != nullptr && *height > 0 && *height < min_h) {
        *height = min_h;
    }
}

bool chrome_paint_layer(uint32_t width, uint32_t height, uint32_t index, ChromePaintLayer *out) {
    const ChromeSpec *spec = chrome_spec();
    if (spec == nullptr || out == nullptr || index >= spec->nlayers || width == 0 || height == 0) {
        return false;
    }
    const ChromeLayerSpec *layer = &spec->layers[index];
    Box lb;
    if (!layer_box(layer, width, height, width, height, &lb)) {
        return false;
    }
    out->x = lb.x;
    out->y = lb.y;
    out->w = lb.w;
    out->h = lb.h;
    out->r = layer->color.r;
    out->g = layer->color.g;
    out->b = layer->color.b;
    out->ncontrols = 0;
    for (uint32_t c = 0; c < spec->ncontrols; ++c) {
        const ChromeControlSpec *ctl = &spec->controls[c];
        if (strcmp(ctl->layer, layer->name) != 0) {
            continue;
        }
        Box cb;
        if (!control_box(ctl, &lb, width, width, &cb)) {
            continue;
        }
        if (out->ncontrols >= ENG_CHROME_CONTROLS) {
            break;
        }
        ChromePaintControl *dst = &out->controls[out->ncontrols++];
        dst->x = cb.x;
        dst->y = cb.y;
        dst->w = cb.w;
        dst->h = cb.h;
        dst->r = ctl->color.r;
        dst->g = ctl->color.g;
        dst->b = ctl->color.b;
        dst->gr = ctl->glyph.r;
        dst->gg = ctl->glyph.g;
        dst->gb = ctl->glyph.b;
        dst->draw_glyph = strcmp(ctl->action, "close") == 0;
    }
    return true;
}

static void surface_size(const WaylandContext *w, uint32_t *surf_w, uint32_t *surf_h) {
    (void)w;
    *surf_w = s_buf_w;
    *surf_h = s_buf_h;
}

void chrome_hit_test(
    const WaylandContext *w,
    uint32_t              surface,
    int32_t               x,
    int32_t               y,
    ChromeHit            *out
) {
    const ChromeSpec *spec = chrome_spec();
    if (out == nullptr) {
        return;
    }
    out->cursor_shape = (spec != nullptr) ? spec->cursor_shape : 1u;
    out->resize_edge = 0;
    out->close = false;
    out->title = false;
    if (spec == nullptr || w == nullptr || s_buf_w == 0 || s_buf_h == 0) {
        return;
    }
    if (surface == 0 || surface != w->surface_id) {
        return;
    }

    uint32_t surf_w = 0;
    uint32_t surf_h = 0;
    surface_size(w, &surf_w, &surf_h);
    if (surf_w == 0 || surf_h == 0 || surf_w > 0x7fffffffu || surf_h > 0x7fffffffu) {
        return;
    }

    int32_t wx = x;
    int32_t wy = y;
    if (wx < 0) {
        wx = 0;
    } else if (wx >= (int32_t)surf_w) {
        wx = (int32_t)surf_w - 1;
    }
    if (wy < 0) {
        wy = 0;
    } else if (wy >= (int32_t)surf_h) {
        wy = (int32_t)surf_h - 1;
    }

    if (spec->resize && surf_w > 4u && surf_h > 4u) {
        const int32_t box = 4;
        const int32_t ww = (int32_t)surf_w;
        const int32_t wh = (int32_t)surf_h;
        bool cleft   = wx < box;
        bool cright  = wx >= ww - box;
        bool ctop    = wy < box;
        bool cbottom = wy >= wh - box;
        uint32_t corner = 0;
        if (ctop && cleft) {
            corner = 5;
        } else if (ctop && cright) {
            corner = 9;
        } else if (cbottom && cleft) {
            corner = 6;
        } else if (cbottom && cright) {
            corner = 10;
        }
        if (corner != 0) {
            out->resize_edge = corner;
            uint32_t shape = edge_shape(corner);
            if (shape != 0) {
                out->cursor_shape = shape;
            }
            return;
        }
    }

    for (uint32_t n = spec->ncontrols; n > 0; --n) {
        const ChromeControlSpec *ctl = &spec->controls[n - 1u];
        const ChromeLayerSpec *layer = find_layer(spec, ctl->layer);
        if (layer == nullptr) {
            continue;
        }
        Box lb;
        Box cb;
        if (!layer_box(layer, s_buf_w, s_buf_h, surf_w, surf_h, &lb) ||
            !control_box(ctl, &lb, s_buf_w, surf_w, &cb) ||
            !inside(&cb, wx, wy)) {
            continue;
        }
        out->close = strcmp(ctl->action, "close") == 0;
        if (ctl->has_cursor) {
            out->cursor_shape = ctl->cursor_shape;
        }
        return;
    }

    if (spec->resize) {
        uint32_t band = spec->resize_band;
        bool left   = (uint32_t)wx < band;
        bool right  = wx >= (int32_t)surf_w - (int32_t)band;
        bool top    = (uint32_t)wy < band;
        bool bottom = wy >= (int32_t)surf_h - (int32_t)band;
        uint32_t edge = 0;
        if (top && left) {
            edge = 5;
        } else if (top && right) {
            edge = 9;
        } else if (bottom && left) {
            edge = 6;
        } else if (bottom && right) {
            edge = 10;
        } else if (top) {
            edge = 1;
        } else if (bottom) {
            edge = 2;
        } else if (left) {
            edge = 4;
        } else if (right) {
            edge = 8;
        }
        if (edge != 0) {
            out->resize_edge = edge;
            uint32_t shape = edge_shape(edge);
            if (shape != 0) {
                out->cursor_shape = shape;
            }
            return;
        }
    }

    for (uint32_t i = 0; i < spec->nlayers; ++i) {
        const ChromeLayerSpec *layer = &spec->layers[i];
        if (strcmp(layer->edge, "top") != 0) {
            continue;
        }
        Box lb;
        if (!layer_box(layer, s_buf_w, s_buf_h, surf_w, surf_h, &lb)) {
            continue;
        }
        if (inside(&lb, wx, wy)) {
            out->title = true;
            return;
        }
    }
}
