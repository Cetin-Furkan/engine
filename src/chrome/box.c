#include "chrome/box.h"
#include <stdio.h>
#include <string.h>

static uint32_t scale_px(uint32_t px, uint32_t from, uint32_t to) {
    if (px == 0u || from == 0u) {
        return 0u;
    }
    if (from == to) {
        return px;
    }
    uint64_t v = ((uint64_t)px * (uint64_t)to) / (uint64_t)from;
    if (v == 0u) {
        return 1u;
    }
    if (v > 0x7fffffffu) {
        return 0x7fffffffu;
    }
    return (uint32_t)v;
}

static BoxDock dock_of_edge(const char *edge) {
    if (strcmp(edge, "bottom") == 0) {
        return BOX_DOCK_BOTTOM;
    }
    return BOX_DOCK_TOP;
}

static void put_text(char *dst, uint32_t cap, const char *src) {
    memset(dst, 0, cap);
    uint32_t n = (uint32_t)strlen(src);
    if (n >= cap) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
}

static bool same_rect(const BoxRect *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    return r->x == x && r->y == y && r->w == w && r->h == h;
}

static const BoxNode *find_dock(const BoxNode *nodes, uint32_t count, BoxDock dock, const char *name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].dock == dock && strcmp(nodes[i].name, name) == 0) {
            return &nodes[i];
        }
    }
    return nullptr;
}

static bool layout_self_check(void);

uint32_t box_layout(const ChromeSpec *spec,
                    uint32_t buf_w, uint32_t buf_h,
                    uint32_t surf_w, uint32_t surf_h,
                    BoxNode *nodes, uint32_t cap,
                    BoxRect *client) {
    static bool s_checked = false;
    static bool s_in_check = false;
    if (!s_checked && !s_in_check) {
        s_in_check = true;
        s_checked = true;
        if (!layout_self_check()) {
            printf("[Chrome Error] Box layout self-check failed\n");
            fflush(stdout);
        }
        s_in_check = false;
    }
    if (nodes == nullptr || cap == 0u) {
        return 0u;
    }
    nodes[0] = (BoxNode){
        .name        = "window",
        .dock        = BOX_DOCK_WINDOW,
        .rect        = { 0, 0, (int32_t)surf_w, (int32_t)surf_h },
        .parent      = (uint16_t)BOX_NONE,
        .child_count = 0,
    };
    uint32_t n = 1u;
    if (spec == nullptr || surf_w == 0u || surf_h == 0u) {
        if (client != nullptr) {
            *client = nodes[0].rect;
        }
        return n;
    }
    bool place = buf_w != 0u && buf_h != 0u &&
                 surf_w <= 0x7fffffffu && surf_h <= 0x7fffffffu;
    int32_t edge_lo = 0;
    int32_t edge_hi = place ? (int32_t)surf_h : 0;
    uint16_t layer_node[ENG_CHROME_LAYERS];
    int32_t start_x[ENG_CHROME_LAYERS];
    int32_t end_x[ENG_CHROME_LAYERS];
    for (uint32_t i = 0; i < ENG_CHROME_LAYERS; ++i) {
        layer_node[i] = (uint16_t)BOX_NONE;
        start_x[i] = 0;
        end_x[i] = 0;
    }
    for (uint32_t i = 0; i < spec->nlayers && n < cap; ++i) {
        const ChromeLayerSpec *ls = &spec->layers[i];
        int32_t room = edge_hi - edge_lo;
        if (!place || ls->thickness == 0u || ls->thickness > buf_h || room <= 0) {
            continue;
        }
        int32_t th = (int32_t)scale_px(ls->thickness, buf_h, surf_h);
        if (th > room) {
            th = room;
        }
        if (th <= 0) {
            continue;
        }
        BoxDock dock = dock_of_edge(ls->edge);
        BoxRect rect = {
            .x = 0,
            .w = (int32_t)surf_w,
            .h = th,
        };
        if (dock == BOX_DOCK_BOTTOM) {
            edge_hi -= th;
            rect.y = edge_hi;
        } else {
            rect.y = edge_lo;
            edge_lo += th;
        }
        BoxNode *node = &nodes[n];
        *node = (BoxNode){
            .dock        = dock,
            .rect        = rect,
            .parent      = 0,
            .child_count = 0,
        };
        memcpy(node->name, ls->name, ENG_CHROME_NAME);
        layer_node[i] = (uint16_t)n;
        start_x[i] = rect.x;
        end_x[i] = rect.x + rect.w;
        nodes[0].child_count++;
        n++;
    }
    for (uint32_t c = 0; c < spec->ncontrols && n < cap; ++c) {
        const ChromeControlSpec *ctl = &spec->controls[c];
        uint32_t layer_i = ENG_CHROME_LAYERS;
        for (uint32_t i = 0; i < spec->nlayers; ++i) {
            if (strcmp(spec->layers[i].name, ctl->layer) == 0) {
                layer_i = i;
                break;
            }
        }
        if (layer_i >= spec->nlayers || layer_node[layer_i] == (uint16_t)BOX_NONE) {
            continue;
        }
        int32_t avail = end_x[layer_i] - start_x[layer_i];
        if (ctl->span == 0u || ctl->span > buf_w || avail <= 0) {
            continue;
        }
        uint32_t raw = scale_px(ctl->span, buf_w, surf_w);
        if (raw == 0u) {
            continue;
        }
        int32_t span = (int32_t)raw;
        if (span > avail) {
            span = avail;
        }
        BoxNode *parent = &nodes[layer_node[layer_i]];
        BoxRect rect = {
            .y = parent->rect.y,
            .w = span,
            .h = parent->rect.h,
        };
        if (strcmp(ctl->align, "end") == 0) {
            end_x[layer_i] -= span;
            rect.x = end_x[layer_i];
        } else {
            rect.x = start_x[layer_i];
            start_x[layer_i] += span;
        }
        BoxNode *node = &nodes[n];
        *node = (BoxNode){
            .dock        = BOX_DOCK_CONTROL,
            .rect        = rect,
            .parent      = layer_node[layer_i],
            .child_count = 0,
        };
        memcpy(node->name, ctl->name, ENG_CHROME_NAME);
        parent->child_count++;
        n++;
    }
    BoxRect client_rect = nodes[0].rect;
    if (place) {
        int32_t client_h = edge_hi - edge_lo;
        if (client_h < 0) {
            client_h = 0;
        }
        client_rect = (BoxRect){ 0, edge_lo, (int32_t)surf_w, client_h };
    }
    if (n < cap) {
        nodes[n] = (BoxNode){
            .name        = "client",
            .dock        = BOX_DOCK_CLIENT,
            .rect        = client_rect,
            .parent      = 0,
            .child_count = 0,
        };
        nodes[0].child_count++;
        n++;
    }
    if (client != nullptr) {
        *client = client_rect;
    }
    static bool s_logged = false;
    if (!s_logged && !s_in_check && surf_w > 1u && surf_h > 1u && spec->nlayers > 0u) {
        s_logged = true;
        printf("[Chrome] Box tree: %u nodes, client %dx%d at %d,%d\n",
               n, client_rect.w, client_rect.h, client_rect.x, client_rect.y);
        fflush(stdout);
    }
    return n;
}

static bool expect_node(const BoxNode *node, int32_t x, int32_t y, int32_t w, int32_t h) {
    return node != nullptr && same_rect(&node->rect, x, y, w, h);
}

static bool layout_self_check(void) {
    ChromeSpec spec = {0};
    BoxNode nodes[BOX_CAP];
    BoxRect client = {0};

    spec.nlayers = 1;
    put_text(spec.layers[0].name, ENG_CHROME_NAME, "bar");
    put_text(spec.layers[0].edge, sizeof spec.layers[0].edge, "top");
    spec.layers[0].thickness = 32;
    spec.ncontrols = 1;
    put_text(spec.controls[0].name, ENG_CHROME_NAME, "close");
    put_text(spec.controls[0].layer, ENG_CHROME_NAME, "bar");
    put_text(spec.controls[0].align, sizeof spec.controls[0].align, "end");
    spec.controls[0].span = 32;
    uint32_t n = box_layout(&spec, 1280, 720, 1280, 720, nodes, BOX_CAP, &client);
    if (!expect_node(find_dock(nodes, n, BOX_DOCK_TOP, "bar"), 0, 0, 1280, 32) ||
        !expect_node(find_dock(nodes, n, BOX_DOCK_CONTROL, "close"), 1248, 0, 32, 32) ||
        !same_rect(&client, 0, 32, 1280, 688)) {
        return false;
    }

    spec = (ChromeSpec){0};
    spec.nlayers = 2;
    put_text(spec.layers[0].name, ENG_CHROME_NAME, "title");
    put_text(spec.layers[0].edge, sizeof spec.layers[0].edge, "top");
    spec.layers[0].thickness = 32;
    put_text(spec.layers[1].name, ENG_CHROME_NAME, "tools");
    put_text(spec.layers[1].edge, sizeof spec.layers[1].edge, "top");
    spec.layers[1].thickness = 24;
    n = box_layout(&spec, 1280, 720, 1280, 720, nodes, BOX_CAP, &client);
    if (!expect_node(find_dock(nodes, n, BOX_DOCK_TOP, "title"), 0, 0, 1280, 32) ||
        !expect_node(find_dock(nodes, n, BOX_DOCK_TOP, "tools"), 0, 32, 1280, 24) ||
        !same_rect(&client, 0, 56, 1280, 664)) {
        return false;
    }

    spec = (ChromeSpec){0};
    spec.nlayers = 2;
    put_text(spec.layers[0].name, ENG_CHROME_NAME, "status");
    put_text(spec.layers[0].edge, sizeof spec.layers[0].edge, "bottom");
    spec.layers[0].thickness = 20;
    put_text(spec.layers[1].name, ENG_CHROME_NAME, "hint");
    put_text(spec.layers[1].edge, sizeof spec.layers[1].edge, "bottom");
    spec.layers[1].thickness = 10;
    n = box_layout(&spec, 1280, 720, 1280, 720, nodes, BOX_CAP, &client);
    if (!expect_node(find_dock(nodes, n, BOX_DOCK_BOTTOM, "status"), 0, 700, 1280, 20) ||
        !expect_node(find_dock(nodes, n, BOX_DOCK_BOTTOM, "hint"), 0, 690, 1280, 10) ||
        !same_rect(&client, 0, 0, 1280, 690)) {
        return false;
    }

    spec = (ChromeSpec){0};
    spec.nlayers = 1;
    put_text(spec.layers[0].name, ENG_CHROME_NAME, "bar");
    put_text(spec.layers[0].edge, sizeof spec.layers[0].edge, "top");
    spec.layers[0].thickness = 32;
    spec.ncontrols = 3;
    put_text(spec.controls[0].name, ENG_CHROME_NAME, "a");
    put_text(spec.controls[0].layer, ENG_CHROME_NAME, "bar");
    put_text(spec.controls[0].align, sizeof spec.controls[0].align, "end");
    spec.controls[0].span = 32;
    put_text(spec.controls[1].name, ENG_CHROME_NAME, "b");
    put_text(spec.controls[1].layer, ENG_CHROME_NAME, "bar");
    put_text(spec.controls[1].align, sizeof spec.controls[1].align, "end");
    spec.controls[1].span = 40;
    put_text(spec.controls[2].name, ENG_CHROME_NAME, "c");
    put_text(spec.controls[2].layer, ENG_CHROME_NAME, "bar");
    put_text(spec.controls[2].align, sizeof spec.controls[2].align, "start");
    spec.controls[2].span = 16;
    n = box_layout(&spec, 1280, 720, 1280, 720, nodes, BOX_CAP, &client);
    if (!expect_node(find_dock(nodes, n, BOX_DOCK_CONTROL, "a"), 1248, 0, 32, 32) ||
        !expect_node(find_dock(nodes, n, BOX_DOCK_CONTROL, "b"), 1208, 0, 40, 32) ||
        !expect_node(find_dock(nodes, n, BOX_DOCK_CONTROL, "c"), 0, 0, 16, 32)) {
        return false;
    }

    spec.controls[0].span = 80;
    put_text(spec.controls[0].align, sizeof spec.controls[0].align, "start");
    spec.controls[1].span = 80;
    put_text(spec.controls[1].align, sizeof spec.controls[1].align, "start");
    spec.ncontrols = 2;
    n = box_layout(&spec, 100, 40, 100, 40, nodes, BOX_CAP, &client);
    if (!expect_node(find_dock(nodes, n, BOX_DOCK_CONTROL, "a"), 0, 0, 80, 32) ||
        !expect_node(find_dock(nodes, n, BOX_DOCK_CONTROL, "b"), 80, 0, 20, 32)) {
        return false;
    }
    return true;
}

bool box_layer_rect(const ChromeSpec *spec, uint32_t layer_index,
                    uint32_t buf_w, uint32_t buf_h,
                    uint32_t surf_w, uint32_t surf_h,
                    BoxRect *out) {
    if (spec == nullptr || out == nullptr || layer_index >= spec->nlayers) {
        return false;
    }
    BoxNode nodes[BOX_CAP];
    uint32_t n = box_layout(spec, buf_w, buf_h, surf_w, surf_h, nodes, BOX_CAP, nullptr);
    const BoxNode *node = find_dock(nodes, n, dock_of_edge(spec->layers[layer_index].edge),
                                    spec->layers[layer_index].name);
    if (node == nullptr) {
        return false;
    }
    *out = node->rect;
    return true;
}

bool box_control_rect(const ChromeSpec *spec, uint32_t control_index,
                      uint32_t buf_w, uint32_t buf_h,
                      uint32_t surf_w, uint32_t surf_h,
                      BoxRect *out) {
    if (spec == nullptr || out == nullptr || control_index >= spec->ncontrols) {
        return false;
    }
    BoxNode nodes[BOX_CAP];
    uint32_t n = box_layout(spec, buf_w, buf_h, surf_w, surf_h, nodes, BOX_CAP, nullptr);
    const BoxNode *node = find_dock(nodes, n, BOX_DOCK_CONTROL, spec->controls[control_index].name);
    if (node == nullptr) {
        return false;
    }
    *out = node->rect;
    return true;
}
