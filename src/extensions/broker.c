#include "manager.h"
#include "lsp.h"
#include "file_operations.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ced_extensions_authorized(const CedExtensionInstance *i, const CedDocument *doc) {
    if (!i->manager->trusted || !(i->record.granted & CED_EXT_READ)) return false;
    for (size_t n = 0; n < i->grant_count; n++) if (i->document_grants[n] == doc->id) return true;
    char canonical[CED_PATH];
    if (!*doc->path || !realpath(doc->path, canonical)) return false;
    return ced_path_within(canonical, i->manager->workspace);
}

static bool accessible(const CedDocument *doc) {
    return doc->text && ced_utf8_valid(doc->text, strlen(doc->text));
}

static cJSON *info(const CedExtensionDocument *d) {
    CedDocument *doc = d->document;
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    char id[64]; snprintf(id, sizeof id, "doc-%llu", (unsigned long long)doc->id);
    char *uri = *doc->path ? ced_lsp_uri(doc->path) : NULL;
    bool ok = (!*doc->path || uri) && cJSON_AddStringToObject(result, "documentId", id) &&
        ced_ext_json_add(result, "uri", uri ? cJSON_CreateString(uri) : cJSON_CreateNull()) &&
        cJSON_AddStringToObject(result, "languageId", d->language ? d->language : "text") &&
        cJSON_AddNumberToObject(result, "version", (double)doc->version) &&
        cJSON_AddBoolToObject(result, "dirty", ced_document_dirty(doc)) &&
        cJSON_AddBoolToObject(result, "textAccessible", accessible(doc)) &&
        cJSON_AddStringToObject(result, "eol", doc->crlf ? "crlf" : "lf");
    free(uri);
    if (!ok) { cJSON_Delete(result); return NULL; }
    return result;
}

static bool find(CedExtensions *m, uint64_t id, CedExtensionDocument *out) {
    for (size_t n = 0; n < 32 && m->host.document(m->host.user, n, out); n++)
        if (out->document->id == id) return true;
    return false;
}

cJSON *ced_extensions_editor(CedExtensionInstance *i, CedCommandContext context) {
    cJSON *editor = cJSON_CreateObject();
    if (!editor) return NULL;
    CedExtensionDocument d;
    if (context.has_document && find(i->manager, context.document_id, &d) && ced_extensions_authorized(i, d.document)) {
        if (!ced_ext_json_add(editor, "document", info(&d))) goto failed;
        if (accessible(d.document)) {
            cJSON *selection = cJSON_AddObjectToObject(editor, "selection");
            if (!selection || !cJSON_AddNumberToObject(selection, "anchor", (double)context.anchor) ||
                !cJSON_AddNumberToObject(selection, "cursor", (double)context.cursor)) goto failed;
        } else if (!cJSON_AddNullToObject(editor, "selection")) goto failed;
    } else if (!cJSON_AddNullToObject(editor, "document") || !cJSON_AddNullToObject(editor, "selection")) goto failed;
    if (!cJSON_AddNumberToObject(editor, "selectionVersion", (double)context.selection_version)) goto failed;
    return editor;
failed:
    cJSON_Delete(editor); return NULL;
}

static void fail_document(CedExtensionInstance *i, const char *id, int code, const char *reason, const CedDocument *doc) {
    if (!id) { ced_extensions_violation(i, reason); return; }
    const char *key = code == -32602 ? "InvalidParams" : code == -32601 ? "MethodNotFound" :
        code == -32600 ? "InvalidRequest" : code == -32005 ? "ResourceLimit" : reason;
    cJSON *reply = ced_ext_rpc_error(id, code, key, reason);
    if (reply && doc && code == -32002) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply, "error"), "data");
        if (!cJSON_AddNumberToObject(data, "currentVersion", (double)doc->version)) { cJSON_Delete(reply); reply = NULL; }
    }
    ced_extension_process_send(i->process, reply);
}

static void fail(CedExtensionInstance *i, const char *id, int code, const char *reason) { fail_document(i, id, code, reason, NULL); }

static bool plain_text(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p < 32 && *p != '\n' && *p != '\t') || *p == 127) return false;
    return true;
}

static bool valid_context(CedExtensionInstance *i, const cJSON *p) {
    const cJSON *context = cJSON_GetObjectItemCaseSensitive(p, "context");
    const char *session = ced_ext_json_string(context, "sessionId"), *workspace = ced_ext_json_string(context, "workspaceId");
    uint64_t generation;
    return cJSON_IsObject(context) && session && !strcmp(session, i->session) && workspace &&
        !strcmp(workspace, i->manager->workspace_id) &&
        ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(context, "generation"), &generation) && generation == i->manager->generation;
}

static int reason_code(const char *reason) {
    if (!strcmp(reason, "StaleDocument")) return -32002;
    if (!strcmp(reason, "StaleEditorContext")) return -32012;
    if (!strcmp(reason, "PermissionDenied")) return -32001;
    if (!strcmp(reason, "Unavailable")) return -32011;
    if (!strcmp(reason, "ResourceLimit") || !strcmp(reason, "Out of memory")) return -32005;
    return -32602;
}

static bool apply(CedExtensionInstance *i, const char *id, const cJSON *p, CedExtensionDocument d, char *error) {
    cJSON *array = cJSON_GetObjectItemCaseSensitive(p, "edits");
    const char *label = ced_ext_json_string(p, "label"); uint64_t version, selection_version;
    if (!label || !*label || strlen(label) > 512 || !plain_text(label) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(p, "expectedVersion"), &version) || !version ||
        !cJSON_IsArray(array) || cJSON_GetArraySize(array) > CED_EDIT_LIMIT) goto invalid;
    cJSON *expected = cJSON_GetObjectItemCaseSensitive(p, "expectedSelectionVersion");
    CedCommandContext current = i->manager->host.context(i->manager->host.user);
    if (expected && !ced_ext_json_uint(expected, &selection_version)) goto invalid;
    if (version != d.document->version) { snprintf(error, CED_ERROR, "StaleDocument"); return false; }
    if (expected && (current.selection_version != selection_version || current.document_id != d.document->id)) {
        snprintf(error, CED_ERROR, "StaleEditorContext"); return false;
    }
    size_t count = (size_t)cJSON_GetArraySize(array);
    CedTextEdit *edits = count ? calloc(count, sizeof *edits) : NULL;
    if (count && !edits) { snprintf(error, CED_ERROR, "ResourceLimit"); return false; }
    size_t n = 0; cJSON *edit;
    cJSON_ArrayForEach(edit, array) {
        uint64_t start, end; const char *text = ced_ext_json_string(edit, "text");
        if (!cJSON_IsObject(edit) || !text ||
            !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(edit, "start"), &start) ||
            !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(edit, "end"), &end) || start > SIZE_MAX || end > SIZE_MAX) { free(edits); goto invalid; }
        edits[n++] = (CedTextEdit){(size_t)start, (size_t)end, text};
    }
    cJSON *selection = cJSON_GetObjectItemCaseSensitive(p, "selection"); uint64_t anchor = 0, cursor = 0;
    if (selection && (!count || !cJSON_IsObject(selection) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(selection, "anchor"), &anchor) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(selection, "cursor"), &cursor) || anchor > SIZE_MAX || cursor > SIZE_MAX)) { free(edits); goto invalid; }
    CedEditTransaction transaction;
    bool ok = ced_document_prepare_edit(d.document, version, edits, count, d.anchor, d.cursor,
        selection != NULL, (size_t)anchor, (size_t)cursor, &transaction, error);
    free(edits);
    if (!ok) return false;
    bool changed = transaction.changed;
    /* Prepare the response before commit as well. */
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddStringToObject(result, "documentId", ced_ext_json_string(p, "documentId")) ||
        !cJSON_AddNumberToObject(result, "version", (double)(version + (changed ? 1 : 0))) ||
        !cJSON_AddBoolToObject(result, "changed", changed)) {
        cJSON_Delete(result); ced_edit_transaction_free(&transaction);
        snprintf(error, CED_ERROR, "ResourceLimit"); return false;
    }
    CedExtensionMessage *reply = ced_extension_process_prepare(i->process, ced_ext_rpc_result(id, result));
    if (!reply) { ced_edit_transaction_free(&transaction); snprintf(error, CED_ERROR, "ResourceLimit"); return false; }
    ok = i->state == EXT_ACTIVE && valid_context(i, p) && ced_extensions_authorized(i, d.document) &&
        i->manager->host.commit(i->manager->host.user, d.document, version, &transaction, error);
    ced_edit_transaction_free(&transaction);
    if (!ok) { ced_extension_process_discard(i->process, reply); return false; }
    /* No allocation remains between the commit and publishing its reply.
     * A disconnected peer still has an unknown outcome; never replay. */
    ced_extension_process_submit(i->process, reply);
    if (changed) {
        char log[180]; snprintf(log, sizeof log, "%s documents/applyEdits: committed revision %llu\n", id, (unsigned long long)d.document->version);
        ced_extension_process_log(i->process, log);
    }
    return true;
invalid:
    snprintf(error, CED_ERROR, "Invalid params"); return false;
}

void ced_extensions_broker(CedExtensionInstance *i, const cJSON *message) {
    const char *id = ced_ext_json_string(message, "id"), *method = ced_ext_json_string(message, "method");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(message, "params");
    bool has_id = cJSON_GetObjectItemCaseSensitive(message, "id") != NULL;
    const char *rpc = ced_ext_json_string(message, "jsonrpc");
    uint64_t counter = 0;
    char error[CED_ERROR] = {0};
    if (!cJSON_IsObject(message) || !rpc || strcmp(rpc,"2.0") || !method || !*method ||
        cJSON_GetObjectItemCaseSensitive(message,"result") || cJSON_GetObjectItemCaseSensitive(message,"error") ||
        (has_id && !ced_ext_rpc_id(id, "ext:", &counter))) {
        if (!has_id) ced_extensions_violation(i, "InvalidRequest");
        else { ced_extension_process_send(i->process, ced_ext_rpc_error(NULL,-32600,"InvalidRequest","Invalid request")); ced_extensions_stop(i,"shutdown"); i->state=EXT_FAILED; }
        return;
    }
    if (id) {
        if (counter <= i->last_request) {
            fail(i, id, -32600, "Request IDs must increase and must not be reused"); return;
        }
        i->last_request = counter;
    }
    if (!cJSON_IsObject(p)) { fail(i,id,-32602,"Invalid params"); return; }
    if (!valid_context(i, p)) { fail(i, id, -32003, "InvalidContext"); return; }
    if (!strcmp(method, "ced/cancel")) {
        if (id || !ced_ext_rpc_id(ced_ext_json_string(p,"requestId"),"ext:",NULL)) fail(i,id,-32602,"Invalid cancellation");
        return; /* Synchronous commits have no outstanding host job. */
    }
    if (i->state != EXT_ACTIVE) { fail(i, id, -32007, "NotActive"); return; }
    if (!strcmp(method, "log/write")) {
        const char *text = ced_ext_json_string(p, "message");
        const char *level = ced_ext_json_string(p, "level");
        if (!id && text && strlen(text) <= 8192 && plain_text(text) && level &&
            (!strcmp(level,"debug") || !strcmp(level,"info") || !strcmp(level,"warning") || !strcmp(level,"error"))) ced_extension_process_log(i->process, text);
        else fail(i, id, -32602, "Invalid params");
        return;
    }
    if (!id) { ced_extensions_violation(i, "Unknown or request-only notification"); return; }
    if (!ced_rpc_reserve(4 * 1024 * 1024)) { fail(i, id, -32005, "ResourceLimit"); return; }
    cJSON *result = NULL;
    bool replied = false;
    CedDocument *error_document = NULL;
    if (!strcmp(method, "window/showMessage")) {
        const char *text = ced_ext_json_string(p, "message"), *level = ced_ext_json_string(p, "level");
        if (!text || !*text || strlen(text) > 512 || !plain_text(text) || !level || (strcmp(level,"info") && strcmp(level,"warning") && strcmp(level,"error"))) snprintf(error, sizeof error, "Invalid params");
        else {
            double now = ced_ext_now();
            if (now - i->message_window > 10) { i->message_window = now; i->messages = 0; }
            if (i->messages++ >= 5) snprintf(error, sizeof error, "ResourceLimit");
            else {
                char display[CED_ERROR]; snprintf(display, sizeof display, "%.120s: %.380s", i->record.manifest.name, text);
                i->manager->host.message(i->manager->host.user, display); result = cJSON_CreateObject();
            }
        }
    } else if (!strcmp(method, "editor/getContext") || !strcmp(method, "documents/list") || !strcmp(method, "documents/get") || !strcmp(method, "documents/applyEdits")) {
        if (!(i->record.granted & CED_EXT_READ) || (!strcmp(method,"documents/applyEdits") && !(i->record.granted & CED_EXT_EDIT))) snprintf(error, sizeof error, "PermissionDenied");
        else if (!strcmp(method, "editor/getContext")) result = ced_extensions_editor(i, i->manager->host.context(i->manager->host.user));
        else if (!strcmp(method, "documents/list")) {
            result = cJSON_CreateObject(); cJSON *array = cJSON_AddArrayToObject(result, "documents"); CedExtensionDocument d;
            bool ok = result && array;
            for (size_t n = 0; n < 32 && i->manager->host.document(i->manager->host.user, n, &d); n++)
                if (ok && ced_extensions_authorized(i, d.document)) ok = ced_ext_json_append(array, info(&d));
            if (!ok) { cJSON_Delete(result); result = NULL; }
        } else {
            const char *document_id = ced_ext_json_string(p, "documentId"); CedExtensionDocument d; bool found = false;
            if (!document_id || !*document_id || strlen(document_id) > 128) { snprintf(error, sizeof error, "Invalid params"); goto finish; }
            for (size_t n = 0; document_id && n < 32 && i->manager->host.document(i->manager->host.user, n, &d); n++) {
                char expected[64]; snprintf(expected, sizeof expected, "doc-%llu", (unsigned long long)d.document->id);
                if (!strcmp(expected, document_id)) { found = true; break; }
            }
            if (!found) snprintf(error, sizeof error, "Unavailable");
            else if (!ced_extensions_authorized(i, d.document)) snprintf(error, sizeof error, "PermissionDenied");
            else if (!accessible(d.document)) snprintf(error, sizeof error, "Unavailable");
            else if (!strcmp(method, "documents/get")) {
                result = cJSON_CreateObject();
                if (!result || !ced_ext_json_add(result,"document",info(&d)) || !cJSON_AddStringToObject(result,"text",d.document->text)) {
                    cJSON_Delete(result); result = NULL;
                }
            } else { error_document = d.document; replied = apply(i, id, p, d, error); }
        }
    } else {
        bool unsupported = !strncmp(method,"state/",6) || !strncmp(method,"events/",7) || !strncmp(method,"configuration/",14);
        fail(i, id, unsupported ? -32008 : -32601, unsupported ? "UnsupportedCapability" : "Method not found");
        ced_rpc_release(4 * 1024 * 1024); return;
    }
finish:
    if (result) ced_extension_process_send(i->process, ced_ext_rpc_result(id, result));
    else if (!replied) {
        if (!*error) snprintf(error, sizeof error, "ResourceLimit");
        fail_document(i, id, reason_code(error), error, error_document);
    }
    ced_rpc_release(4 * 1024 * 1024);
}
