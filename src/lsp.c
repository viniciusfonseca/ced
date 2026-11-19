#include "lsp.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#ifdef _WIN32
#include "windows_process.h"
#else
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MESSAGE_CAP (16 * 1024 * 1024)
#define DOCUMENT_CAP 32

#ifndef _WIN32
extern char **environ;
#endif
typedef struct {
    char path[CED_PATH];
    char *text;
    int version, diagnostic_count;
    CedDiagnostic diagnostics[CED_LSP_DIAGNOSTICS];
} Document;
struct CedLsp {
#ifdef _WIN32
    CedWindowsProcess process;
#endif
    int fd;
    pid_t pid;
    bool ready, running, shutting_down;
    int next_id, next_version, request_id, request_version, shutdown_id;
    double initialize_started, request_started;
    char status[160];
    char *input, *output;
    size_t input_length, output_length, input_capacity;
    Document *documents[DOCUMENT_CAP];
    int document_count;
    CedLspResult result;
};

/* cJSON transfers ownership only when insertion succeeds. */
static void add_item(cJSON *object, const char *name, cJSON *value) {
    if (!cJSON_AddItemToObject(object, name, value)) cJSON_Delete(value);
}

static void add_element(cJSON *array, cJSON *value) {
    if (!cJSON_AddItemToArray(array, value)) cJSON_Delete(value);
}

static cJSON *item(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static const char *string(const cJSON *o) { const char *s = cJSON_GetStringValue(o); return s ? s : ""; }
static int number(const cJSON *o) { return cJSON_IsNumber(o) ? o->valueint : 0; }

static double now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + time.tv_nsec / 1e9;
}

static unsigned codepoint(const char **cursor) {
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned c = *p++;
    int count = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (count) {
        c &= (1u << (6 - count)) - 1;
        for (int i = 0; i < count && *p && (*p & 0xc0) == 0x80; i++) c = (c << 6) | (*p++ & 63);
    }
    *cursor = (const char *)p;
    return c;
}

CedLspPosition ced_lsp_position(const char *text, int offset) {
    CedLspPosition pos = {0};
    const char *p = text;
    while (*p && p - text < offset) {
        unsigned c = codepoint(&p);
        if (c == '\n') { pos.line++; pos.character = 0; }
        else pos.character += c > 0xffff ? 2 : 1;
    }
    return pos;
}

int ced_lsp_offset(const char *text, CedLspPosition pos) {
    const char *p = text;
    int line = 0, column = 0;
    while (*p && line < pos.line) if (*p++ == '\n') line++;
    while (*p && *p != '\n' && column < pos.character) {
        const char *next = p;
        unsigned c = codepoint(&next);
        int width = c > 0xffff ? 2 : 1;
        if (column + width > pos.character) break;
        column += width; p = next;
    }
    return (int)(p - text);
}

char *ced_lsp_uri(const char *path) {
    size_t length = strlen(path);
    char *uri = malloc(length * 3 + 9);
    if (!uri) return NULL;
    char *out = uri;
    memcpy(out, "file://", 7); out += 7;
#ifdef _WIN32
    if (length>1 && path[1]==':') *out++='/';
#endif
    static const char hex[] = "0123456789ABCDEF";
#ifdef _WIN32
    const char *safe="/:-._~";
#else
    const char *safe="/-._~";
#endif
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(safe, *p)) *out++ = (char)*p;
        else { *out++ = '%'; *out++ = hex[*p >> 4]; *out++ = hex[*p & 15]; }
    }
    *out = 0;
    return uri;
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ced_lsp_path(const char *uri, char *out, size_t cap) {
    if (strncmp(uri, "file:///", 8)) return false;
    size_t n = 0;
    const char *start = uri + 7;
#ifdef _WIN32
    if (start[0]=='/' && start[1] && start[2]==':') start++;
#endif
    for (const char *p = start; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') {
            if (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
            c = (unsigned char)((hex(p[1]) << 4) | hex(p[2])); p += 2;
            if (!c) return false;
        }
        if (n + 1 >= cap) return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return true;
}

bool ced_lsp_language(const char *path) {
    const char *language = ced_language(path);
    return !strcmp(language, "c") || !strcmp(language, "cpp");
}

static Document *document(CedLsp *lsp, const char *path) {
    for (int i = 0; i < lsp->document_count; i++) if (!strcmp(lsp->documents[i]->path, path)) return lsp->documents[i];
    return NULL;
}

static void stopped(CedLsp *lsp, const char *status) {
    lsp->ready = false; lsp->running = false;
    snprintf(lsp->status, sizeof lsp->status, "%s", status);
}

/* All I/O is nonblocking. Partial writes stay queued; stdout is framed by
 * Content-Length, so split/coalesced messages are handled identically. */
static bool queue(CedLsp *lsp, cJSON *message) {
    char *body = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (!body) return false;
    size_t length = strlen(body);
    char header[80];
    int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", length);
    if (length + (size_t)n > MESSAGE_CAP - lsp->output_length) { cJSON_free(body); return false; }
    char *out = realloc(lsp->output, lsp->output_length + (size_t)n + length);
    if (!out) { cJSON_free(body); return false; }
    lsp->output = out;
    memcpy(out + lsp->output_length, header, (size_t)n);
    memcpy(out + lsp->output_length + n, body, length);
    lsp->output_length += (size_t)n + length;
    cJSON_free(body);
    return true;
}

static bool send_message(CedLsp *lsp, const char *method, cJSON *params, int id) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "jsonrpc", "2.0");
    cJSON_AddStringToObject(message, "method", method);
    if (params) add_item(message, "params", params);
    if (id) cJSON_AddNumberToObject(message, "id", id);
    return queue(lsp, message);
}

static cJSON *text_document(const char *path) {
    char *uri = ced_lsp_uri(path);
    cJSON *doc = cJSON_CreateObject();
    if (uri) cJSON_AddStringToObject(doc, "uri", uri);
    free(uri);
    return doc;
}

static cJSON *document_params(const char *path) {
    cJSON *params = cJSON_CreateObject();
    add_item(params, "textDocument", text_document(path));
    return params;
}

static CedLspPosition position(const cJSON *o) {
    return (CedLspPosition){number(item(o, "line")), number(item(o, "character"))};
}

static void diagnostic_message(CedLsp *lsp, cJSON *params) {
    char path[CED_PATH];
    if (!ced_lsp_path(string(item(params, "uri")), path, sizeof path)) return;
    Document *doc = document(lsp, path);
    if (!doc) return;
    cJSON *version = item(params, "version");
    if (cJSON_IsNumber(version) && number(version) != doc->version) return;
    doc->diagnostic_count = 0;
    cJSON *diagnostic;
    cJSON_ArrayForEach(diagnostic, item(params, "diagnostics")) {
        if (doc->diagnostic_count == CED_LSP_DIAGNOSTICS) break;
        CedDiagnostic *d = &doc->diagnostics[doc->diagnostic_count++];
        cJSON *range = item(diagnostic, "range");
        d->start = ced_lsp_offset(doc->text, position(item(range, "start")));
        d->end = ced_lsp_offset(doc->text, position(item(range, "end")));
        d->severity = number(item(diagnostic, "severity"));
        snprintf(d->message, sizeof d->message, "%s", string(item(diagnostic, "message")));
    }
}

static void hover_text(cJSON *contents, char *out, size_t cap) {
    if (cJSON_IsString(contents)) snprintf(out, cap, "%s", string(contents));
    else if (cJSON_IsObject(contents)) snprintf(out, cap, "%s", string(item(contents, "value")));
    else if (cJSON_IsArray(contents)) {
        size_t used = 0;
        cJSON *entry;
        cJSON_ArrayForEach(entry, contents) {
            const char *text = cJSON_IsString(entry) ? string(entry) : string(item(entry, "value"));
            if (used + 1 >= cap) break;
            int n = snprintf(out + used, cap - used, "%s%s", used ? "\n\n" : "", text);
            if (n < 0 || (size_t)n >= cap - used) break;
            used += (size_t)n;
        }
    }
}

static void response(CedLsp *lsp, cJSON *message) {
    int id = number(item(message, "id"));
    if (id == 1 && !lsp->shutting_down) {
        if (item(message, "error")) { stopped(lsp, "clangd: initialization failed"); return; }
        cJSON *encoding = item(item(item(message, "result"), "capabilities"), "positionEncoding");
        if (cJSON_IsString(encoding) && strcmp(string(encoding), "utf-16")) { stopped(lsp, "clangd: unsupported position encoding"); return; }
        lsp->ready = send_message(lsp, "initialized", cJSON_CreateObject(), 0);
        snprintf(lsp->status, sizeof lsp->status, "clangd: %s", lsp->ready ? "ready" : "failed");
        return;
    }
    if (id == lsp->shutdown_id && lsp->shutting_down) { send_message(lsp, "exit", NULL, 0); return; }
    if (id != lsp->request_id || !id) return;
    lsp->request_id = 0;
    Document *doc = document(lsp, lsp->result.path);
    if (!doc || doc->version != lsp->request_version) return;
    CedLspResult *r = &lsp->result;
    cJSON *result = item(message, "result"), *error = item(message, "error");
    if (error) snprintf(r->error, sizeof r->error, "%s", string(item(error, "message")));
    else if (r->kind == CED_LSP_HOVER) hover_text(item(result, "contents"), r->hover, sizeof r->hover);
    else if (r->kind == CED_LSP_DEFINITION) {
        if (cJSON_IsArray(result)) result = cJSON_GetArrayItem(result, 0);
        cJSON *uri = item(result, "uri"), *range = item(result, "range");
        if (!uri) { uri = item(result, "targetUri"); range = item(result, "targetSelectionRange"); }
        ced_lsp_path(string(uri), r->target, sizeof r->target);
        r->position = position(item(range, "start"));
    } else {
        cJSON *entries = cJSON_IsArray(result) ? result : item(result, "items"), *entry;
        cJSON_ArrayForEach(entry, entries) {
            if (r->count == CED_LSP_COMPLETIONS) break;
            /* Snippets and auxiliary edits are not advertised by this client. */
            if (number(item(entry, "insertTextFormat")) == 2 || cJSON_GetArraySize(item(entry, "additionalTextEdits"))) continue;
            cJSON *edit = item(entry, "textEdit");
            const char *text = string(item(edit, "newText"));
            if (!*text) text = string(item(entry, "insertText"));
            if (!*text) text = string(item(entry, "label"));
            if (strlen(text) >= sizeof r->completions[0].text) continue;
            CedCompletion *c = &r->completions[r->count++];
            snprintf(c->label, sizeof c->label, "%s", string(item(entry, "label")));
            snprintf(c->detail, sizeof c->detail, "%s", string(item(entry, "detail")));
            snprintf(c->text, sizeof c->text, "%s", text);
            cJSON *range = item(edit, "range");
            if (!range) range = item(edit, "replace");
            if (range) { c->start = ced_lsp_offset(doc->text, position(item(range, "start"))); c->end = ced_lsp_offset(doc->text, position(item(range, "end"))); }
            else {
                c->start = c->end = r->offset;
                while (c->start > 0 && (isalnum((unsigned char)doc->text[c->start - 1]) || doc->text[c->start - 1] == '_')) c->start--;
            }
            /* clangd may return a prefix-only TextEdit even when replace
             * edits are supported. Complete the identifier under the caret
             * rather than leaving its old suffix after the inserted name. */
            if (c->end == r->offset && c->start < c->end) {
                bool identifier = true;
                for (int at = c->start; at < c->end; at++) {
                    unsigned char ch = (unsigned char)doc->text[at];
                    if (!isalnum(ch) && ch != '_' && ch < 128) { identifier = false; break; }
                }
                if (identifier) {
                    unsigned char ch;
                    while ((ch = (unsigned char)doc->text[c->end]) && (isalnum(ch) || ch == '_' || ch >= 128)) c->end++;
                }
            }
        }
    }
    r->ready = true;
}

static void handle(CedLsp *lsp, const char *body, size_t length) {
    cJSON *message = cJSON_ParseWithLength(body, length);
    if (!message) { stopped(lsp, "clangd: invalid JSON"); return; }
    const char *method = string(item(message, "method"));
    if (!strcmp(method, "textDocument/publishDiagnostics")) diagnostic_message(lsp, item(message, "params"));
    else if (*method && item(message, "id")) {
        /* Reply to server requests instead of leaving the server waiting. */
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
        add_item(reply, "id", cJSON_Duplicate(item(message, "id"), false));
        if (!strcmp(method, "workspace/configuration")) {
            cJSON *values = cJSON_CreateArray();
            int count = cJSON_GetArraySize(item(item(message, "params"), "items"));
            for (int i = 0; i < count; i++) add_element(values, cJSON_CreateNull());
            add_item(reply, "result", values);
        } else if (!strcmp(method, "client/registerCapability") || !strcmp(method, "window/workDoneProgress/create")) cJSON_AddNullToObject(reply, "result");
        else {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddNumberToObject(error, "code", -32601);
            cJSON_AddStringToObject(error, "message", "Method not supported");
            add_item(reply, "error", error);
        }
        queue(lsp, reply);
    } else if (!*method) response(lsp, message);
    cJSON_Delete(message);
}

void ced_lsp_poll(CedLsp *lsp) {
    if (!lsp || !lsp->running) return;
    if (!lsp->ready && !lsp->shutting_down && now() - lsp->initialize_started > 10) {
        stopped(lsp, "clangd: initialization timed out"); return;
    }
    if (lsp->request_id && now() - lsp->request_started > 10) {
        lsp->request_id = 0;
        snprintf(lsp->result.error, sizeof lsp->result.error, "Language server request timed out.");
        lsp->result.ready = true;
    }
    if (lsp->output_length) {
#ifndef _WIN32
#ifdef MSG_NOSIGNAL
        int flags = MSG_NOSIGNAL;
#else
        int flags = 0;
#endif
#endif
#ifdef _WIN32
        ssize_t n = ced_windows_write(&lsp->process,lsp->output,lsp->output_length);
#else
        ssize_t n = send(lsp->fd, lsp->output, lsp->output_length, flags);
#endif
        if (n > 0) { memmove(lsp->output, lsp->output + n, lsp->output_length - (size_t)n); lsp->output_length -= (size_t)n; }
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { stopped(lsp, "clangd: disconnected"); return; }
    }
    if (!lsp->output_length) { free(lsp->output); lsp->output = NULL; }
    char bytes[65536];
#ifdef _WIN32
    ssize_t n = ced_windows_read(&lsp->process,bytes,sizeof bytes);
#else
    ssize_t n = recv(lsp->fd, bytes, sizeof bytes, 0);
#endif
    if (n > 0) {
        if ((size_t)n > MESSAGE_CAP - lsp->input_length) { stopped(lsp, "clangd: message too large"); return; }
        size_t needed = lsp->input_length + (size_t)n + 1;
        if (needed > lsp->input_capacity) {
            size_t capacity = lsp->input_capacity ? lsp->input_capacity * 2 : 65536;
            if (capacity < needed) capacity = needed;
            if (capacity > MESSAGE_CAP + 1) capacity = MESSAGE_CAP + 1;
            char *input = realloc(lsp->input, capacity);
            if (!input) { stopped(lsp, "clangd: out of memory"); return; }
            lsp->input = input;
            lsp->input_capacity = capacity;
        }
        char *input = lsp->input;
        memcpy(input + lsp->input_length, bytes, (size_t)n); lsp->input_length += (size_t)n; input[lsp->input_length] = 0;
    } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { stopped(lsp, "clangd: stopped"); return; }
    for (int i = 0; i < 32 && lsp->input_length; i++) {
        char *separator = strstr(lsp->input, "\r\n\r\n");
        if (!separator) { if (lsp->input_length > 8192) stopped(lsp, "clangd: invalid header"); break; }
        size_t header_length = (size_t)(separator - lsp->input) + 4;
        char *length_field = strstr(lsp->input, "Content-Length:");
        if (!length_field || length_field >= separator) { stopped(lsp, "clangd: missing Content-Length"); break; }
        char *end; errno = 0;
        unsigned long length = strtoul(length_field + 15, &end, 10);
        if (errno || end == length_field + 15 || end > separator || !length || length > MESSAGE_CAP - header_length) { stopped(lsp, "clangd: invalid Content-Length"); break; }
        if (header_length + length > lsp->input_length) break;
        handle(lsp, lsp->input + header_length, length);
        size_t consumed = header_length + length;
        memmove(lsp->input, lsp->input + consumed, lsp->input_length - consumed);
        lsp->input_length -= consumed; lsp->input[lsp->input_length] = 0;
    }
    if (!lsp->input_length) {
        free(lsp->input); lsp->input = NULL; lsp->input_capacity = 0;
    }
}

CedLsp *ced_lsp_start(const char *root, const char *executable, char *error, size_t cap) {
    CedLsp *lsp = calloc(1, sizeof *lsp);
    if (!lsp) { snprintf(error, cap, "Out of memory."); return NULL; }
    lsp->fd = -1; lsp->next_id = 1;
#ifdef _WIN32
    if (!executable || !*executable) executable="clangd";
    const char *args[]={executable,"--background-index","--log=error","--header-insertion=never",NULL};
    if (!ced_windows_spawn(&lsp->process,args,root,false,false,NULL)) {
        free(lsp); snprintf(error,cap,"Could not start clangd. Install clangd and add it to PATH."); return NULL;
    }
    lsp->running=true; lsp->initialize_started=now();
#else
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) { free(lsp); snprintf(error, cap, "Could not create LSP connection."); return NULL; }
    fcntl(sockets[0], F_SETFD, FD_CLOEXEC); fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int enabled = 1; setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, sockets[0]);
    posix_spawn_file_actions_addclose(&actions, sockets[1]);
    if (!executable || !*executable) executable = "clangd";
    const char *args[] = {executable, "--background-index", "--log=error", "--header-insertion=never", NULL};
    int result = posix_spawnp(&lsp->pid, executable, &actions, NULL, (char *const *)args, environ);
    posix_spawn_file_actions_destroy(&actions); close(sockets[1]);
    if (result) { close(sockets[0]); free(lsp); snprintf(error, cap, "clangd unavailable: %s", strerror(result)); return NULL; }
    lsp->fd = sockets[0]; lsp->running = true; lsp->initialize_started = now();
    fcntl(lsp->fd, F_SETFL, O_NONBLOCK);
#endif
    snprintf(lsp->status, sizeof lsp->status, "clangd: starting");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "processId", getpid());
    char *uri = ced_lsp_uri(root);
    if (uri) cJSON_AddStringToObject(params, "rootUri", uri);
    free(uri);
    cJSON *capabilities = cJSON_Parse("{\"general\":{\"positionEncodings\":[\"utf-16\"]},\"textDocument\":{\"synchronization\":{\"didSave\":true},\"completion\":{\"completionItem\":{\"snippetSupport\":false,\"insertReplaceSupport\":true}},\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},\"publishDiagnostics\":{\"versionSupport\":true}}}");
    add_item(params, "capabilities", capabilities);
    if (!send_message(lsp, "initialize", params, 1)) { ced_lsp_stop(lsp); snprintf(error, cap, "Could not initialize clangd."); return NULL; }
    return lsp;
}

bool ced_lsp_sync(CedLsp *lsp, const char *path, const char *text) {
    if (!ced_lsp_ready(lsp) || !ced_lsp_language(path) || !*path) return false;
    Document *doc = document(lsp, path);
    if (doc && !strcmp(doc->text, text)) return true;
    if (!doc && lsp->document_count == DOCUMENT_CAP) return false;
    char *copy = strdup(text);
    if (!copy) return false;
    Document *created = doc ? NULL : calloc(1, sizeof *created);
    if (!doc && !created) { free(copy); return false; }
    cJSON *params = document_params(path);
    cJSON *td = item(params, "textDocument");
    int version = ++lsp->next_version;
    cJSON_AddNumberToObject(td, "version", version);
    if (!doc) {
        cJSON_AddStringToObject(td, "languageId", !strcmp(ced_language(path), "cpp") ? "cpp" : "c");
        cJSON_AddStringToObject(td, "text", text);
    } else {
        cJSON *changes = cJSON_CreateArray(), *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change, "text", text); add_element(changes, change);
        add_item(params, "contentChanges", changes);
    }
    if (!send_message(lsp, doc ? "textDocument/didChange" : "textDocument/didOpen", params, 0)) { free(copy); free(created); return false; }
    if (!doc) { doc = created; lsp->documents[lsp->document_count++] = doc; snprintf(doc->path, sizeof doc->path, "%s", path); }
    free(doc->text); doc->text = copy; doc->version = version; doc->diagnostic_count = 0;
    if (!strcmp(lsp->result.path, path)) lsp->result.ready = false;
    return true;
}

void ced_lsp_close(CedLsp *lsp, const char *path) {
    if (!lsp) return;
    Document *doc = document(lsp, path);
    if (!doc) return;
    if (lsp->ready) send_message(lsp, "textDocument/didClose", document_params(path), 0);
    int index = 0;
    while (lsp->documents[index] != doc) index++;
    free(doc->text);
    free(doc);
    memmove(lsp->documents + index, lsp->documents + index + 1,
            sizeof *lsp->documents * (size_t)(lsp->document_count - index - 1));
    lsp->documents[--lsp->document_count] = NULL;
}

void ced_lsp_saved(CedLsp *lsp, const char *path) {
    if (ced_lsp_ready(lsp) && document(lsp, path)) send_message(lsp, "textDocument/didSave", document_params(path), 0);
}

bool ced_lsp_request(CedLsp *lsp, CedLspRequest kind, const char *path, const char *text, int offset) {
    if (!ced_lsp_sync(lsp, path, text)) return false;
    if (offset < 0 || (size_t)offset > strlen(text)) return false;
    memset(&lsp->result, 0, sizeof lsp->result);
    lsp->result.kind = kind; lsp->result.offset = offset;
    snprintf(lsp->result.path, sizeof lsp->result.path, "%s", path);
    lsp->request_id = ++lsp->next_id;
    lsp->request_started = now();
    lsp->request_version = document(lsp, path)->version;
    CedLspPosition pos = ced_lsp_position(text, offset);
    cJSON *params = document_params(path), *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "line", pos.line); cJSON_AddNumberToObject(p, "character", pos.character);
    add_item(params, "position", p);
    static const char *methods[] = {"textDocument/completion", "textDocument/hover", "textDocument/definition"};
    return send_message(lsp, methods[kind], params, lsp->request_id);
}

bool ced_lsp_ready(const CedLsp *lsp) { return lsp && lsp->ready && lsp->running; }
bool ced_lsp_running(const CedLsp *lsp) { return lsp && lsp->running; }
bool ced_lsp_pending(const CedLsp *lsp) { return lsp && lsp->request_id != 0; }
const char *ced_lsp_status(const CedLsp *lsp) { return lsp ? lsp->status : "clangd: unavailable"; }
CedLspResult *ced_lsp_result(CedLsp *lsp) { return lsp ? &lsp->result : NULL; }
const CedDiagnostic *ced_lsp_diagnostics(CedLsp *lsp, const char *path, int *count) {
    Document *doc = lsp ? document(lsp, path) : NULL;
    *count = doc ? doc->diagnostic_count : 0;
    return doc ? doc->diagnostics : NULL;
}

void ced_lsp_stop(CedLsp *lsp) {
    if (!lsp) return;
    if (lsp->ready && lsp->running) {
        lsp->shutting_down = true; lsp->shutdown_id = ++lsp->next_id;
        send_message(lsp, "shutdown", NULL, lsp->shutdown_id);
        for (int i = 0; i < 20 && lsp->running; i++) {
            ced_lsp_poll(lsp);
            struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
        }
    }
#ifdef _WIN32
    ced_windows_process_close(&lsp->process);
#else
    if (lsp->fd >= 0) close(lsp->fd);
    int status;
    if (lsp->pid > 0 && waitpid(lsp->pid, &status, WNOHANG) == 0) {
        kill(lsp->pid, SIGTERM);
        bool exited = false;
        for (int i = 0; i < 20; i++) {
            if (waitpid(lsp->pid, &status, WNOHANG) != 0) { exited = true; break; }
            struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
        }
        if (!exited) { kill(lsp->pid, SIGKILL); while (waitpid(lsp->pid, &status, 0) < 0 && errno == EINTR) {} }
    }
#endif
    for (int i = 0; i < lsp->document_count; i++) {
        free(lsp->documents[i]->text);
        free(lsp->documents[i]);
    }
    free(lsp->input); free(lsp->output); free(lsp);
}
