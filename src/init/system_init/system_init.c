#include "init/system_init/system_init.h"
#include <stdio.h>

int system_init(void) {
    printf("hello init from system\n");
    return 0;
}

void system_shutdown(void) {
    printf("goodbye from system\n");
}
