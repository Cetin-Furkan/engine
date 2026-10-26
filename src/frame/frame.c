#include "frame/frame.h"
#include "frame/frame_int.h"

// Starts the present process and calls the code in this folder.

int engine_subsystem_init(void) {
    return frame_boot();
}

int engine_subsystem_run(void) {
    return frame_run();
}

int engine_subsystem_shutdown(void) {
    return frame_stop();
}
