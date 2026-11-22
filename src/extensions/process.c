#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "process_internal.h"
#include "output.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define QUEUE_LIMIT 8
#define QUEUE_BYTES (8 * 1024 * 1024)

static size_t dom_charge(const cJSON *json) {
    size_t size = sizeof *json + 64;
    if (json->string) size += strlen(json->string) + 65;
    if (cJSON_IsString(json) && json->valuestring) size += strlen(json->valuestring) + 65;
    for (const cJSON *child = json->child; child; child = child->next) size += dom_charge(child);
    return size;
}

void ced_extension_message_free(CedExtensionMessage *message) {
    if (!message) return;
    cJSON_Delete(message->json); ced_rpc_release(message->charge); free(message);
}

static CedExtensionMessage *pop(MessageQueue *queue) {
    CedExtensionMessage *message = queue->first;
    if (message) {
        queue->first = message->next; queue->count--; queue->bytes -= message->charge;
        if (!queue->first) queue->last = NULL;
        message->next = NULL;
    }
    return message;
}

static bool push(MessageQueue *queue, CedExtensionMessage *message) {
    if (queue->count == QUEUE_LIMIT || message->charge > QUEUE_BYTES - queue->bytes) return false;
    if (queue->last) queue->last->next = message; else queue->first = message;
    queue->last = message; queue->count++; queue->bytes += message->charge;
    return true;
}

static void set_error(CedExtensionProcess *process, const char *error) {
    pthread_mutex_lock(&process->mutex);
    if (!process->error[0]) snprintf(process->error, sizeof process->error, "%s", error);
    process->stop = true;
    pthread_mutex_unlock(&process->mutex);
}

CedExtensionMessage *ced_extension_process_prepare(CedExtensionProcess *process, cJSON *json) {
    if (!json) return NULL;
    size_t charge = dom_charge(json) + sizeof(CedExtensionMessage) + 64;
    if (charge > QUEUE_BYTES || !ced_rpc_reserve(charge)) { cJSON_Delete(json); return NULL; }
    CedExtensionMessage *message = calloc(1, sizeof *message);
    if (!message) { ced_rpc_release(charge); cJSON_Delete(json); return NULL; }
    message->json = json; message->charge = charge;
    pthread_mutex_lock(&process->mutex);
    bool ok = !process->done && !process->stop && process->outgoing.count + process->reserved_count < QUEUE_LIMIT &&
        charge <= QUEUE_BYTES - process->outgoing.bytes - process->reserved_bytes;
    if (ok) { process->reserved_count++; process->reserved_bytes += charge; }
    pthread_mutex_unlock(&process->mutex);
    if (!ok) { ced_extension_message_free(message); return NULL; }
    return message;
}

void ced_extension_process_discard(CedExtensionProcess *process, CedExtensionMessage *message) {
    if (!message) return;
    pthread_mutex_lock(&process->mutex);
    process->reserved_count--; process->reserved_bytes -= message->charge;
    pthread_mutex_unlock(&process->mutex);
    ced_extension_message_free(message);
}

bool ced_extension_process_submit(CedExtensionProcess *process, CedExtensionMessage *message) {
    pthread_mutex_lock(&process->mutex);
    process->reserved_count--; process->reserved_bytes -= message->charge;
    bool ok = !process->done && !process->stop && push(&process->outgoing, message);
    pthread_mutex_unlock(&process->mutex);
    if (!ok) ced_extension_message_free(message);
    return ok;
}

bool ced_extension_process_send(CedExtensionProcess *process, cJSON *json) {
    CedExtensionMessage *message = ced_extension_process_prepare(process, json);
    if (message && ced_extension_process_submit(process, message)) return true;
    set_error(process, "ResourceLimit: outgoing queue or response allocation"); return false;
}

CedExtensionMessage *ced_extension_process_receive(CedExtensionProcess *process) {
    pthread_mutex_lock(&process->mutex);
    CedExtensionMessage *message = pop(&process->incoming);
    pthread_mutex_unlock(&process->mutex);
    return message;
}

void ced_extension_process_log(CedExtensionProcess *process, const char *text) {
    pthread_mutex_lock(&process->mutex);
    size_t length = strnlen(text, 8191), used = strlen(process->pending_log);
    if (length) {
        size_t newline = text[length - 1] == '\n' ? 0 : 1;
        if (used + length + newline > 8192) {
            size_t drop = used + length + newline - 8192;
            memmove(process->pending_log, process->pending_log + drop, used - drop);
            used -= drop;
        }
        memcpy(process->pending_log + used, text, length); used += length;
        if (newline) process->pending_log[used++] = '\n';
        process->pending_log[used] = 0;
    }
    pthread_mutex_unlock(&process->mutex);
}

void ced_extension_process_stop(CedExtensionProcess *process) {
    if (!process) return;
    pthread_mutex_lock(&process->mutex); process->stop = true; pthread_mutex_unlock(&process->mutex);
}

bool ced_extension_process_done(CedExtensionProcess *process, char *error) {
    pthread_mutex_lock(&process->mutex);
    bool done = process->done;
    if (error) snprintf(error, CED_ERROR, "%s", process->error);
    pthread_mutex_unlock(&process->mutex);
    return done;
}

size_t ced_extension_process_queued(CedExtensionProcess *process) {
    pthread_mutex_lock(&process->mutex);
    size_t bytes = process->incoming.bytes + process->outgoing.bytes + process->reserved_bytes;
    pthread_mutex_unlock(&process->mutex);
    return bytes;
}

static size_t string_size(const char *text) {
    size_t size = 2;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) size += *p < 32 ? 6 : (*p == '\\' || *p == '"') ? 2 : 1;
    return size;
}

static size_t print_size(const cJSON *json) {
    if (cJSON_IsString(json)) return string_size(json->valuestring);
    if (!cJSON_IsObject(json) && !cJSON_IsArray(json)) return 64;
    size_t size = 2;
    for (const cJSON *child = json->child; child; child = child->next) {
        if (cJSON_IsObject(json)) size += string_size(child->string) + 1;
        size += print_size(child) + 1;
    }
    return size;
}

static char *serialize(const cJSON *json, size_t *length) {
    size_t capacity = print_size(json) + 8;
    if (capacity > CED_RPC_OUTPUT_LIMIT || !ced_rpc_reserve(capacity)) return NULL;
    char *body = malloc(capacity);
    char *frame = NULL;
    if (body && cJSON_PrintPreallocated((cJSON *)json, body, (int)capacity, false) && strlen(body) <= CED_RPC_BODY_LIMIT)
        frame = ced_rpc_frame(body, strlen(body), length);
    free(body); ced_rpc_release(capacity);
    return frame;
}

static int log_open(CedExtensionProcess *process, char path[CED_PATH]) {
    char directory[CED_PATH];
    const char *parts[] = {"logs", "extensions", process->session};
    snprintf(directory, sizeof directory, "%s", process->profile);
    for (size_t i = 0; i < 3; i++) {
        char next[CED_PATH];
        if (!ced_join(next, sizeof next, directory, parts[i]) || (mkdir(next, 0700) && errno != EEXIST)) return -1;
        int fd = open(next, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return -1;
        close(fd); snprintf(directory, sizeof directory, "%s", next);
    }
    char name[160]; snprintf(name, sizeof name, "%s.log", process->id);
    if (!ced_join(path, CED_PATH, directory, name)) return -1;
    return open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
}

static void log_write(const char *source, int *fd, const char *path, const char *bytes, size_t length, size_t *total, size_t *rate, double *window) {
    double now = ced_ext_now();
    if (now - *window >= 1) { *window = now; *rate = 0; }
    if (*rate >= 65536) return;
    if (length > 8192) length = 8192;
    if (length > 65536 - *rate) length = 65536 - *rate;
    ced_output_write(source,bytes,length);
    *rate += length;
    if (*fd < 0) return;
    if (*total + length > 1024 * 1024) {
        close(*fd);
        char first[CED_PATH + 4], second[CED_PATH + 4];
        snprintf(first, sizeof first, "%s.1", path); snprintf(second, sizeof second, "%s.2", path);
        unlink(second); rename(first, second); rename(path, first);
        *fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600); *total = 0;
        if (*fd < 0) return;
    }
    char clean[8193];
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        clean[i] = (c >= 32 && c < 127) || c == '\n' || c == '\t' ? (char)c : '?';
    }
    size_t used = 0;
    while (used < length) {
        ssize_t n = write(*fd, clean + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    *total += used;
}

static int spawn_extension(CedExtensionProcess *process, const char *entrypoint, int *socket_fd, int *stderr_fd, pid_t *pid) {
    int sockets[2], errors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return errno;
    if (pipe(errors)) { int code = errno; close(sockets[0]); close(sockets[1]); return code; }
    for (unsigned i = 0; i < 2; i++) { fcntl(sockets[i], F_SETFD, FD_CLOEXEC); fcntl(errors[i], F_SETFD, FD_CLOEXEC); }
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    posix_spawn_file_actions_addchdir_np(&actions, process->package);
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
    int result = allocated ? posix_spawn(pid, entrypoint, &actions, &attributes, arguments, environment) : ENOMEM;
    for (size_t i = 0; i < env_count; i++) free(environment[i]);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    close(sockets[1]); close(errors[1]);
    if (result) { close(sockets[0]); close(errors[0]); return result; }
    fcntl(sockets[0], F_SETFL, O_NONBLOCK); fcntl(errors[0], F_SETFL, O_NONBLOCK);
    *socket_fd = sockets[0]; *stderr_fd = errors[0];
    return 0;
}

static bool receive_frames(CedExtensionProcess *process, CedRpcReader *reader) {
    for (unsigned iteration = 0; iteration < 4; iteration++) {
        char *body; size_t length;
        int result = ced_rpc_next(reader, &body, &length);
        if (!result) return true;
        if (result < 0) { set_error(process, "Invalid extension framing or transport resource limit"); return false; }
        /* Upper bound: decoded strings are no longer than the input, and the
         * parser's token cap bounds DOM nodes and allocation overhead. */
        size_t reservation = length * 2 + CED_EXTENSION_JSON_TOKEN_LIMIT * 256;
        if (!ced_rpc_reserve(reservation)) { free(body); ced_rpc_release(length + 1); set_error(process, "ResourceLimit: parser budget"); return false; }
        char error[CED_ERROR];
        cJSON *json = ced_ext_json_parse(body, length, 64, error);
        free(body); ced_rpc_release(length + 1);
        if (!json) {
            ced_rpc_release(reservation);
            ced_extension_process_send(process, ced_ext_rpc_error(NULL, -32700, "InvalidJSON", error));
            set_error(process, "Invalid extension JSON"); return false;
        }
        size_t charge = dom_charge(json) + sizeof(CedExtensionMessage) + 64;
        if (charge > reservation) { cJSON_Delete(json); ced_rpc_release(reservation); set_error(process, "ResourceLimit: DOM budget"); return false; }
        ced_rpc_release(reservation - charge);
        CedExtensionMessage *message = calloc(1, sizeof *message);
        if (!message) { cJSON_Delete(json); ced_rpc_release(charge); set_error(process, "Out of memory"); return false; }
        message->json = json; message->charge = charge;
        message->body_length = length;
        pthread_mutex_lock(&process->mutex); bool ok = push(&process->incoming, message); pthread_mutex_unlock(&process->mutex);
        if (!ok) { ced_extension_message_free(message); set_error(process, "ResourceLimit: incoming queue"); return false; }
    }
    return true;
}

static void release_process(CedExtensionProcess *process) {
    CedExtensionMessage *message;
    while ((message = pop(&process->incoming))) ced_extension_message_free(message);
    while ((message = pop(&process->outgoing))) ced_extension_message_free(message);
    pthread_mutex_destroy(&process->mutex); free(process);
}

static void *process_worker(void *user) {
    CedExtensionProcess *process = user;
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGPIPE); pthread_sigmask(SIG_BLOCK, &signals, NULL);
    int socket_fd = -1, stderr_fd = -1, lease = -1, log_fd = -1;
    pid_t pid = -1;
    CedRpcReader reader = {0};
    char *output = NULL; size_t output_length = 0, output_at = 0;
    char log_path[CED_PATH] = {0}, error[CED_ERROR];
    CedExtensionRecord verified = {0};
    lease = ced_extension_lease(process->profile, process->id, process->version, false, error);
    if (lease < 0 || !ced_extension_inspect(process->package, &verified, error) || strcmp(verified.hash, process->hash)) {
        set_error(process, "Installed package changed or is unavailable"); goto finished;
    }
    char executable[CED_PATH];
    if (!ced_join(executable, sizeof executable, process->package, verified.manifest.entrypoint)) { set_error(process, "Entrypoint path is too long"); goto finished; }
    pthread_mutex_lock(&process->mutex); bool cancelled = process->stop; pthread_mutex_unlock(&process->mutex);
    if (cancelled) goto finished;
    int spawned = spawn_extension(process, executable, &socket_fd, &stderr_fd, &pid);
    ced_extension_record_free(&verified);
    if (spawned) { snprintf(error, sizeof error, "Extension spawn failed: %s", strerror(spawned)); set_error(process, error); goto finished; }
    log_fd = log_open(process, log_path);
    size_t log_total = 0, log_rate = 0;
    double log_window = 0, stop_time = 0;
    bool term_sent = false, kill_sent = false, child_done = false;
    while (!child_done) {
        char pending_log[8193];
        pthread_mutex_lock(&process->mutex);
        bool stop = process->stop;
        memcpy(pending_log, process->pending_log, sizeof pending_log); process->pending_log[0] = 0;
        CedExtensionMessage *send_message = !output ? pop(&process->outgoing) : NULL;
        pthread_mutex_unlock(&process->mutex);
        if (*pending_log) log_write(process->id,&log_fd, log_path, pending_log, strlen(pending_log), &log_total, &log_rate, &log_window);
        if (stop && !stop_time) stop_time = ced_ext_now();
        double stopping = stop_time ? ced_ext_now() - stop_time : 0;
        if (stop_time && stopping >= 0.5 && !term_sent) { kill(-pid, SIGTERM); term_sent = true; }
        if (stop_time && stopping >= 1 && !kill_sent) { kill(-pid, SIGKILL); kill_sent = true; }
        if (send_message) {
            output = serialize(send_message->json, &output_length); output_at = 0;
            ced_extension_message_free(send_message);
            if (!output) set_error(process, "ResourceLimit: serialization budget");
        }
        struct pollfd descriptors[2] = {{socket_fd, POLLIN | (output ? POLLOUT : 0), 0}, {stderr_fd, POLLIN, 0}};
        int ready = poll(descriptors, 2, 20);
        if (ready < 0 && errno != EINTR) set_error(process, "Extension polling failed");
        if (descriptors[0].revents & POLLOUT) {
            size_t remaining = output_length - output_at;
            if (remaining > 65536) remaining = 65536;
            ssize_t n = write(socket_fd, output + output_at, remaining);
            if (n > 0) output_at += (size_t)n;
            else if (n < 0 && errno != EINTR && errno != EAGAIN) set_error(process, "Extension connection closed");
            if (output_at == output_length) { ced_rpc_frame_free(output, output_length); output = NULL; output_length = 0; }
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            char bytes[16384];
            size_t room = CED_RPC_BODY_LIMIT + CED_RPC_HEADER_LIMIT - reader.length;
            if (room > sizeof bytes) room = sizeof bytes;
            ssize_t n = room ? read(socket_fd, bytes, room) : -1;
            if (n > 0) {
                if (!ced_rpc_feed(&reader, bytes, (size_t)n) || !receive_frames(process, &reader)) set_error(process, "Invalid extension message or queue limit");
            } else if (!n) set_error(process, reader.length ? "Extension exited during a frame" : "Extension connection closed");
            else if (!room || (errno != EINTR && errno != EAGAIN)) set_error(process, "Extension read failed");
        } else if (reader.length) receive_frames(process, &reader);
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            char bytes[8192]; ssize_t n = read(stderr_fd, bytes, sizeof bytes);
            if (n > 0) log_write(process->id,&log_fd, log_path, bytes, (size_t)n, &log_total, &log_rate, &log_window);
        }
        if (descriptors[0].revents & (POLLERR | POLLNVAL)) set_error(process, "Extension transport failed");
        int status;
        pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
            child_done = true;
            /* Descendants must not outlive their extension instance. */
            kill(-pid, SIGKILL);
            pthread_mutex_lock(&process->mutex); bool expected = process->stop; pthread_mutex_unlock(&process->mutex);
            if (!expected) set_error(process, "Extension process exited");
        }
    }
finished:
    ced_extension_record_free(&verified);
    if (output) ced_rpc_frame_free(output, output_length);
    ced_rpc_reader_free(&reader);
    if (socket_fd >= 0) close(socket_fd);
    if (stderr_fd >= 0) close(stderr_fd);
    if (log_fd >= 0) close(log_fd);
    if (lease >= 0) close(lease);
    pthread_mutex_lock(&process->mutex); process->done = true; bool abandoned = process->abandoned; pthread_mutex_unlock(&process->mutex);
    if (abandoned) release_process(process);
    return NULL;
}

CedExtensionProcess *ced_extension_process_start(const char *profile, const CedExtensionRecord *record, const char *session, char *error) {
    CedExtensionProcess *process = calloc(1, sizeof *process);
    if (!process) return NULL;
    snprintf(process->profile, sizeof process->profile, "%s", profile);
    snprintf(process->package, sizeof process->package, "%s", record->package);
    snprintf(process->id, sizeof process->id, "%s", record->manifest.id);
    snprintf(process->version, sizeof process->version, "%s", record->manifest.version);
    snprintf(process->hash, sizeof process->hash, "%s", record->hash);
    snprintf(process->session, sizeof process->session, "%s", session);
    pthread_mutex_init(&process->mutex, NULL);
    if (pthread_create(&process->thread, NULL, process_worker, process)) {
        pthread_mutex_destroy(&process->mutex); free(process); snprintf(error, CED_ERROR, "Could not start extension worker"); return NULL;
    }
    return process;
}

void ced_extension_process_destroy(CedExtensionProcess *process) {
    if (!process) return;
    ced_extension_process_stop(process);
    double deadline = ced_ext_now() + 1.25;
    while (!ced_extension_process_done(process, NULL) && ced_ext_now() < deadline) {
        struct timespec delay = {.tv_nsec = 5000000}; nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&process->mutex);
    bool done = process->done;
    if (!done) process->abandoned = true;
    pthread_mutex_unlock(&process->mutex);
    if (!done) { pthread_detach(process->thread); return; }
    pthread_join(process->thread, NULL); release_process(process);
}
