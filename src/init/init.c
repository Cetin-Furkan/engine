#include "init/init.h"
#include <stdio.h>

int engine_init(void) {
    printf("[Engine] Initializing subsystem...\n");
    return 0;
}

void engine_shutdown(void) {
    printf("[Engine] Shutting down subsystem...\n");
}
