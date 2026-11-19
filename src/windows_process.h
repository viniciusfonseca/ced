#ifndef CED_WINDOWS_PROCESS_H
#define CED_WINDOWS_PROCESS_H
#include "windows_compat.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    HANDLE process, job, input, output;
    HPCON console;
    OVERLAPPED read, write;
    bool reading, writing;
    DWORD available, offset;
    char incoming[65536], outgoing[65536];
} CedWindowsProcess;

bool ced_windows_spawn(CedWindowsProcess *process, const char *const args[], const char *cwd,
                       bool terminal, bool merge_stderr, HANDLE stdin_file);
ssize_t ced_windows_read(CedWindowsProcess *process, char *bytes, size_t capacity);
ssize_t ced_windows_write(CedWindowsProcess *process, const char *bytes, size_t length);
bool ced_windows_exited(CedWindowsProcess *process, int *status);
void ced_windows_process_close(CedWindowsProcess *process);
int ced_windows_command(const char *const args[], char **output, size_t *length, int input_fd);
#endif
