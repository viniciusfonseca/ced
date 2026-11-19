#include "process_memory.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <stdio.h>
#include <unistd.h>
#endif

uint64_t ced_memory_usage_bytes(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info = {0}; info.cb = sizeof info;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof info)) return (uint64_t)info.WorkingSetSize;
#elif defined(__APPLE__)
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS &&
        count >= TASK_VM_INFO_REV1_COUNT)
        return (uint64_t)info.phys_footprint;
#elif defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    if (!file) return 0;
    unsigned long long total, resident;
    int fields = fscanf(file, "%llu %llu", &total, &resident);
    fclose(file);
    long page_size = sysconf(_SC_PAGESIZE);
    if (fields == 2 && page_size > 0 && resident <= UINT64_MAX / (uint64_t)page_size)
        return (uint64_t)resident * (uint64_t)page_size;
#endif
    return 0;
}
