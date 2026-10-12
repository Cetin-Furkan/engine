#include "init/vulkan_init/vulkan_init.h"
#include <stdio.h>

int vulkan_init(void) {
    printf("hello init from vulkan\n");
    return 0;
}

void vulkan_shutdown(void) {
    printf("goodbye from vulkan\n");
}
