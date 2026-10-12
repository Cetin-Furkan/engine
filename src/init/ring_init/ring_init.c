#include "init/ring_init/ring_init.h"
#include <stdio.h>

int ring_init(void) {
    printf("hello init from ring\n");
    return 0;
}

void ring_shutdown(void) {
    printf("goodbye from ring\n");
}
