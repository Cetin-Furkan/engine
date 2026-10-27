#ifndef ENGINE_WORLD_SPIN_H
#define ENGINE_WORLD_SPIN_H

#include <stdbool.h>
#include <stdint.h>

// One revolution on the compositor clock. 72 milli-degrees per millisecond
// is one turn in 5000 ms. A finite spin rests at rest_milli. An infinite
// spin wraps. The shown fields are the picture already on screen.

typedef struct Spin {
    int32_t  rest_milli;
    int32_t  live_milli;
    int32_t  shown_milli;
    uint32_t t0;
    bool     have_t0;
    bool     spinning;
    bool     infinite;
    bool     glow;
    bool     shown_glow;
} Spin;

void spin_clear(void);
Spin spin_start(int32_t rest_milli);

// True when the live angle moved. The first stamp only stores the clock.
bool spin_advance(Spin *s, uint32_t time_ms, bool log_start);
void spin_commit(Spin *s);
bool spin_busy(const Spin *s);

// True when the spin is now endless.
bool spin_toggle(Spin *s);
void spin_respin(Spin *s);

float spin_deg(const Spin *s, bool live);

#endif /* ENGINE_WORLD_SPIN_H */
