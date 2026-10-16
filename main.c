/* === FILE: ./main.c === */

#include <stdio.h>
#include "init/system_init/system_init.h"
#include "init/ring_init/ring_init.h"
#include "init/wayland_init/wayland_init.h"
#include "init/vulkan_init/vulkan_init.h"
#include "init/engine_init/engine_init.h"

#ifndef ENGINE_VERSION_STR
#define ENGINE_VERSION_STR "0.0.0-unknown"
#endif

#ifndef ENGINE_BUILD_MODE
#define ENGINE_BUILD_MODE "development"
#endif

static void format_stage(int rc, char *buf, size_t buf_size) {
    if (rc < 0) {
        snprintf(buf, buf_size, "skipped");
    } else if (rc == 0) {
        snprintf(buf, buf_size, "ok");
    } else {
        snprintf(buf, buf_size, "failed (%d)", rc);
    }
}

int main(void) {
    printf("[C23 Engine] Version: %s (%s mode, C standard: %ld)\n",
           ENGINE_VERSION_STR, ENGINE_BUILD_MODE, __STDC_VERSION__);

    // Boot order: system -> ring -> wayland -> vulkan -> engine.
    // A failed stage skips everything after it. Shutdown runs in reverse (LIFO).
    int system_rc  = system_init();
    int ring_rc    = -1;
    int wayland_rc = -1;
    int vulkan_rc  = -1;
    int engine_rc  = -1;

    if (system_rc == 0) {
        ring_rc = ring_init();
    }
    if (system_rc == 0 && ring_rc == 0) {
        wayland_rc = wayland_init();
    }
    if (system_rc == 0 && ring_rc == 0 && wayland_rc == 0) {
        vulkan_rc = vulkan_init();
    }
    if (system_rc == 0 && ring_rc == 0 && wayland_rc == 0 && vulkan_rc == 0) {
        engine_rc = engine_subsystem_init();
    }

    const bool ok = system_rc == 0 && ring_rc == 0 && wayland_rc == 0 &&
                    vulkan_rc == 0 && engine_rc == 0;
    if (ok) {
        printf("[C23 Engine] All active subsystems initialized successfully.\n");
    } else {
        char system_status[32];
        char ring_status[32];
        char wayland_status[32];
        char vulkan_status[32];
        char engine_status[32];
        format_stage(system_rc, system_status, sizeof system_status);
        format_stage(ring_rc, ring_status, sizeof ring_status);
        format_stage(wayland_rc, wayland_status, sizeof wayland_status);
        format_stage(vulkan_rc, vulkan_status, sizeof vulkan_status);
        format_stage(engine_rc, engine_status, sizeof engine_status);
        fprintf(stderr,
                "[C23 Engine] Subsystem init failed (system=%s, ring=%s, wayland=%s, vulkan=%s, engine=%s).\n",
                system_status, ring_status, wayland_status, vulkan_status, engine_status);
    }

    if (engine_rc == 0) {
        engine_subsystem_shutdown();
    }
    if (vulkan_rc == 0) {
        vulkan_shutdown();
    }
    if (wayland_rc == 0) {
        wayland_shutdown();
    }
    if (ring_rc == 0) {
        ring_shutdown();
    }
    if (system_rc == 0) {
        system_shutdown();
    }

    return ok ? 0 : 1;
}