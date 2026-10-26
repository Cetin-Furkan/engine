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

    BoxNode nodes[BOX_CAP];
    uint32_t node_count = box_layout(spec, s_buf_w, s_buf_h, surf_w, surf_h, nodes, BOX_CAP, nullptr);
    for (uint32_t n = spec->ncontrols; n > 0; --n) {
        const ChromeControlSpec *ctl = &spec->controls[n - 1u];
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
