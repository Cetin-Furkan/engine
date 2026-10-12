#include "init/wayland_init/wayland_init.h"
#include <stdio.h>

int wayland_init(void) {
    printf("hello init from wayland\n");
    return 0;
}

void wayland_shutdown(void) {
    printf("goodbye from wayland\n");
}
