#include "process_internal.h"
#include "output.h"
#include "child.h"
#include "files.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    CedExtDirectory dirs[4]; size_t count = 0;
    const char *parts[] = {"logs", "extensions", process->session};
    int fd = -1;
    if (!ced_ext_directory(&dirs[0], NULL, process->profile, false)) return -1;
    count++;
    for (size_t i = 0; i < 3; i++) {
        if (!ced_ext_directory(&dirs[count], &dirs[count - 1], parts[i], true)) goto done;
        count++;
    }
    char relative[320], name[160];
    snprintf(name, sizeof name, "%s.log", process->id);
    snprintf(relative, sizeof relative, "logs/extensions/%s/%s", process->session, name);
    if (ced_join(path, CED_PATH, process->profile, relative))
        fd = ced_ext_file(&dirs[count - 1], name, O_WRONLY | O_APPEND | O_CREAT);
done:
    while (count) ced_ext_directory_close(&dirs[--count]);
    return fd;
}

static void log_write(CedExtensionProcess *process, int *fd, char *path, const char *bytes, size_t length, size_t *total, size_t *rate, double *window) {
    double now = ced_ext_now();
    if (now - *window >= 1) { *window = now; *rate = 0; }
    if (*rate >= 65536) return;
    if (length > 8192) length = 8192;
    if (length > 65536 - *rate) length = 65536 - *rate;
    ced_output_write(process->id,bytes,length);
    *rate += length;
    if (*fd < 0) return;
    if (*total + length > 1024 * 1024) {
        close(*fd);
        char first[CED_PATH + 4], second[CED_PATH + 4];
        snprintf(first, sizeof first, "%s.1", path); snprintf(second, sizeof second, "%s.2", path);
        unlink(second); rename(first, second); rename(path, first);
        *fd = log_open(process, path); *total = 0;
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
    CedExtChild child;
    bool started = false;
    int lease = -1, log_fd = -1;
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
    started = ced_ext_child_start(&child, executable, process->package, error);
    ced_extension_record_free(&verified);
    if (!started) { set_error(process, error); goto finished; }
    log_fd = log_open(process, log_path);
    size_t log_total = 0, log_rate = 0;
    double log_window = 0, stop_time = 0;
    bool term_sent = false, kill_sent = false, child_exited = false;
    for (;;) {
        char pending_log[8193];
        pthread_mutex_lock(&process->mutex);
        bool stop = process->stop;
        memcpy(pending_log, process->pending_log, sizeof pending_log); process->pending_log[0] = 0;
        CedExtensionMessage *send_message = !output ? pop(&process->outgoing) : NULL;
        pthread_mutex_unlock(&process->mutex);
        if (*pending_log) log_write(process, &log_fd, log_path, pending_log, strlen(pending_log), &log_total, &log_rate, &log_window);
        if (stop && !stop_time) stop_time = ced_ext_now();
        double stopping = stop_time ? ced_ext_now() - stop_time : 0;
        if (stop_time && stopping >= 0.5 && !term_sent) { ced_ext_child_terminate(&child, false); term_sent = true; }
        if (stop_time && stopping >= 1 && !kill_sent) { ced_ext_child_terminate(&child, true); kill_sent = true; }
        if (send_message) {
            output = serialize(send_message->json, &output_length); output_at = 0;
            ced_extension_message_free(send_message);
            if (!output) set_error(process, "ResourceLimit: serialization budget");
        }
        if (!child_exited) ced_ext_child_wait(&child, output != NULL);
        if (output && !child_exited) {
            ssize_t n = ced_ext_child_write(&child, output + output_at, output_length - output_at);
            if (n > 0) output_at += (size_t)n;
            else if (n < 0 && errno != EINTR && errno != EAGAIN) set_error(process, "Extension connection closed");
            if (output_at == output_length) { ced_rpc_frame_free(output, output_length); output = NULL; output_length = 0; }
        }
        char bytes[16384];
        size_t room = CED_RPC_BODY_LIMIT + CED_RPC_HEADER_LIMIT - reader.length;
        if (room > sizeof bytes) room = sizeof bytes;
        ssize_t received = room ? ced_ext_child_read(&child, bytes, room) : -1;
        if (received > 0) {
            if (!ced_rpc_feed(&reader, bytes, (size_t)received) || !receive_frames(process, &reader)) set_error(process, "Invalid extension message or queue limit");
        } else if (!received || (room && (errno == EINTR || errno == EAGAIN))) {
            if (reader.length) receive_frames(process, &reader);
            if (!received) set_error(process, reader.length ? "Extension exited during a frame" : "Extension connection closed");
        } else set_error(process, "Extension read failed");
        ssize_t n = ced_ext_child_stderr(&child, bytes, 8192);
        if (n > 0) log_write(process, &log_fd, log_path, bytes, (size_t)n, &log_total, &log_rate, &log_window);
        if (child_exited && received <= 0 && n <= 0) break;
        if (!child_exited && ced_ext_child_exited(&child)) {
            child_exited = true;
            ced_ext_child_terminate(&child, true);
            pthread_mutex_lock(&process->mutex); bool expected = process->stop; pthread_mutex_unlock(&process->mutex);
            if (!expected) set_error(process, "Extension process exited");
        }
    }
finished:
    ced_extension_record_free(&verified);
    if (output) ced_rpc_frame_free(output, output_length);
    ced_rpc_reader_free(&reader);
    if (started) ced_ext_child_close(&child);
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
    pthread_t thread = process->thread;
    if (!done) process->abandoned = true;
    pthread_mutex_unlock(&process->mutex);
    if (!done) { pthread_detach(thread); return; }
    pthread_join(thread, NULL); release_process(process);
}
