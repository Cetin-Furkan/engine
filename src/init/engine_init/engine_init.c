#include "init/engine_init/engine_init.h"
#include "init/helpers/fmt.h"

int engine_subsystem_init(void) {
    log_info_raw("hello init from engine\n");
    return 0;
}

void engine_subsystem_shutdown(void) {
    log_info_raw("goodbye from engine\n");
}