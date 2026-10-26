#include "world/world_int.h"
#include <stdio.h>

static WorldObj s_obj[k_table_cap];
static int      s_n = 0;
static int      s_selected = -1;
static int      s_shown_selected = -1;
static bool     s_spun = false;

void table_reset(void) {
    s_n = 0;
    s_selected = -1;
    s_shown_selected = -1;
    s_spun = false;
}

int table_add(const WorldOps *ops, void *data, Coord at, Spin *spin) {
    if (ops == nullptr || s_n >= k_table_cap) {
        return -1;
    }
    int index = s_n++;
    s_obj[index] = (WorldObj){
        .ops   = ops,
        .data  = data,
        .at    = at,
        .alive = true,
        .spin  = spin,
    };
    return index;
}

void table_publish(void) {
    for (int i = 0; i < s_n; ++i) {
        if (!s_obj[i].alive) {
            continue;
        }
        s_obj[i].shown_alive = true;
        if (s_obj[i].spin != nullptr) {
            spin_commit(s_obj[i].spin);
        }
        if (s_obj[i].ops->commit != nullptr) {
            s_obj[i].ops->commit(&s_obj[i]);
        }
    }
    s_shown_selected = s_selected;
}

WorldObj *table_at(int index) {
    if (index < 0 || index >= s_n) {
        return nullptr;
    }
    return &s_obj[index];
}

int table_count(void) {
    return s_n;
}

WorldObj *table_find(const WorldOps *ops) {
    for (int i = 0; i < s_n; ++i) {
        if (s_obj[i].alive && s_obj[i].ops == ops) {
            return &s_obj[i];
        }
    }
    return nullptr;
}

WorldObj *table_selected(void) {
    return table_at(s_selected);
}

static const char *obj_name(const WorldObj *o) {
    if (o->ops != nullptr && o->ops->name != nullptr) {
        const char *n = o->ops->name(o);
        if (n != nullptr) {
            return n;
        }
    }
    return "Object";
}

bool table_select(int index) {
    WorldObj *o = table_at(index);
    if (o == nullptr || !o->alive) {
        return false;
    }
    bool changed = s_selected != index;
    if (changed) {
        s_selected = index;
        printf("[Engine] %s selected.\n", obj_name(o));
        fflush(stdout);
    }
    return changed;
}

void table_select_quiet(int index) {
    if (table_at(index) == nullptr) {
        return;
    }
    s_selected = index;
}

void table_clear_selection(void) {
    s_selected = -1;
}

bool table_highlight(const WorldObj *o, bool live) {
    if (o == nullptr) {
        return false;
    }
    int index = (int)(o - s_obj);
    if (index < 0 || index >= s_n) {
        return false;
    }
    return index == (live ? s_selected : s_shown_selected);
}

int table_pick(const CameraRay *ray, Coord *at, float *t_hit) {
    int best = -1;
    float best_t = 0.0f;
    Coord best_at = {0};
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (!o->alive || o->ops == nullptr || o->ops->hit == nullptr) {
            continue;
        }
        float t = 0.0f;
        Coord hit = {0};
        if (!o->ops->hit(o, ray, &t, &hit)) {
            continue;
        }
        if (best < 0 || t < best_t) {
            best = i;
            best_t = t;
            best_at = hit;
        }
    }
    if (best < 0) {
        return -1;
    }
    if (at != nullptr) {
        *at = best_at;
    }
    if (t_hit != nullptr) {
        *t_hit = best_t;
    }
    return best;
}

void table_draw(VkCommandBuffer cmd, uint32_t width, uint32_t height, bool live) {
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        bool up = live ? o->alive : o->shown_alive;
        if (!up || o->ops == nullptr || o->ops->draw == nullptr) {
            continue;
        }
        o->ops->draw(o, cmd, width, height, live);
    }
}

bool table_advance(uint32_t time_ms) {
    bool changed = s_selected != s_shown_selected;
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->alive != o->shown_alive) {
            changed = true;
        }
        if (!o->alive || o->ops == nullptr || o->ops->advance == nullptr) {
            continue;
        }
        if (o->ops->advance(o, time_ms)) {
            changed = true;
        }
    }
    return changed;
}

void table_commit(void) {
    int write = 0;
    int selected = -1;
    for (int i = 0; i < s_n; ++i) {
        if (!s_obj[i].alive) {
            continue;
        }
        if (i == s_selected) {
            selected = write;
        }
        s_obj[i].shown_alive = true;
        if (s_obj[i].spin != nullptr) {
            spin_commit(s_obj[i].spin);
        }
        if (s_obj[i].ops->commit != nullptr) {
            s_obj[i].ops->commit(&s_obj[i]);
        }
        if (write != i) {
            s_obj[write] = s_obj[i];
        }
        write++;
    }
    s_n = write;
    s_selected = selected;
    s_shown_selected = selected;
}

bool table_busy(void) {
    if (s_selected != s_shown_selected) {
        return true;
    }
    for (int i = 0; i < s_n; ++i) {
        WorldObj *o = &s_obj[i];
        if (o->alive != o->shown_alive) {
            return true;
        }
        if (!o->alive) {
            continue;
        }
        if (o->spin != nullptr && spin_busy(o->spin)) {
            return true;
        }
        if (o->ops != nullptr && o->ops->busy != nullptr && o->ops->busy(o)) {
            return true;
        }
    }
    return false;
}

void table_mark_spun(void) {
    s_spun = true;
}

bool table_spun_pending(void) {
    return s_spun;
}

void table_clear_spun(void) {
    s_spun = false;
}

uint32_t table_alive_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < s_n; ++i) {
        if (s_obj[i].alive) {
            n++;
        }
    }
    return n;
}
