#include "lsp.h"
#include "output.h"
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
    bool ready, running, shutting_down, incremental, project_warning;
    CedLspLanguage language;
    char label[64], root[CED_PATH];
    cJSON *settings;
#ifndef _WIN32
    char maven_directory[48];
    int stderr_fd, log_fd;
#endif
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

CedLspLanguage ced_lsp_language_kind(const char *path) {
    const char *language=ced_language(path);
    if (!strcmp(language,"c") || !strcmp(language,"cpp")) return CED_LSP_C;
    if (!strcmp(language,"java")) return CED_LSP_JAVA;
    if (!strcmp(language,"kotlin")) return CED_LSP_KOTLIN;
    return CED_LSP_NONE;
}

bool ced_lsp_language(const char *path) { return ced_lsp_language_kind(path)!=CED_LSP_NONE; }

static Document *document(CedLsp *lsp, const char *path) {
    for (int i = 0; i < lsp->document_count; i++) if (!strcmp(lsp->documents[i]->path, path)) return lsp->documents[i];
    return NULL;
}

static void status_text(CedLsp *lsp, const char *status) {
    snprintf(lsp->status,sizeof lsp->status,"%s: %s",lsp->label,status);
}

static void stopped(CedLsp *lsp, const char *status) {
    lsp->ready=false; lsp->running=false;
    status_text(lsp,status);
    if (!lsp->shutting_down) ced_output_text(lsp->label,status);
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
        if (item(message, "error")) {
            ced_output_text(lsp->label,string(item(item(message,"error"),"message")));
            stopped(lsp,"initialization failed"); return;
        }
        cJSON *encoding = item(item(item(message, "result"), "capabilities"), "positionEncoding");
        if (cJSON_IsString(encoding) && strcmp(string(encoding), "utf-16")) { stopped(lsp, "unsupported position encoding"); return; }
        cJSON *sync=item(item(item(message,"result"),"capabilities"),"textDocumentSync");
        lsp->incremental=number(cJSON_IsObject(sync) ? item(sync,"change") : sync)==2;
        lsp->ready = send_message(lsp, "initialized", cJSON_CreateObject(), 0);
        status_text(lsp,lsp->ready ? (lsp->language==CED_LSP_JAVA ? "indexing" : "ready") : "failed");
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

static cJSON *workspace_folders(const char *root) {
    cJSON *folders=cJSON_CreateArray(), *folder=cJSON_CreateObject();
    char *uri=ced_lsp_uri(root);
    if (uri) cJSON_AddStringToObject(folder,"uri",uri);
    free(uri);
    cJSON_AddStringToObject(folder,"name",ced_basename(root));
    add_element(folders,folder);
    return folders;
}

static const cJSON *configuration_section(const cJSON *settings, const char *section) {
    for (const char *end; *section; section=end+1) {
        end=strchr(section,'.');
        if (!end) return item(settings,section);
        const cJSON *value=NULL;
        for (const cJSON *entry=settings ? settings->child : NULL;entry;entry=entry->next)
            if (entry->string && strlen(entry->string)==(size_t)(end-section) &&
                !strncmp(entry->string,section,(size_t)(end-section))) { value=entry; break; }
        settings=value;
    }
    return settings;
}

static void handle(CedLsp *lsp, const char *body, size_t length) {
    cJSON *message = cJSON_ParseWithLength(body, length);
    if (!message) { stopped(lsp, "invalid JSON"); return; }
    const char *method = string(item(message, "method"));
    const cJSON *params=item(message,"params");
    if (!strcmp(method,"window/logMessage") || !strcmp(method,"window/showMessage") ||
        !strcmp(method,"window/showMessageRequest") || !strcmp(method,"intellij/importLog"))
        ced_output_text(lsp->label,string(item(params,"message")));
    if (!strcmp(method,"textDocument/publishDiagnostics")) diagnostic_message(lsp,item(message,"params"));
    else if (lsp->language==CED_LSP_JAVA && !strcmp(method,"language/status")) {
        const char *type=string(item(params,"type"));
        if (!strcmp(type,"ProjectStatus")) {
            lsp->project_warning=strcmp(string(item(params,"message")),"OK")!=0;
        } else if (!strcmp(type,"ServiceReady")) status_text(lsp,lsp->project_warning ? "check project import" : "ready");
        else if (!strcmp(type,"Starting") || !strcmp(type,"Started")) status_text(lsp,"indexing");
        else if (!strcmp(type,"Error")) status_text(lsp,"project import failed");
    } else if (lsp->language==CED_LSP_KOTLIN && !strcmp(method,"intellij/importLog")) {
        if (cJSON_IsTrue(item(params,"failed"))) status_text(lsp,"project import failed");
        else if (cJSON_IsTrue(item(params,"succeeded"))) status_text(lsp,"ready");
        else if (cJSON_IsTrue(item(params,"started"))) status_text(lsp,"importing");
    } else if (*method && item(message, "id")) {
        /* Reply to server requests instead of leaving the server waiting. */
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
        add_item(reply, "id", cJSON_Duplicate(item(message, "id"), false));
        if (!strcmp(method, "workspace/configuration")) {
            cJSON *values = cJSON_CreateArray();
            const cJSON *entry;
            cJSON_ArrayForEach(entry,item(item(message,"params"),"items")) {
                const cJSON *value=configuration_section(lsp->settings,string(item(entry,"section")));
                add_element(values,value ? cJSON_Duplicate(value,true) : cJSON_CreateNull());
            }
            add_item(reply, "result", values);
        } else if (!strcmp(method,"workspace/workspaceFolders")) add_item(reply,"result",workspace_folders(lsp->root));
        else if (!strcmp(method, "client/registerCapability") || !strcmp(method, "client/unregisterCapability") ||
            !strcmp(method,"window/showMessageRequest") || !strcmp(method, "window/workDoneProgress/create")) cJSON_AddNullToObject(reply, "result");
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

static void drain_stderr(CedLsp *lsp) {
    char bytes[8192];
    for (int i=0;i<4;i++) {
#ifdef _WIN32
        ssize_t n=ced_windows_read_stderr(&lsp->process,bytes,sizeof bytes);
#else
        if (lsp->stderr_fd<0) return;
        ssize_t n=read(lsp->stderr_fd,bytes,sizeof bytes);
        if (n<0 && errno==EINTR) continue;
        if (!n) { close(lsp->stderr_fd); lsp->stderr_fd=-1; }
#endif
        if (n<=0) break;
        ced_output_write(lsp->label,bytes,(size_t)n);
#ifndef _WIN32
        if (lsp->log_fd>=0) {
            for (ssize_t offset=0;offset<n;) {
                ssize_t written=write(lsp->log_fd,bytes+offset,(size_t)(n-offset));
                if (written<0 && errno==EINTR) continue;
                if (written<=0) break;
                offset+=written;
            }
        }
#endif
    }
}

void ced_lsp_poll(CedLsp *lsp) {
    if (!lsp) return;
    drain_stderr(lsp);
    if (!lsp->running) return;
    if (!lsp->ready && !lsp->shutting_down && now() - lsp->initialize_started > (lsp->language==CED_LSP_C ? 10 : 180)) {
        stopped(lsp, "initialization timed out"); return;
    }
    if (lsp->request_id && now() - lsp->request_started > (lsp->language==CED_LSP_C ? 10 : 45)) {
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
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { drain_stderr(lsp); stopped(lsp, "disconnected"); return; }
    }
    if (!lsp->output_length) { free(lsp->output); lsp->output = NULL; }
    char bytes[65536];
#ifdef _WIN32
    ssize_t n = ced_windows_read(&lsp->process,bytes,sizeof bytes);
#else
    ssize_t n = recv(lsp->fd, bytes, sizeof bytes, 0);
#endif
    if (n > 0) {
        if ((size_t)n > MESSAGE_CAP - lsp->input_length) { stopped(lsp, "message too large"); return; }
        size_t needed = lsp->input_length + (size_t)n + 1;
        if (needed > lsp->input_capacity) {
            size_t capacity = lsp->input_capacity ? lsp->input_capacity * 2 : 65536;
            if (capacity < needed) capacity = needed;
            if (capacity > MESSAGE_CAP + 1) capacity = MESSAGE_CAP + 1;
            char *input = realloc(lsp->input, capacity);
            if (!input) { stopped(lsp, "out of memory"); return; }
            lsp->input = input;
            lsp->input_capacity = capacity;
        }
        char *input = lsp->input;
        memcpy(input + lsp->input_length, bytes, (size_t)n); lsp->input_length += (size_t)n; input[lsp->input_length] = 0;
    } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { drain_stderr(lsp); stopped(lsp, "stopped"); return; }
    for (int i = 0; i < 32 && lsp->input_length; i++) {
        char *separator = strstr(lsp->input, "\r\n\r\n");
        if (!separator) { if (lsp->input_length > 8192) stopped(lsp, "invalid header"); break; }
        size_t header_length = (size_t)(separator - lsp->input) + 4;
        char *length_field = strstr(lsp->input, "Content-Length:");
        if (!length_field || length_field >= separator) { stopped(lsp, "missing Content-Length"); break; }
        char *end; errno = 0;
        unsigned long length = strtoul(length_field + 15, &end, 10);
        if (errno || end == length_field + 15 || end > separator || !length || length > MESSAGE_CAP - header_length) { stopped(lsp, "invalid Content-Length"); break; }
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
    if (!executable || !*executable) executable="clangd";
    const char *args[]={executable,"--background-index","--log=error","--header-insertion=never",NULL};
    CedLspServer server={.root=root,.label="clangd",.language=CED_LSP_C,.argv=args};
    return ced_lsp_start_server(&server,error,cap);
}

static bool kotlin_maven_project(CedLsp *lsp, cJSON *options, const char *settings) {
    char args[CED_PATH + 12], pom[CED_PATH];
#ifdef _WIN32
    /* Maven splits MAVEN_ARGS on whitespace. Use the short path when Windows
     * provides one for a settings file whose native path contains spaces. */
    wchar_t *path = ced_windows_wide(settings), alias[CED_PATH];
    DWORD length = path ? GetShortPathNameW(path, alias, CED_PATH) : 0;
    free(path);
    char *native = length && length < CED_PATH ? ced_windows_utf8(alias) : NULL;
    bool valid = native && !strpbrk(native, " \t") && strlen(native) < CED_PATH;
    if (valid) snprintf(args, sizeof args, "--settings=%s", native);
    free(native);
    if (!valid) return false;
#else
    char directory[]="/tmp/ced-maven-XXXXXX";
    if (!mkdtemp(directory)) return false;
    strcpy(lsp->maven_directory,directory);
    char alias[64];
    if (!ced_join(alias,sizeof alias,directory,"settings.xml") || symlink(settings,alias)) return false;
    snprintf(args,sizeof args,"--settings=%s",alias);
#endif
    if (!ced_join(pom,sizeof pom,lsp->root,"pom.xml")) return false;
    char *uri=ced_lsp_uri(pom);
    cJSON *projects=cJSON_AddArrayToObject(options,"projects");
    cJSON *project=cJSON_CreateObject();
    bool ok=uri && projects && project && cJSON_AddStringToObject(project,"type","maven") &&
        cJSON_AddStringToObject(project,"path",uri);
    if (ok) {
        cJSON *env=cJSON_AddObjectToObject(project,"env");
        ok=env && cJSON_AddStringToObject(env,"MAVEN_ARGS",args) && cJSON_AddItemToArray(projects,project);
    }
    if (!ok) cJSON_Delete(project);
    free(uri);
    return ok;
}

/* The launcher needs tool locations and build/cache directories. Keep the
 * command extension's reduced environment independent of the native LSP. */
#ifndef _WIN32
static char *const *server_environment(CedLspLanguage language, char **env) {
    if (language==CED_LSP_C) return environ;
    static const char *const allowed[]={
        "HOME","TMPDIR","LANG","LC_ALL","LC_CTYPE","PATH","JAVA_HOME","GRADLE_USER_HOME",
        "XDG_CACHE_HOME","M2_HOME","MAVEN_HOME",NULL
    };
    size_t count=0;
    for (size_t n=0;allowed[n];n++) {
        size_t length=strlen(allowed[n]);
        for (char **entry=environ;*entry;entry++)
            if (!strncmp(*entry,allowed[n],length) && (*entry)[length]=='=') {
                env[count++]=*entry;
                break;
            }
    }
    env[count]=NULL;
    return env;
}
#endif

CedLsp *ced_lsp_start_server(const CedLspServer *server, char *error, size_t cap) {
    CedLsp *lsp=calloc(1,sizeof *lsp);
    if (!lsp) { snprintf(error,cap,"Out of memory."); return NULL; }
    lsp->fd=-1; lsp->next_id=1; lsp->language=server->language;
#ifndef _WIN32
    lsp->stderr_fd=lsp->log_fd=-1;
#endif
    snprintf(lsp->label,sizeof lsp->label,"%s",server->label);
    snprintf(lsp->root,sizeof lsp->root,"%s",server->root);
    lsp->settings=server->settings_json ? cJSON_Parse(server->settings_json) : NULL;
#ifdef _WIN32
    static const wchar_t *const allowed[] = {
        L"APPDATA", L"COMSPEC", L"GRADLE_USER_HOME", L"HOME", L"JAVA_HOME", L"LANG",
        L"LC_ALL", L"LC_CTYPE", L"LOCALAPPDATA", L"M2_HOME", L"MAVEN_HOME", L"PATH",
        L"PATHEXT", L"SYSTEMROOT", L"TEMP", L"TMP", L"USERPROFILE", L"WINDIR", L"XDG_CACHE_HOME", NULL
    };
    bool spawned = server->language == CED_LSP_C
        ? ced_windows_spawn(&lsp->process,server->argv,server->root,false,false,true,NULL)
        : ced_windows_spawn_restricted(&lsp->process,server->argv,server->root,allowed);
    int result=spawned ? 0 : ENOENT;
#else
    int sockets[2];
    if (socketpair(AF_UNIX,SOCK_STREAM,0,sockets)) {
        snprintf(error,cap,"Could not create LSP connection."); ced_lsp_stop(lsp); return NULL;
    }
    int errors[2];
    if (pipe(errors)) {
        close(sockets[0]); close(sockets[1]); ced_lsp_stop(lsp);
        snprintf(error,cap,"Could not capture language server output."); return NULL;
    }
    if (fcntl(errors[0],F_SETFL,O_NONBLOCK)<0 || fcntl(errors[0],F_SETFD,FD_CLOEXEC)<0 ||
        fcntl(errors[1],F_SETFD,FD_CLOEXEC)<0) {
        close(errors[0]); close(errors[1]); close(sockets[0]); close(sockets[1]); ced_lsp_stop(lsp);
        snprintf(error,cap,"Could not configure language server output."); return NULL;
    }
    fcntl(sockets[0],F_SETFD,FD_CLOEXEC); fcntl(sockets[1],F_SETFD,FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int enabled=1; setsockopt(sockets[0],SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof enabled);
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,sockets[1],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions,sockets[1],STDOUT_FILENO);
    const char *log=getenv("CED_LSP_LOG");
    if (log && log[0]=='/') lsp->log_fd=open(log,O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NONBLOCK,0600);
    posix_spawn_file_actions_adddup2(&actions,errors[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,errors[0]);
    posix_spawn_file_actions_addclose(&actions,errors[1]);
    posix_spawn_file_actions_addclose(&actions,sockets[0]);
    posix_spawn_file_actions_addclose(&actions,sockets[1]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setpgroup(&attributes,0);
    short flags=POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    flags|=POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes,flags);
    char *environment[16];
    int result=posix_spawnp(&lsp->pid,server->argv[0],&actions,&attributes,(char *const *)server->argv,
        server_environment(server->language,environment));
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions); close(sockets[1]); close(errors[1]);
    if (result) { close(sockets[0]); close(errors[0]); }
    else { lsp->fd=sockets[0]; lsp->stderr_fd=errors[0]; fcntl(lsp->fd,F_SETFL,O_NONBLOCK); }
#endif
    if (result) {
        snprintf(error,cap,"%s unavailable: %s",server->label,strerror(result));
        ced_lsp_stop(lsp); return NULL;
    }
    lsp->running=true; lsp->initialize_started=now();
    status_text(lsp,"starting");
    ced_output_text(lsp->label,"Starting language server");
    cJSON *params=cJSON_CreateObject();
    cJSON_AddNumberToObject(params,"processId",getpid());
    char *uri=ced_lsp_uri(server->root);
    if (uri) cJSON_AddStringToObject(params,"rootUri",uri);
    free(uri);
    add_item(params,"workspaceFolders",workspace_folders(server->root));
    cJSON *client=cJSON_AddObjectToObject(params,"clientInfo");
    cJSON_AddStringToObject(client,"name","CED");
    cJSON *capabilities=cJSON_Parse("{\"workspace\":{\"configuration\":true,\"workspaceFolders\":true},\"general\":{\"positionEncodings\":[\"utf-16\"]},\"textDocument\":{\"synchronization\":{\"didSave\":true},\"completion\":{\"completionItem\":{\"snippetSupport\":false,\"insertReplaceSupport\":true}},\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},\"publishDiagnostics\":{\"versionSupport\":true}}}");
    add_item(params,"capabilities",capabilities);
    if (server->language!=CED_LSP_C) {
        cJSON *options=cJSON_AddObjectToObject(params,"initializationOptions");
        if (lsp->settings) add_item(options,"settings",cJSON_Duplicate(lsp->settings,true));
        char *root_uri=ced_lsp_uri(server->root);
        cJSON *folders=cJSON_AddArrayToObject(options,"workspaceFolders");
        if (root_uri) add_element(folders,cJSON_CreateString(root_uri));
        free(root_uri);
        /* Maven's launcher splits MAVEN_ARGS on whitespace. The private alias
         * carries the real path without copying its contents into CED or logs. */
        if (server->language==CED_LSP_KOTLIN && server->maven_settings && *server->maven_settings &&
            !kotlin_maven_project(lsp,options,server->maven_settings)) {
            cJSON_Delete(params); ced_lsp_stop(lsp);
            snprintf(error,cap,"Could not prepare the Kotlin Maven settings path."); return NULL;
        }
    }
    if (!send_message(lsp,"initialize",params,1)) {
        ced_lsp_stop(lsp); snprintf(error,cap,"Could not initialize %s.",server->label); return NULL;
    }
    return lsp;
}

bool ced_lsp_sync(CedLsp *lsp, const char *path, const char *text) {
    if (!ced_lsp_ready(lsp) || ced_lsp_language_kind(path)!=lsp->language || !*path) return false;
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
        cJSON_AddStringToObject(td, "languageId", ced_language(path));
        cJSON_AddStringToObject(td, "text", text);
    } else {
        cJSON *changes = cJSON_CreateArray(), *change = cJSON_CreateObject();
        cJSON_AddStringToObject(change,"text",text);
        if (lsp->incremental) {
            CedLspPosition end=ced_lsp_position(doc->text,(int)strlen(doc->text));
            cJSON *range=cJSON_AddObjectToObject(change,"range");
            cJSON *first=cJSON_AddObjectToObject(range,"start"), *last=cJSON_AddObjectToObject(range,"end");
            cJSON_AddNumberToObject(first,"line",0); cJSON_AddNumberToObject(first,"character",0);
            cJSON_AddNumberToObject(last,"line",end.line); cJSON_AddNumberToObject(last,"character",end.character);
        }
        add_element(changes,change);
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
const char *ced_lsp_status(const CedLsp *lsp) { return lsp ? lsp->status : "Language server unavailable"; }
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
    drain_stderr(lsp);
#ifdef _WIN32
    ced_windows_process_close(&lsp->process);
#else
    if (lsp->fd >= 0) close(lsp->fd);
    int status;
    if (lsp->pid > 0 && waitpid(lsp->pid, &status, WNOHANG) == 0) {
        kill(-lsp->pid, SIGTERM);
        bool exited = false;
        for (int i = 0; i < 20; i++) {
            if (waitpid(lsp->pid, &status, WNOHANG) != 0) { exited = true; break; }
            struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
        }
        if (!exited) { kill(-lsp->pid, SIGKILL); while (waitpid(lsp->pid, &status, 0) < 0 && errno == EINTR) {} }
    }
    if (lsp->pid > 0) kill(-lsp->pid, SIGKILL);
    drain_stderr(lsp);
    if (lsp->stderr_fd>=0) close(lsp->stderr_fd);
    if (lsp->log_fd>=0) close(lsp->log_fd);
    if (*lsp->maven_directory) {
        char alias[64];
        if (ced_join(alias,sizeof alias,lsp->maven_directory,"settings.xml")) unlink(alias);
        rmdir(lsp->maven_directory);
    }
#endif
    for (int i = 0; i < lsp->document_count; i++) {
        free(lsp->documents[i]->text);
        free(lsp->documents[i]);
    }
    cJSON_Delete(lsp->settings); free(lsp->input); free(lsp->output); free(lsp);
}
