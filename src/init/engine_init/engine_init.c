#include "init/engine_init/engine_init.h"
#include <stdio.h>

int engine_subsystem_init(void) {
    printf("hello init from engine\n");
    return 0;
}

void engine_subsystem_shutdown(void) {
    printf("goodbye from engine\n");
}
