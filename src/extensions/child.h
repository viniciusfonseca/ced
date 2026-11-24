#ifndef CED_EXTENSION_CHILD_H
#define CED_EXTENSION_CHILD_H

#include "workspace.h"
#include <sys/types.h>
#ifdef _WIN32
#include "windows_process.h"
typedef CedWindowsProcess CedExtChild;
#else
typedef struct { int stream, errors; pid_t pid; } CedExtChild;
#endif

bool ced_ext_child_start(CedExtChild *child, const char *entrypoint, const char *cwd, char *error);
ssize_t ced_ext_child_read(CedExtChild *child, char *bytes, size_t capacity);
ssize_t ced_ext_child_write(CedExtChild *child, const char *bytes, size_t length);
ssize_t ced_ext_child_stderr(CedExtChild *child, char *bytes, size_t capacity);
void ced_ext_child_wait(CedExtChild *child, bool writing);
bool ced_ext_child_exited(CedExtChild *child);
void ced_ext_child_terminate(CedExtChild *child, bool forced);
void ced_ext_child_close(CedExtChild *child);

#endif
