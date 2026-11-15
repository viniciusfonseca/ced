#ifndef CED_EXTENSION_PROCESS_INTERNAL_H
#define CED_EXTENSION_PROCESS_INTERNAL_H

/* Private worker storage; also used by isolated queue/fault-injection tests. */
#include "process.h"
#include <pthread.h>

typedef struct { CedExtensionMessage *first, *last; size_t count, bytes; } MessageQueue;
struct CedExtensionProcess {
    pthread_t thread;
    pthread_mutex_t mutex;
    MessageQueue incoming, outgoing;
    size_t reserved_count, reserved_bytes;
    bool stop, done, abandoned;
    char profile[CED_PATH], package[CED_PATH], id[129], version[64], hash[65], session[96];
    char error[CED_ERROR], pending_log[8193];
};

#endif
