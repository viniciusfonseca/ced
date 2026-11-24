#include "child.h"

bool ced_ext_child_start(CedExtChild *child, const char *entrypoint, const char *cwd, char *error) {
    static const wchar_t *const allowed[] = {
        L"HOME", L"LANG", L"LC_ALL", L"LC_CTYPE", L"SYSTEMROOT", L"TEMP", L"TMP", L"USERPROFILE", L"WINDIR", NULL
    };
    const char *args[] = {entrypoint, NULL};
    if (ced_windows_spawn_restricted(child, args, cwd, allowed)) return true;
    snprintf(error, CED_ERROR, "Could not launch the Windows extension executable");
    return false;
}

ssize_t ced_ext_child_read(CedExtChild *child, char *bytes, size_t capacity) { return ced_windows_read(child, bytes, capacity); }
ssize_t ced_ext_child_write(CedExtChild *child, const char *bytes, size_t length) { return ced_windows_write(child, bytes, length); }
ssize_t ced_ext_child_stderr(CedExtChild *child, char *bytes, size_t capacity) { return ced_windows_read_stderr(child, bytes, capacity); }

void ced_ext_child_wait(CedExtChild *child, bool writing) {
    if (child->available || (writing && !child->writing)) return;
    DWORD stderr_bytes;
    if (PeekNamedPipe(child->errors, NULL, 0, NULL, &stderr_bytes, NULL) && stderr_bytes) return;
    HANDLE events[3] = {child->process};
    DWORD count = 1;
    if (child->reading) events[count++] = child->read.hEvent;
    if (child->writing) events[count++] = child->write.hEvent;
    WaitForMultipleObjects(count, events, FALSE, 10);
}

bool ced_ext_child_exited(CedExtChild *child) { return ced_windows_exited(child, NULL); }

void ced_ext_child_terminate(CedExtChild *child, bool forced) {
    if (forced) TerminateJobObject(child->job, 1);
}
void ced_ext_child_close(CedExtChild *child) { ced_windows_process_close(child); }
