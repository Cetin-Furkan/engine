/* === FILE: ./include/init/system_init/system_init.h === */

#ifndef SYSTEM_INIT_H
#define SYSTEM_INIT_H

#include <stdint.h>

typedef struct SystemCapabilities {
    char     sysname[65];
    char     release[65];
    uint64_t memlock_bytes;
    uint64_t uring_setup_flags;
    uint64_t uring_feature_flags;
    uint32_t uring_nr_request_opcodes;
    uint32_t uring_nr_register_opcodes;
    bool     has_no_iowait;
    bool     is_initialized;
} SystemCapabilities;

[[nodiscard]] int system_init(void);
void system_shutdown(void);
[[nodiscard]] const SystemCapabilities *system_get_capabilities(void);

#endif /* SYSTEM_INIT_H */