#include "text/text_int.h"

#include "text/atlas/atlas.h"
#include "text/shape/shape.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

enum : uint32_t {
    k_runs   = 48,
    k_bytes  = 480,
    k_glyphs = 512,
};

typedef struct Run {
    bool     used;
    bool     sealed;
    uint32_t gen;
    uint32_t face;
    float    x0, y0, x1, y1;
    float    r, g, b, a;
    float    px;
    uint32_t caret;
    uint32_t nbytes;
    uint32_t hash;
    char     utf8[k_bytes + 1u];
    TextGlyphQuad cache[k_glyphs];
    uint32_t cache_n;
    float    cache_w, cache_h, cache_px;
    uint32_t cache_face, cache_epoch, cache_hash, cache_caret;
    float    caret_x, caret_y, asc, desc;
    bool     cache_ok;
} Run;

typedef struct ShownRun {
    bool     used;
    uint32_t gen;
    uint32_t face;
    float    x0, y0, x1, y1;
    float    r, g, b, a;
    float    px;
    uint32_t caret;
    uint32_t nbytes;
    char     utf8[k_bytes + 1u];
} ShownRun;

static Run s_live[k_runs];
static ShownRun s_shown[k_runs];
static bool s_editing = false;
static uint32_t s_edit = 0;
static bool s_anchor = false;
static float s_ax = 0.0f, s_ay = 0.0f, s_bx = 0.0f, s_by = 0.0f;
static bool s_shown_editing = false;
static uint32_t s_shown_edit = 0;
static bool s_shown_anchor = false;
static float s_shown_ax = 0.0f, s_shown_ay = 0.0f, s_shown_bx = 0.0f, s_shown_by = 0.0f;
static uint32_t s_fb_w = 0;
static uint32_t s_fb_h = 0;
static bool s_quad_warned = false;

static uint32_t hash_bytes(const char *s, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t bump_gen(uint32_t gen) {
    gen++;
    if (gen == 0u || gen > 0x03FFFFFFu) {
        gen = 1u;
    }
    return gen;
}

static void clear_run(Run *run, uint32_t gen) {
    memset(run, 0, sizeof *run);
    run->gen = gen;
}

static void order_rect(float *x0, float *y0, float *x1, float *y1) {
    if (*x0 > *x1) {
        float t = *x0;
        *x0 = *x1;
        *x1 = t;
    }
    if (*y0 > *y1) {
        float t = *y0;
        *y0 = *y1;
        *y1 = t;
    }
}

static float px_for_height(float height) {
    float px = 18.0f;
    float fit = height * 0.72f;
    if (px > fit) {
        px = fit;
    }
    if (px < 10.0f) {
        px = 10.0f;
    }
    return px;
}

static bool copy_text(Run *run, const char *utf8) {
    if (utf8 == nullptr) {
        utf8 = "";
    }
    uint32_t n = (uint32_t)strlen(utf8);
    if (n > k_bytes) {
        fprintf(stderr, "[Text] A run holds %u bytes\n", k_bytes);
        return false;
    }
    memcpy(run->utf8, utf8, n);
    run->utf8[n] = '\0';
    run->nbytes = n;
    run->hash = hash_bytes(run->utf8, n);
    if (run->caret > n) {
        run->caret = n;
    }
    run->cache_ok = false;
    return true;
}

static int user_slot(uint32_t id) {
    uint32_t slot = id & 63u;
    uint32_t gen = id >> 6;
    if (id == 0u || slot == 0u || slot >= k_runs) {
        return -1;
    }
    if (!s_live[slot].used || s_live[slot].sealed || s_live[slot].gen != gen) {
        return -1;
    }
    return (int)slot;
}

static uint32_t alloc_run(float x0, float y0, float x1, float y1, const char *utf8, bool edit) {
    order_rect(&x0, &y0, &x1, &y1);
    if ((x1 - x0) < 8.0f && (y1 - y0) < 8.0f) {
        x1 = x0 + 280.0f;
        y0 = y1 - 72.0f;
    }
    if (utf8 != nullptr && strlen(utf8) > k_bytes) {
        fprintf(stderr, "[Text] A run holds %u bytes\n", k_bytes);
        return 0;
    }
    for (uint32_t slot = 1; slot < k_runs; ++slot) {
        if (s_live[slot].used) {
            continue;
        }
        uint32_t gen = s_live[slot].gen;
        if (gen == 0u) {
            gen = 1u;
        }
        clear_run(&s_live[slot], gen);
        Run *run = &s_live[slot];
        run->used = true;
        run->face = TEXT_FACE_SYSTEM;
        run->x0 = x0;
        run->y0 = y0;
        run->x1 = x1;
        run->y1 = y1;
        run->px = px_for_height(y1 - y0);
        run->r = 0.96f;
        run->g = 0.97f;
        run->b = 0.98f;
        run->a = 1.0f;
        if (!copy_text(run, utf8)) {
            clear_run(run, gen);
            return 0;
        }
        run->caret = run->nbytes;
        if (edit) {
            s_editing = true;
            s_edit = slot;
        }
        return (gen << 6) | slot;
    }
    fprintf(stderr, "[Text] No free text box\n");
    return 0;
}

uint32_t text_add(float x0, float y0, float x1, float y1, const char *utf8) {
    return alloc_run(x0, y0, x1, y1, utf8, false);
}

bool text_set(uint32_t id, const char *utf8) {
    int slot = user_slot(id);
    if (slot < 0) {
        return false;
    }
    if (!copy_text(&s_live[slot], utf8)) {
        return false;
    }
    s_live[slot].caret = s_live[slot].nbytes;
    return true;
}

bool text_set_rect(uint32_t id, float x0, float y0, float x1, float y1) {
    int slot = user_slot(id);
    if (slot < 0) {
        return false;
    }
    order_rect(&x0, &y0, &x1, &y1);
    Run *run = &s_live[slot];
    run->x0 = x0;
    run->y0 = y0;
    run->x1 = x1;
    run->y1 = y1;
    run->px = px_for_height(y1 - y0);
    run->cache_ok = false;
    return true;
}

bool text_set_face(uint32_t id, uint32_t face) {
    int slot = user_slot(id);
    if (slot < 0 || face >= TEXT_FACE_COUNT) {
        return false;
    }
    s_live[slot].face = face;
    s_live[slot].cache_ok = false;
    return true;
}

bool text_set_color(uint32_t id, float r, float g, float b, float a) {
    int slot = user_slot(id);
    if (slot < 0) {
        return false;
    }
    s_live[slot].r = r;
    s_live[slot].g = g;
    s_live[slot].b = b;
    s_live[slot].a = a;
    return true;
}

bool text_remove(uint32_t id) {
    int slot = user_slot(id);
    if (slot < 0) {
        return false;
    }
    if (s_editing && s_edit == (uint32_t)slot) {
        s_editing = false;
    }
    uint32_t gen = bump_gen(s_live[slot].gen);
    clear_run(&s_live[slot], gen);
    return true;
}

static int hit_run(float x, float y) {
    for (uint32_t slot = k_runs; slot > 1u; --slot) {
        Run *run = &s_live[slot - 1u];
        if (!run->used || run->sealed) {
            continue;
        }
        if (x >= run->x0 && x <= run->x1 && y >= run->y0 && y <= run->y1) {
            return (int)(slot - 1u);
        }
    }
    return -1;
}

static void begin_edit(uint32_t slot) {
    s_editing = true;
    s_edit = slot;
    s_live[slot].caret = s_live[slot].nbytes;
    s_live[slot].cache_ok = false;
}

bool text_click(float x, float y, bool t_down) {
    if (t_down && s_anchor) {
        s_anchor = false;
        uint32_t id = alloc_run(s_ax, s_ay, x, y, "", true);
        if (id != 0u) {
            printf("[Text] Box ready.\n");
            fflush(stdout);
        }
        return true;
    }
    if (t_down) {
        if (s_editing) {
            int hit = hit_run(x, y);
            if (hit >= 0 && (uint32_t)hit == s_edit) {
                begin_edit((uint32_t)hit);
                return true;
            }
        }
        s_anchor = true;
        s_ax = x;
        s_ay = y;
        s_bx = x;
        s_by = y;
        return true;
    }
    int hit = hit_run(x, y);
    if (hit >= 0) {
        begin_edit((uint32_t)hit);
        return true;
    }
    if (s_editing) {
        s_editing = false;
        return true;
    }
    return false;
}

void text_arm_tick(bool t_down, bool have_xy, int32_t px, int32_t py) {
    if (s_anchor && !t_down) {
        s_anchor = false;
        printf("[Text] Box cancelled.\n");
        fflush(stdout);
        return;
    }
    if (!s_anchor || !have_xy || s_fb_w == 0u || s_fb_h == 0u) {
        return;
    }
    s_bx = (float)px - (float)s_fb_w * 0.5f;
    s_by = (float)s_fb_h * 0.5f - (float)py;
}

static uint32_t utf8_prev(const char *s, uint32_t i) {
    if (i == 0u) {
        return 0;
    }
    i--;
    while (i > 0u && ((uint8_t)s[i] & 0xC0u) == 0x80u) {
        i--;
    }
    return i;
}

static uint32_t utf8_next(const char *s, uint32_t n, uint32_t i) {
    if (i >= n) {
        return n;
    }
    i++;
    while (i < n && ((uint8_t)s[i] & 0xC0u) == 0x80u) {
        i++;
    }
    return i;
}

static uint32_t line_start(const char *s, uint32_t i) {
    while (i > 0u && s[i - 1u] != '\n') {
        i--;
    }
    return i;
}

static uint32_t line_end(const char *s, uint32_t n, uint32_t i) {
    while (i < n && s[i] != '\n') {
        i++;
    }
    return i;
}

static uint32_t cp_count(const char *s, uint32_t begin, uint32_t end) {
    uint32_t count = 0;
    uint32_t i = begin;
    while (i < end) {
        i = utf8_next(s, end, i);
        count++;
    }
    return count;
}

static uint32_t cp_skip(const char *s, uint32_t begin, uint32_t end, uint32_t count) {
    uint32_t i = begin;
    while (count > 0u && i < end) {
        i = utf8_next(s, end, i);
        count--;
    }
    return i;
}

bool text_insert(const char *utf8, uint32_t bytes) {
    if (!s_editing || s_edit == 0u || s_edit >= k_runs || utf8 == nullptr || bytes == 0u) {
        return false;
    }
    Run *run = &s_live[s_edit];
    if (!run->used || run->sealed) {
        return false;
    }
    if (run->nbytes + bytes > k_bytes) {
        return false;
    }
    uint32_t caret = run->caret;
    if (caret > run->nbytes) {
        caret = run->nbytes;
    }
    memmove(run->utf8 + caret + bytes, run->utf8 + caret, run->nbytes - caret + 1u);
    memcpy(run->utf8 + caret, utf8, bytes);
    run->nbytes += bytes;
    run->caret = caret + bytes;
    run->utf8[run->nbytes] = '\0';
    run->hash = hash_bytes(run->utf8, run->nbytes);
    run->cache_ok = false;
    return true;
}

bool text_on_key(uint32_t key) {
    if (!s_editing || s_edit == 0u || s_edit >= k_runs) {
        return false;
    }
    Run *run = &s_live[s_edit];
    if (!run->used || run->sealed) {
        s_editing = false;
        return false;
    }
    if (key == KEY_ESC) {
        s_editing = false;
        return true;
    }
    if (key == KEY_BACKSPACE) {
        if (run->caret == 0u) {
            return true;
        }
        uint32_t prev = utf8_prev(run->utf8, run->caret);
        memmove(run->utf8 + prev, run->utf8 + run->caret, run->nbytes - run->caret + 1u);
        run->nbytes -= run->caret - prev;
        run->caret = prev;
        run->hash = hash_bytes(run->utf8, run->nbytes);
        run->cache_ok = false;
        return true;
    }
    if (key == KEY_DELETE) {
        if (run->caret >= run->nbytes) {
            return true;
        }
        uint32_t next = utf8_next(run->utf8, run->nbytes, run->caret);
        memmove(run->utf8 + run->caret, run->utf8 + next, run->nbytes - next + 1u);
        run->nbytes -= next - run->caret;
        run->hash = hash_bytes(run->utf8, run->nbytes);
        run->cache_ok = false;
        return true;
    }
    if (key == KEY_LEFT) {
        run->caret = utf8_prev(run->utf8, run->caret);
        run->cache_ok = false;
        return true;
    }
    if (key == KEY_RIGHT) {
        run->caret = utf8_next(run->utf8, run->nbytes, run->caret);
        run->cache_ok = false;
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        uint32_t start = line_start(run->utf8, run->caret);
        uint32_t col = cp_count(run->utf8, start, run->caret);
        if (key == KEY_UP) {
            if (start == 0u) {
                run->caret = 0;
            } else {
                uint32_t prev_end = start - 1u;
                uint32_t prev = line_start(run->utf8, prev_end);
                run->caret = cp_skip(run->utf8, prev, prev_end, col);
            }
        } else if (start < run->nbytes) {
            uint32_t end = line_end(run->utf8, run->nbytes, start);
            if (end >= run->nbytes) {
                run->caret = run->nbytes;
            } else {
                uint32_t next = end + 1u;
                uint32_t next_end = line_end(run->utf8, run->nbytes, next);
                run->caret = cp_skip(run->utf8, next, next_end, col);
            }
        }
        run->cache_ok = false;
        return true;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        (void)text_insert("\n", 1u);
        return true;
    }
    return true;
}

bool text_editing(void) {
    return s_editing;
}

static bool same_run(const Run *live, const ShownRun *shown) {
    if (live->used != shown->used) {
        return false;
    }
    if (!live->used) {
        return true;
    }
    return live->gen == shown->gen && live->face == shown->face && live->caret == shown->caret &&
           live->nbytes == shown->nbytes && live->x0 == shown->x0 && live->y0 == shown->y0 &&
           live->x1 == shown->x1 && live->y1 == shown->y1 && live->r == shown->r &&
           live->g == shown->g && live->b == shown->b && live->a == shown->a &&
           live->px == shown->px && memcmp(live->utf8, shown->utf8, live->nbytes) == 0;
}

bool text_busy(void) {
    for (uint32_t i = 0; i < k_runs; ++i) {
        if (!same_run(&s_live[i], &s_shown[i])) {
            return true;
        }
    }
    if (s_editing != s_shown_editing || (s_editing && s_edit != s_shown_edit)) {
        return true;
    }
    if (s_anchor != s_shown_anchor) {
        return true;
    }
    if (s_anchor && (s_ax != s_shown_ax || s_ay != s_shown_ay || s_bx != s_shown_bx || s_by != s_shown_by)) {
        return true;
    }
    return false;
}

static void copy_shown(uint32_t slot) {
    const Run *live = &s_live[slot];
    ShownRun *shown = &s_shown[slot];
    *shown = (ShownRun){0};
    shown->used = live->used;
    shown->gen = live->gen;
    shown->face = live->face;
    shown->x0 = live->x0;
    shown->y0 = live->y0;
    shown->x1 = live->x1;
    shown->y1 = live->y1;
    shown->r = live->r;
    shown->g = live->g;
    shown->b = live->b;
    shown->a = live->a;
    shown->px = live->px;
    shown->caret = live->caret;
    shown->nbytes = live->nbytes;
    if (live->nbytes <= k_bytes) {
        memcpy(shown->utf8, live->utf8, live->nbytes + 1u);
    }
}

void text_commit(void) {
    for (uint32_t i = 0; i < k_runs; ++i) {
        copy_shown(i);
    }
    s_shown_editing = s_editing;
    s_shown_edit = s_edit;
    s_shown_anchor = s_anchor;
    s_shown_ax = s_ax;
    s_shown_ay = s_ay;
    s_shown_bx = s_bx;
    s_shown_by = s_by;
}

void text_place_boot(void) {
    memset(s_live, 0, sizeof s_live);
    memset(s_shown, 0, sizeof s_shown);
    s_editing = false;
    s_anchor = false;
    Run *run = &s_live[0];
    run->used = true;
    run->sealed = true;
    run->gen = 1;
    run->face = TEXT_FACE_SEALED;
    run->px = 22.0f;
    run->r = 0.93f;
    run->g = 0.91f;
    run->b = 0.86f;
    run->a = 1.0f;
    memcpy(run->utf8, "Antigravity", 12u);
    run->nbytes = 11;
    run->hash = hash_bytes(run->utf8, run->nbytes);
    text_commit();
}

static void ensure_layout(Run *run, float width, float height) {
    uint32_t epoch = text_face_epoch(run->face);
    if (run->cache_ok && run->cache_face == run->face && run->cache_epoch == epoch &&
        run->cache_px == run->px && run->cache_w == width && run->cache_h == height &&
        run->cache_hash == run->hash && run->cache_caret == run->caret) {
        return;
    }
    run->cache_n = text_shape_box(run->face, run->utf8, run->nbytes, width, height, run->px,
                                  run->caret, run->cache, k_glyphs, &run->caret_x, &run->caret_y,
                                  &run->asc, &run->desc);
    run->cache_face = run->face;
    run->cache_epoch = epoch;
    run->cache_px = run->px;
    run->cache_w = width;
    run->cache_h = height;
    run->cache_hash = run->hash;
    run->cache_caret = run->caret;
    run->cache_ok = true;
}

static uint32_t push_quad(TextQuad *out, uint32_t n, uint32_t cap,
                          float x0, float y0, float x1, float y1,
                          float u0, float v0, float u1, float v1,
                          float r, float g, float b, float a,
                          float bx0, float by0, float bx1, float by1, float mode) {
    if (n + 1u > cap) {
        if (!s_quad_warned) {
            fprintf(stderr, "[Text Error] Text quad buffer is full\n");
            s_quad_warned = true;
        }
        return n;
    }
    out[n] = (TextQuad){
        .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
        .u0 = u0, .v0 = v0, .u1 = u1, .v1 = v1,
        .r = r, .g = g, .b = b, .a = a,
        .bx0 = bx0, .by0 = by0, .bx1 = bx1, .by1 = by1,
        .mode = mode,
    };
    return n + 1u;
}

static uint32_t push_frame(TextQuad *out, uint32_t n, uint32_t cap,
                           float x0, float y0, float x1, float y1,
                           float r, float g, float b, float a) {
    float t = 1.5f;
    if ((x1 - x0) < t * 2.0f || (y1 - y0) < t * 2.0f) {
        return n;
    }
    n = push_quad(out, n, cap, x0, y0, x1, y0 + t, 0, 0, 0, 0, r, g, b, a, x0, y0, x1, y1, 0.0f);
    n = push_quad(out, n, cap, x0, y1 - t, x1, y1, 0, 0, 0, 0, r, g, b, a, x0, y0, x1, y1, 0.0f);
    n = push_quad(out, n, cap, x0, y0 + t, x0 + t, y1 - t, 0, 0, 0, 0, r, g, b, a, x0, y0, x1, y1, 0.0f);
    n = push_quad(out, n, cap, x1 - t, y0 + t, x1, y1 - t, 0, 0, 0, 0, r, g, b, a, x0, y0, x1, y1, 0.0f);
    return n;
}

static float to_px_x(float click_x, float fb_w) {
    return click_x + fb_w * 0.5f;
}

static float to_px_y(float click_y, float fb_h) {
    return fb_h * 0.5f - click_y;
}

static uint32_t emit_run(TextQuad *out, uint32_t n, uint32_t cap, Run *run,
                         float left, float bottom, float right, float top,
                         uint32_t fb_w, uint32_t fb_h, bool plate) {
    float width = right - left;
    float height = top - bottom;
    if (width < 1.0f || height < 1.0f) {
        return n;
    }
    ensure_layout(run, width, height);
    float x0 = to_px_x(left, (float)fb_w);
    float x1 = to_px_x(right, (float)fb_w);
    float y0 = to_px_y(top, (float)fb_h);
    float y1 = to_px_y(bottom, (float)fb_h);
    float fringe = (float)TEXT_SDF_SPREAD * (run->px / (float)TEXT_SDF_PX);
    float clip_x0 = x0 - fringe;
    float clip_y0 = y0 - fringe;
    float clip_x1 = x1 + fringe;
    float clip_y1 = y1 + fringe;
    if (plate) {
        n = push_quad(out, n, cap, x0, y0, x1, y1, 0, 0, 0, 0,
                      0.10f, 0.11f, 0.13f, 0.72f, x0, y0, x1, y1, 0.0f);
        bool hot = s_editing && s_edit < k_runs && &s_live[s_edit] == run;
        if (hot) {
            n = push_frame(out, n, cap, x0, y0, x1, y1, 0.72f, 0.78f, 0.86f, 0.95f);
        } else {
            n = push_frame(out, n, cap, x0, y0, x1, y1, 0.35f, 0.38f, 0.42f, 0.85f);
        }
    }
    uint64_t atlas = text_atlas_address();
    uint32_t epoch = text_face_epoch(run->face);
    for (uint32_t i = 0; i < run->cache_n; ++i) {
        const TextGlyphQuad *quad = &run->cache[i];
        if (!quad->visible || atlas == 0u) {
            continue;
        }
        TextSdf sdf = {0};
        TextAtlasRect rect = {0};
        if (!text_face_glyph(run->face, quad->glyph, &sdf) ||
            !text_atlas_pack(run->face, epoch, quad->glyph, sdf.bytes, sdf.width, sdf.rows, &rect) ||
            rect.w == 0u || rect.h == 0u) {
            continue;
        }
        float gx0 = to_px_x(left + quad->x0, (float)fb_w);
        float gx1 = to_px_x(left + quad->x1, (float)fb_w);
        float gy0 = to_px_y(top + quad->y1, (float)fb_h);
        float gy1 = to_px_y(top + quad->y0, (float)fb_h);
        n = push_quad(out, n, cap, gx0, gy0, gx1, gy1,
                      (float)rect.x, (float)rect.y,
                      (float)(rect.x + rect.w), (float)(rect.y + rect.h),
                      run->r, run->g, run->b, run->a,
                      clip_x0, clip_y0, clip_x1, clip_y1, 1.0f);
    }
    if (plate && s_editing && s_edit < k_runs && &s_live[s_edit] == run) {
        float cx = to_px_x(left + run->caret_x, (float)fb_w);
        float cy0 = to_px_y(top + run->caret_y + run->asc, (float)fb_h);
        float cy1 = to_px_y(top + run->caret_y - run->desc, (float)fb_h);
        n = push_quad(out, n, cap, cx, cy0, cx + 1.5f, cy1, 0, 0, 0, 0,
                      0.96f, 0.97f, 0.98f, 1.0f, x0, y0, x1, y1, 0.0f);
    }
    return n;
}

uint32_t text_fill(TextQuad *out, uint32_t cap, uint32_t fb_w, uint32_t fb_h) {
    s_fb_w = fb_w;
    s_fb_h = fb_h;
    if (out == nullptr || cap == 0u || fb_w == 0u || fb_h == 0u) {
        return 0;
    }
    uint32_t n = 0;
    float sealed_left = 16.0f - (float)fb_w * 0.5f;
    float sealed_top = (float)fb_h * 0.5f - 44.0f;
    float sealed_right = sealed_left + 260.0f;
    float sealed_bottom = sealed_top - 36.0f;
    if (s_live[0].used) {
        n = emit_run(out, n, cap, &s_live[0], sealed_left, sealed_bottom, sealed_right, sealed_top,
                     fb_w, fb_h, false);
    }
    for (uint32_t slot = 1; slot < k_runs; ++slot) {
        if (!s_live[slot].used) {
            continue;
        }
        n = emit_run(out, n, cap, &s_live[slot], s_live[slot].x0, s_live[slot].y0,
                     s_live[slot].x1, s_live[slot].y1, fb_w, fb_h, true);
    }
    if (s_anchor) {
        float x0 = s_ax;
        float y0 = s_ay;
        float x1 = s_bx;
        float y1 = s_by;
        order_rect(&x0, &y0, &x1, &y1);
        if ((x1 - x0) < 1.0f && (y1 - y0) < 1.0f) {
            x1 = x0 + 8.0f;
            y0 = y1 - 8.0f;
        }
        float px0 = to_px_x(x0, (float)fb_w);
        float px1 = to_px_x(x1, (float)fb_w);
        float py0 = to_px_y(y1, (float)fb_h);
        float py1 = to_px_y(y0, (float)fb_h);
        n = push_frame(out, n, cap, px0, py0, px1, py1, 0.93f, 0.91f, 0.86f, 0.90f);
    }
    return n;
}
