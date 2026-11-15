#ifndef CED_PROCESS_MEMORY_H
#define CED_PROCESS_MEMORY_H

#include <stdint.h>

/* Current process memory in bytes: physical footprint on macOS, RSS on Linux.
 * Returns zero if unavailable. */
uint64_t ced_memory_usage_bytes(void);

#endif
