#include "windows_process.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Quote each argument with the Windows CRT rules, including backslashes before
 * quotes and at the end. No command shell interprets filenames or Git arguments. */
static wchar_t *command_line(const char *const args[]) {
    size_t size = 1;
    for (size_t i = 0; args[i]; i++) size += 2 * strlen(args[i]) + 4;
    if (size > 32767) return NULL;
    char *line = malloc(size); if (!line) return NULL;
    char *out = line;
    for (size_t i = 0; args[i]; i++) {
        if (i) *out++ = ' ';
        *out++ = '"'; size_t slashes = 0;
        for (const char *p = args[i];; p++) {
            if (*p == '\\') { slashes++; continue; }
            size_t count = (*p == '"' || !*p) ? slashes * 2 : slashes;
            while (count--) *out++ = '\\';
            slashes = 0;
            if (!*p) break;
            if (*p == '"') *out++ = '\\';
            *out++ = *p;
        }
        *out++ = '"';
    }
    *out = 0;
    wchar_t *wide = ced_windows_wide(line); free(line); return wide;
}

bool ced_windows_relaunch(const char *const args[]) {
    wchar_t executable[32768];
    DWORD length = GetModuleFileNameW(NULL, executable, sizeof executable / sizeof *executable);
    if (!length || length >= sizeof executable / sizeof *executable) return false;
    wchar_t *line = command_line(args);
    if (!line) return false;
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION child = {0};
    bool started = CreateProcessW(executable, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child);
    free(line);
    if (started) {
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
    return started;
}

/* Parent ends support overlapped I/O; child/ConPTY ends remain synchronous. */
static bool pipe_pair(HANDLE *parent, HANDLE *child, bool writing) {
    static LONG serial;
    wchar_t name[128];
    swprintf(name, 128, L"\\\\.\\pipe\\ced-%lu-%ld", GetCurrentProcessId(), InterlockedIncrement(&serial));
    DWORD access = writing ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
    *parent = CreateNamedPipeW(name, access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, NULL);
    if (*parent == INVALID_HANDLE_VALUE) { *parent = NULL; return false; }
    SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
    *child = CreateFileW(name, writing ? GENERIC_READ : GENERIC_WRITE, 0, &security, OPEN_EXISTING, 0, NULL);
    if (*child == INVALID_HANDLE_VALUE) { *child = NULL; CloseHandle(*parent); *parent = NULL; return false; }
    /* The synchronous client connection has already completed. */
    return true;
}

/* GetEnvironmentStringsW supplies the sorted native block. Copy only the
 * selected variables, preserving their UTF-16 values and order. */
static wchar_t *restricted_environment(const wchar_t *const allowed[]) {
    wchar_t *inherited = GetEnvironmentStringsW();
    if (!inherited) return NULL;
    size_t capacity = 2;
    for (const wchar_t *entry = inherited; *entry; entry += wcslen(entry) + 1)
        capacity += wcslen(entry) + 1;
    wchar_t *block = calloc(capacity, sizeof *block);
    size_t used = 0;
    for (const wchar_t *entry = inherited; block && *entry; entry += wcslen(entry) + 1) {
        const wchar_t *separator = wcschr(entry, L'=');
        if (!separator || separator == entry) continue;
        size_t length = (size_t)(separator - entry);
        for (size_t i = 0; allowed[i]; i++) {
            if (wcslen(allowed[i]) != length || _wcsnicmp(entry, allowed[i], length)) continue;
            size_t count = wcslen(entry) + 1;
            memcpy(block + used, entry, count * sizeof *block);
            used += count;
            break;
        }
    }
    FreeEnvironmentStringsW(inherited);
    return block;
}

static bool spawn(CedWindowsProcess *p, const char *const args[], const char *cwd,
                  bool terminal, bool merge_stderr, bool capture_stderr, HANDLE stdin_file,
                  const wchar_t *const allowed[]) {
    memset(p, 0, sizeof *p);
    HANDLE child_input = NULL, child_output = NULL, child_error = NULL;
    wchar_t *command = command_line(args), *directory = cwd ? ced_windows_wide(cwd) : NULL;
    wchar_t *environment = allowed ? restricted_environment(allowed) : NULL;
    wchar_t *application = allowed ? ced_windows_wide(args[0]) : NULL;
    STARTUPINFOEXW startup = {0}; PROCESS_INFORMATION info = {0};
    bool ok = command && (!cwd || directory) && (!allowed || (environment && application));
    if (ok && stdin_file) {
        ok = DuplicateHandle(GetCurrentProcess(), stdin_file, GetCurrentProcess(), &child_input, 0, TRUE, DUPLICATE_SAME_ACCESS);
    } else if (ok) ok = pipe_pair(&p->input, &child_input, true);
    if (ok) ok = pipe_pair(&p->output, &child_output, false);
    p->read.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    p->write.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = ok && p->read.hEvent && p->write.hEvent;
    if (ok && terminal) ok = SUCCEEDED(CreatePseudoConsole((COORD){80, 24}, child_input, child_output, 0, &p->console));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    startup.lpAttributeList = malloc(bytes);
    bool attributes = startup.lpAttributeList && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes);
    ok = ok && attributes;
    HANDLE inherited[3] = {child_input, child_output, NULL};
    if (ok && terminal) {
        /* NULL standard handles let ConPTY supply its own console handles.
         * Without this flag, redirected parent handles can be duplicated into
         * the child even when CreateProcess disables handle inheritance. */
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       p->console, sizeof p->console, NULL, NULL);
    } else if (ok) {
        SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
        if (!merge_stderr && capture_stderr) {
            ok=CreatePipe(&p->errors,&child_error,&security,65536) &&
                SetHandleInformation(p->errors,HANDLE_FLAG_INHERIT,0);
        } else if (!merge_stderr) {
            child_error=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,NULL);
            if (child_error==INVALID_HANDLE_VALUE) { child_error=NULL; ok=false; }
        }
        inherited[2] = child_error;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input;
        startup.StartupInfo.hStdOutput = child_output;
        startup.StartupInfo.hStdError = merge_stderr ? child_output : child_error;
        ok = ok && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                         inherited, (merge_stderr ? 2 : 3) * sizeof(HANDLE), NULL, NULL);
    }
    p->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ok = ok && p->job && SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    startup.StartupInfo.cb = sizeof startup;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (!terminal) flags |= CREATE_NO_WINDOW;
    if (ok) ok = CreateProcessW(application, command, NULL, NULL, !terminal, flags, environment, directory, &startup.StartupInfo, &info);
    if (ok) {
        p->process = info.hProcess;
        ok = AssignProcessToJobObject(p->job, p->process) && ResumeThread(info.hThread) != (DWORD)-1;
        if (!ok) TerminateProcess(p->process, 1);
        CloseHandle(info.hThread);
    }
    if (attributes) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(command); free(directory); free(environment); free(application);
    if (child_input) CloseHandle(child_input);
    if (child_output) CloseHandle(child_output);
    if (child_error) CloseHandle(child_error);
    if (!ok) ced_windows_process_close(p);
    return ok;
}

bool ced_windows_spawn(CedWindowsProcess *p, const char *const args[], const char *cwd,
                       bool terminal, bool merge_stderr, bool capture_stderr, HANDLE stdin_file) {
    return spawn(p, args, cwd, terminal, merge_stderr, capture_stderr, stdin_file, NULL);
}

bool ced_windows_spawn_restricted(CedWindowsProcess *p, const char *const args[], const char *cwd,
                                  const wchar_t *const allowed[]) {
    return spawn(p, args, cwd, false, false, true, NULL, allowed);
}

ssize_t ced_windows_read_stderr(CedWindowsProcess *p, char *bytes, size_t capacity) {
    DWORD available, read=0;
    if (p->errors && PeekNamedPipe(p->errors,NULL,0,NULL,&available,NULL) && available)
        ReadFile(p->errors,bytes,(DWORD)(capacity<available ? capacity : available),&read,NULL);
    return (ssize_t)read;
}

ssize_t ced_windows_read(CedWindowsProcess *p, char *bytes, size_t capacity) {
    if (!p->output) { errno = EPIPE; return -1; }
    if (!p->available) {
        DWORD count = 0; BOOL ok;
        if (p->reading) {
            ok = GetOverlappedResult(p->output, &p->read, &count, FALSE);
            if (!ok && GetLastError() == ERROR_IO_INCOMPLETE) { errno = EAGAIN; return -1; }
            p->reading = false;
        } else {
            ResetEvent(p->read.hEvent);
            ok = ReadFile(p->output, p->incoming, sizeof p->incoming, &count, &p->read);
            if (!ok && GetLastError() == ERROR_IO_PENDING) { p->reading = true; errno = EAGAIN; return -1; }
        }
        if (!ok) { errno = EPIPE; return 0; }
        p->available = count; p->offset = 0;
    }
    size_t count = capacity < p->available ? capacity : p->available;
    memcpy(bytes, p->incoming + p->offset, count); p->offset += (DWORD)count; p->available -= (DWORD)count;
    return (ssize_t)count;
}

ssize_t ced_windows_write(CedWindowsProcess *p, const char *bytes, size_t length) {
    if (!p->input) { errno = EPIPE; return -1; }
    DWORD count = 0; BOOL ok;
    if (p->writing) {
        ok = GetOverlappedResult(p->input, &p->write, &count, FALSE);
        if (!ok && GetLastError() == ERROR_IO_INCOMPLETE) { errno = EAGAIN; return -1; }
        p->writing = false;
    } else {
        DWORD size = (DWORD)(length < sizeof p->outgoing ? length : sizeof p->outgoing);
        memcpy(p->outgoing, bytes, size); ResetEvent(p->write.hEvent);
        ok = WriteFile(p->input, p->outgoing, size, &count, &p->write);
        if (!ok && GetLastError() == ERROR_IO_PENDING) { p->writing = true; errno = EAGAIN; return -1; }
    }
    if (!ok) { errno = EPIPE; return -1; }
    return (ssize_t)count;
}

bool ced_windows_exited(CedWindowsProcess *p, int *status) {
    DWORD code;
    if (!p->process || !GetExitCodeProcess(p->process, &code)) { if (status) *status = -1; return true; }
    if (WaitForSingleObject(p->process,0)==WAIT_TIMEOUT) return false;
    if (status) *status = (int)code;
    return true;
}

void ced_windows_process_close(CedWindowsProcess *p) {
    if (p->job) { TerminateJobObject(p->job, 1); CloseHandle(p->job); }
    if (p->process) { WaitForSingleObject(p->process, 2000); CloseHandle(p->process); }
    DWORD transferred;
    if (p->input) {
        CancelIoEx(p->input, NULL);
        if (p->writing) GetOverlappedResult(p->input,&p->write,&transferred,TRUE);
        CloseHandle(p->input);
    }
    if (p->output) {
        CancelIoEx(p->output, NULL);
        if (p->reading) GetOverlappedResult(p->output,&p->read,&transferred,TRUE);
        CloseHandle(p->output);
    }
    if (p->errors) CloseHandle(p->errors);
    if (p->console) ClosePseudoConsole(p->console);
    if (p->read.hEvent) CloseHandle(p->read.hEvent);
    if (p->write.hEvent) CloseHandle(p->write.hEvent);
    memset(p, 0, sizeof *p);
}

int ced_windows_command(const char *const args[], char **output, size_t *length, int input_fd) {
    CedWindowsProcess *p = calloc(1, sizeof *p); *output = NULL;
    if (length) *length = 0;
    HANDLE input = input_fd >= 0 ? (HANDLE)_get_osfhandle(input_fd) : CreateFileW(L"NUL", GENERIC_READ,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    bool ok = p && input != INVALID_HANDLE_VALUE && ced_windows_spawn(p, args, NULL, false, true, false, input);
    if (input_fd < 0 && input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!ok) { free(p); return -1; }
    size_t used = 0, capacity = 8192; const size_t limit = 8 * 1024 * 1024;
    char *out = malloc(capacity), bytes[8192]; bool truncated = !out; int status = -1;
    for (;;) {
        ssize_t n = ced_windows_read(p, bytes, sizeof bytes);
        if (n > 0 && !truncated) {
            size_t needed = used + (size_t)n + 1;
            if (needed > limit + 1) truncated = true;
            else if (needed > capacity) {
                size_t next = capacity * 2; if (next < needed) next = needed;
                char *larger = realloc(out, next);
                if (!larger) truncated = true; else { out = larger; capacity = next; }
            }
            if (!truncated) { memcpy(out + used, bytes, (size_t)n); used += (size_t)n; }
        }
        if (!n) break;
        if (n < 0) {
            if (errno != EAGAIN) { truncated = true; break; }
            WaitForSingleObject(p->read.hEvent, 20);
        }
    }
    WaitForSingleObject(p->process, INFINITE); ced_windows_exited(p, &status);
    ced_windows_process_close(p); free(p);
    if (out) { out[used] = 0; *output = out; }
    if (length) *length = used;
    return truncated ? -1 : status;
}
