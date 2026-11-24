#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "child.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int spawn_extension(CedExtChild *child, const char *entrypoint, const char *cwd) {
    int sockets[2], errors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return errno;
    if (pipe(errors)) { int code = errno; close(sockets[0]); close(sockets[1]); return code; }
    for (unsigned i = 0; i < 2; i++) { fcntl(sockets[i], F_SETFD, FD_CLOEXEC); fcntl(errors[i], F_SETFD, FD_CLOEXEC); }
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    posix_spawn_file_actions_addchdir_np(&actions, cwd);
#ifdef __linux__
    DIR *descriptors = opendir("/proc/self/fd");
    if (!descriptors) { posix_spawn_file_actions_destroy(&actions); for (unsigned i=0;i<2;i++){close(sockets[i]);close(errors[i]);} return errno; }
    struct dirent *descriptor;
    while ((descriptor = readdir(descriptors))) {
        char *end; long fd = strtol(descriptor->d_name, &end, 10);
        if (!*end && fd > STDERR_FILENO && fd != dirfd(descriptors)) posix_spawn_file_actions_addclose(&actions, (int)fd);
    }
    closedir(descriptors);
#endif
    posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags); posix_spawnattr_setpgroup(&attributes, 0);
    char *environment[6] = {0};
    const char *allowed[] = {"HOME", "TMPDIR", "LANG", "LC_ALL", "LC_CTYPE"};
    size_t env_count = 0;
    bool allocated = true;
    for (size_t i = 0; i < 5; i++) {
        const char *value = getenv(allowed[i]);
        if (!value) continue;
        size_t length = strlen(allowed[i]) + strlen(value) + 2;
        environment[env_count] = malloc(length);
        if (!environment[env_count]) { allocated = false; break; }
        snprintf(environment[env_count++], length, "%s=%s", allowed[i], value);
    }
    char *arguments[] = {(char *)entrypoint, NULL};
    int result = allocated ? posix_spawn(&child->pid, entrypoint, &actions, &attributes, arguments, environment) : ENOMEM;
    for (size_t i = 0; i < env_count; i++) free(environment[i]);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(sockets[1]); close(errors[1]);
    if (result) { close(sockets[0]); close(errors[0]); return result; }
    fcntl(sockets[0], F_SETFL, O_NONBLOCK); fcntl(errors[0], F_SETFL, O_NONBLOCK);
    child->stream = sockets[0]; child->errors = errors[0];
    return 0;
}

bool ced_ext_child_start(CedExtChild *child, const char *entrypoint, const char *cwd, char *error) {
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    int status = spawn_extension(child, entrypoint, cwd);
    if (status) snprintf(error, CED_ERROR, "Extension spawn failed: %s", strerror(status));
    return status == 0;
}

ssize_t ced_ext_child_read(CedExtChild *child, char *bytes, size_t capacity) { return read(child->stream, bytes, capacity); }
ssize_t ced_ext_child_write(CedExtChild *child, const char *bytes, size_t length) {
    return write(child->stream, bytes, length > 65536 ? 65536 : length);
}
ssize_t ced_ext_child_stderr(CedExtChild *child, char *bytes, size_t capacity) { return read(child->errors, bytes, capacity); }

void ced_ext_child_wait(CedExtChild *child, bool writing) {
    struct pollfd fds[] = {{child->stream, POLLIN | (writing ? POLLOUT : 0), 0}, {child->errors, POLLIN, 0}};
    poll(fds, 2, 20);
}

bool ced_ext_child_exited(CedExtChild *child) {
    pid_t result = waitpid(child->pid, NULL, WNOHANG);
    return result == child->pid || (result < 0 && errno == ECHILD);
}

void ced_ext_child_terminate(CedExtChild *child, bool forced) { kill(-child->pid, forced ? SIGKILL : SIGTERM); }

void ced_ext_child_close(CedExtChild *child) {
    close(child->stream);
    close(child->errors);
}
