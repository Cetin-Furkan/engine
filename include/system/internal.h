#ifndef ENGINE_SYSTEM_INTERNAL_H
#define ENGINE_SYSTEM_INTERNAL_H

#include "system/system.h"

void system_probe_cpu(SystemCpuInfo *out);
[[nodiscard]] int system_probe_memory_and_limits(SystemMemoryInfo *out);
[[nodiscard]] int system_probe_uring_kernel(SystemUringInfo *out);

#endif /* ENGINE_SYSTEM_INTERNAL_H */
