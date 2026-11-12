#include "chrome/layer.h"
#include "chrome/box.h"
#include <string.h>

static uint32_t s_buf_w = 0;
static uint32_t s_buf_h = 0;

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

static bool inside(const BoxRect *b, int32_t x, int32_t y) {
    return b->w > 0 && b->h > 0 &&
           x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

static const BoxNode *find_named(const BoxNode *nodes, uint32_t count, BoxDock dock, const char *name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].dock == dock && strcmp(nodes[i].name, name) == 0) {
            return &nodes[i];
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

void chrome_band_thickness(uint32_t *top, uint32_t *bottom) {
    uint32_t t = 0;
    uint32_t b = 0;
    const ChromeSpec *spec = chrome_spec();
    if (spec != nullptr) {
        for (uint32_t i = 0; i < spec->nlayers; ++i) {
            uint32_t thick = spec->layers[i].thickness;
            if (strcmp(spec->layers[i].edge, "bottom") == 0) {
                b = sat_add(b, thick);
            } else {
                t = sat_add(t, thick);
            }
        }
    }
    if (top != nullptr) {
        *top = t;
    }
    if (bottom != nullptr) {
        *bottom = b;
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
    BoxNode nodes[BOX_CAP];
    uint32_t n = box_layout(spec, width, height, width, height, nodes, BOX_CAP, nullptr);
    BoxDock layer_dock = strcmp(layer->edge, "bottom") == 0 ? BOX_DOCK_BOTTOM : BOX_DOCK_TOP;
    const BoxNode *lb = find_named(nodes, n, layer_dock, layer->name);
    if (lb == nullptr) {
        return false;
    }
    out->x = lb->rect.x;
    out->y = lb->rect.y;
    out->w = lb->rect.w;
    out->h = lb->rect.h;
    out->r = layer->color.r;
    out->g = layer->color.g;
    out->b = layer->color.b;
    out->ncontrols = 0;
    for (uint32_t c = 0; c < spec->ncontrols; ++c) {
        const ChromeControlSpec *ctl = &spec->controls[c];
        if (strcmp(ctl->layer, layer->name) != 0) {
            continue;
        }
        const BoxNode *cb = find_named(nodes, n, BOX_DOCK_CONTROL, ctl->name);
        if (cb == nullptr) {
            continue;
        }
        if (out->ncontrols >= ENG_CHROME_CONTROLS) {
            break;
        }
        ChromePaintControl *dst = &out->controls[out->ncontrols++];
        dst->x = cb->rect.x;
        dst->y = cb->rect.y;
        dst->w = cb->rect.w;
        dst->h = cb->rect.h;
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
    if (w != nullptr && w->geom_w != 0 && w->geom_h != 0) {
        *surf_w = w->geom_w;
        *surf_h = w->geom_h;
        return;
    }
    *surf_w = s_buf_w;
    *surf_h = (w != nullptr && w->bar_surface_id != 0) ? sat_add(s_buf_h, w->bar_px) : s_buf_h;
}

static bool control_layer_is_rendered(const ChromeSpec *spec, const WaylandContext *w, const char *layer_name) {
    if (spec == nullptr || w == nullptr || w->bar_surface_id == 0 || layer_name == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < spec->nlayers; ++i) {
        if (strcmp(spec->layers[i].name, layer_name) == 0) {
            return strcmp(spec->layers[i].edge, "top") == 0;
        }
    }
    return false;
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
    // When the compositor forces server-side decorations, suppress client-side chrome hitboxes.
    if (w->decoration_mode == ENG_DECOR_SERVER) {
        return;
    }
    bool on_bar = w->bar_surface_id != 0 && surface == w->bar_surface_id;
    bool on_main = surface == w->surface_id;
    if (surface == 0 || (!on_bar && !on_main)) {
        return;
    }
    // The bar surface is window-local at the top. The scene surface starts
    // under that band, so its pointer y is short by the band.
    if (on_main && w->bar_px > 0) {
        y += (int32_t)w->bar_px;
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

    uint32_t locked = ENG_TOP_MAXIMIZED | ENG_TOP_FULLSCREEN | ENG_TOP_TILED_LEFT |
                      ENG_TOP_TILED_RIGHT | ENG_TOP_TILED_TOP | ENG_TOP_TILED_BOTTOM;
    bool edges = spec->resize && (w->toplevel_states & locked) == 0;
    if (edges && surf_w > 4u && surf_h > 4u) {
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

    BoxNode nodes[BOX_CAP];
    uint32_t node_count = box_layout(spec, surf_w, surf_h, surf_w, surf_h, nodes, BOX_CAP, nullptr);
    for (uint32_t n = spec->ncontrols; n > 0; --n) {
        const ChromeControlSpec *ctl = &spec->controls[n - 1u];
        if (!control_layer_is_rendered(spec, w, ctl->layer)) {
            continue;
        }
        const BoxNode *cb = find_named(nodes, node_count, BOX_DOCK_CONTROL, ctl->name);
        if (cb == nullptr || !inside(&cb->rect, wx, wy)) {
            continue;
        }
        out->close = strcmp(ctl->action, "close") == 0;
        if (ctl->has_cursor) {
            out->cursor_shape = ctl->cursor_shape;
        }
        return;
    }

    if (edges) {
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
        const BoxNode *lb = find_named(nodes, node_count, BOX_DOCK_TOP, layer->name);
        if (lb == nullptr) {
            continue;
        }
        if (inside(&lb->rect, wx, wy)) {
            out->title = true;
            return;
        }
    }
}