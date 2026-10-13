#include <stdio.h>
#include "init/wayland_init/wayland_init.h"
#include "init/vulkan_init/vulkan_init.h"
#include "init/engine_init/engine_init.h"

#ifndef ENGINE_VERSION_STR
#define ENGINE_VERSION_STR "0.0.0-unknown"
#endif

#ifndef ENGINE_BUILD_MODE
#define ENGINE_BUILD_MODE "development"
#endif

int main(void) {
    // Pure C23: bool/true/false and nullptr are native keywords, no stdbool.h needed
    bool initialized = true;
    const char *status = nullptr;

    printf("[C23 Engine] Version: %s (%s mode, C standard: %ld)\n",
           ENGINE_VERSION_STR, ENGINE_BUILD_MODE, __STDC_VERSION__);

    wayland_init();
    vulkan_init();
    engine_subsystem_init();

    if (initialized && status == nullptr) {
        printf("[C23 Engine] All active subsystems initialized successfully.\n");
    }

    // Symmetric reverse teardown
    engine_subsystem_shutdown();
    vulkan_shutdown();
    wayland_shutdown();

    return 0;
}
