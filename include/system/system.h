#ifndef ENGINE_SYSTEM_H
#define ENGINE_SYSTEM_H

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

#endif /* ENGINE_SYSTEM_H */