#include "input/internal.h"
#include <string.h>

bool input_take_u32(const unsigned char **p, const unsigned char *end, uint32_t *o) {
    if ((size_t)(end - *p) < 4u) {
        return false;
    }
    memcpy(o, *p, 4);
    *p += 4;
    return true;
}

bool input_take_i32(const unsigned char **p, const unsigned char *end, int32_t *o) {
    uint32_t u = 0;
    if (!input_take_u32(p, end, &u)) {
        return false;
    }
    memcpy(o, &u, 4);
    return true;
}
