#include "world/spin.h"
#include <stdio.h>

enum : int32_t {
    k_turn_milli = 360000,
};
enum : uint32_t {
    k_turn_period_ms = 5000u,
    k_milli_per_ms   = 72u,
};

static bool s_logged_start = false;

void spin_clear(void) {
    s_logged_start = false;
}

Spin spin_start(int32_t rest_milli) {
    Spin s = {0};
    s.rest_milli = rest_milli;
    s.live_milli = rest_milli;
    s.shown_milli = rest_milli;
    s.spinning = true;
    return s;
}

bool spin_advance(Spin *s, uint32_t time_ms, bool log_start) {
    if (s == nullptr || !s->spinning) {
        return false;
    }
    if (!s->have_t0) {
        s->t0 = time_ms;
        s->have_t0 = true;
        s->live_milli = s->shown_milli;
        if (log_start && !s_logged_start) {
            printf("[Engine] Turn started. One revolution, wl_surface.frame clock, %u ms.\n",
                   k_turn_period_ms);
            fflush(stdout);
            s_logged_start = true;
        }
        return false;
    }
    if (s->infinite) {
        uint32_t step = time_ms - s->t0;
        if (step == 0u) {
            return false;
        }
        if (step > 1000u) {
            s->t0 = time_ms;
            return false;
        }
        s->t0 = time_ms;
        int32_t next = s->live_milli + (int32_t)(k_milli_per_ms * step);
        next %= k_turn_milli;
        if (next < 0) {
            next += k_turn_milli;
        }
        s->live_milli = next;
        return next != s->shown_milli;
    }
    uint32_t dt = time_ms - s->t0;
    int32_t milli = s->rest_milli;
    if (dt < k_turn_period_ms) {
        milli += (int32_t)(k_milli_per_ms * dt);
    }
    s->live_milli = milli;
    if (milli == s->shown_milli) {
        if (dt >= k_turn_period_ms) {
            s->spinning = false;
        }
        return false;
    }
    return true;
}

void spin_commit(Spin *s) {
    if (s == nullptr) {
        return;
    }
    s->shown_milli = s->live_milli;
    s->shown_glow = s->glow;
}

bool spin_busy(const Spin *s) {
    if (s == nullptr) {
        return false;
    }
    return s->spinning || s->live_milli != s->shown_milli || s->glow != s->shown_glow;
}

bool spin_toggle(Spin *s) {
    if (s->infinite) {
        s->infinite = false;
        s->spinning = false;
        return false;
    }
    s->infinite = true;
    s->spinning = true;
    s->have_t0 = false;
    return true;
}

void spin_respin(Spin *s) {
    s->spinning = true;
    s->infinite = false;
    s->have_t0 = false;
}

float spin_deg(const Spin *s, bool live) {
    if (s == nullptr) {
        return 0.0f;
    }
    int32_t milli = live ? s->live_milli : s->shown_milli;
    return (float)milli / 1000.0f;
}
