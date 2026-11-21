#include "manager.h"
#include "lsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const methods[] = {"editor/getContext", "documents/list", "documents/get",
    "documents/applyEdits", "window/showMessage", "log/write", NULL};

static void clear_tab_state(CedExtensionInstance *i) {
    cJSON_Delete(i->invocation_arguments);
    i->invocation_arguments = NULL;
    i->tab_action = false;
    for (size_t n = 0; n < i->record.manifest.tab_count; n++) {
        cJSON_Delete(i->tabs[n].model);
        i->tabs[n] = (CedExtensionTabState){0};
    }
}

static void report(CedExtensions *m, const char *text) {
    snprintf(m->error, sizeof m->error, "%s", text);
    if (*text && m->host.message) m->host.message(m->host.user, text);
}

cJSON *ced_extensions_context(const CedExtensionInstance *i) {
    cJSON *context = cJSON_CreateObject();
    if (!context || !cJSON_AddStringToObject(context, "sessionId", i->session) ||
        !cJSON_AddStringToObject(context, "workspaceId", i->manager->workspace_id) ||
        !cJSON_AddNumberToObject(context, "generation", (double)i->manager->generation)) {
        cJSON_Delete(context); return NULL;
    }
    return context;
}

static void request(CedExtensionInstance *i, const char *method, cJSON *params, double timeout) {
    CedExtensions *m = i->manager;
    snprintf(i->pending, sizeof i->pending, "ced:%llu", (unsigned long long)++m->next_request);
    cJSON *message = cJSON_CreateObject();
    bool ok = message && params && cJSON_AddStringToObject(message,"jsonrpc","2.0") &&
        cJSON_AddStringToObject(message,"id",i->pending) && cJSON_AddStringToObject(message,"method",method) &&
        ced_ext_json_add(params,"context",ced_extensions_context(i));
    if (ok) { ok = ced_ext_json_add(message,"params",params); params = NULL; }
    if (!ok) {
        cJSON_Delete(params); cJSON_Delete(message);
        snprintf(i->error,sizeof i->error,"ResourceLimit: request allocation");
        ced_extension_process_stop(i->process); return;
    }
    i->deadline = ced_ext_now() + timeout;
    i->request_started = ced_ext_now();
    snprintf(i->pending_method, sizeof i->pending_method, "%s", method);
    char log[160]; snprintf(log, sizeof log, "%s %s: sent\n", i->pending, method);
    ced_extension_process_log(i->process, log);
    ced_extension_process_send(i->process, message);
}

void ced_extensions_stop(CedExtensionInstance *i, const char *reason) {
    if (i->process && i->state != EXT_STOPPING) {
        if (*i->pending) {
            cJSON *cancel = cJSON_CreateObject(), *params = cJSON_CreateObject();
            cJSON_AddStringToObject(cancel, "jsonrpc", "2.0");
            cJSON_AddStringToObject(cancel, "method", "ced/cancel");
            cJSON_AddStringToObject(params, "requestId", i->pending);
            cJSON_AddItemToObject(params, "context", ced_extensions_context(i));
            cJSON_AddItemToObject(cancel, "params", params);
            ced_extension_process_send(i->process, cancel);
        }
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "reason", reason);
        request(i, "ced/deactivate", params, .5);
        i->state = EXT_STOPPING;
        ced_extension_process_stop(i->process);
    }
    *i->command = 0;
    i->invocation_document = 0;
    cJSON_Delete(i->invocation_editor); i->invocation_editor = NULL;
    clear_tab_state(i);
}

void ced_extensions_violation(CedExtensionInstance *i, const char *reason) {
    double now = ced_ext_now();
    if (now - i->violation_window >= 10) { i->violation_window = now; i->violations = 0; }
    unsigned count = ++i->violations;
    if (count == 8) {
        snprintf(i->error, sizeof i->error, "Repeated invalid extension notifications");
        ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED;
        report(i->manager, i->error);
    }
    if (count <= 3 || count == 8) {
        char log[240]; snprintf(log, sizeof log, "Protocol notification violation (%u/8): %.160s\n", count, reason);
        ced_extension_process_log(i->process, log);
    }
}

bool ced_extensions_grant_document(CedExtensionInstance *i, uint64_t id, bool allow) {
    size_t grant = 0;
    while (grant < i->grant_count && i->document_grants[grant] != id) grant++;
    if (!allow) {
        if (grant == i->grant_count) return true;
        memmove(i->document_grants + grant, i->document_grants + grant + 1, (i->grant_count - grant - 1) * sizeof *i->document_grants);
        i->grant_count--;
        ced_extensions_stop(i, "restart");
        return true;
    }
    if (!id || !i->manager->trusted || !i->record.valid || !(i->record.granted & CED_EXT_READ)) return false;
    if (grant < i->grant_count) return true;
    if (i->grant_count == 32) return false;
    i->document_grants[i->grant_count++] = id; return true;
}

unsigned ced_extensions_document_access(const CedExtensions *m, const CedDocument *document) {
    unsigned mask = 0;
    if (m) for (size_t n = 0; n < m->count; n++)
        if (ced_extensions_authorized(&m->instances[n], document)) mask |= 1u << n;
    return mask;
}

void ced_extensions_document_changed(CedExtensions *m, const CedDocument *document, unsigned previous_access) {
    if (!m) return;
    for (size_t n = 0; n < m->count; n++)
        if ((previous_access & (1u << n)) && !ced_extensions_authorized(&m->instances[n], document))
            ced_extensions_stop(&m->instances[n], "restart");
}

void ced_extensions_document_closed(CedExtensions *m, uint64_t id) {
    if (!m) return;
    for (size_t n = 0; n < m->count; n++) {
        CedExtensionInstance *i = &m->instances[n];
        if (i->invocation_document == id) ced_extensions_stop(i, "restart");
        ced_extensions_grant_document(i, id, false);
    }
}

static void invoke(CedExtensionInstance *i) {
    cJSON *params = cJSON_CreateObject();
    bool have_command = cJSON_AddStringToObject(params, "commandId", i->command) != NULL;
    bool have_arguments = ced_ext_json_add(params, "arguments",
        i->invocation_arguments ? i->invocation_arguments : cJSON_CreateNull());
    i->invocation_arguments = NULL;
    bool have_editor = ced_ext_json_add(params, "editor", i->invocation_editor);
    i->invocation_editor = NULL;
    if (!have_command || !have_arguments || !have_editor) {
        cJSON_Delete(params);
        snprintf(i->error, sizeof i->error, "ResourceLimit: command allocation");
        ced_extension_process_stop(i->process);
        *i->command = 0;
        i->invocation_document = 0;
        i->state = EXT_FAILED;
        report(i->manager, i->error);
        return;
    }
    request(i, "commands/invoke", params, 5);
}

static void initialize(CedExtensionInstance *i) {
    cJSON *p = cJSON_CreateObject(), *host = cJSON_AddObjectToObject(p, "host");
    cJSON_AddStringToObject(host, "name", "CED"); cJSON_AddStringToObject(host, "version", CED_EXTENSION_APP_VERSION);
    cJSON_AddNumberToObject(host, "tabProtocol", 1);
    cJSON_AddNumberToObject(host, "tabWidgets", 6);
    cJSON *extension = cJSON_AddObjectToObject(p, "extension");
    cJSON_AddStringToObject(extension, "id", i->record.manifest.id);
    cJSON_AddStringToObject(extension, "version", i->record.manifest.version);
    cJSON *versions = cJSON_AddArrayToObject(p, "apiVersions");
    cJSON_AddItemToArray(versions, cJSON_CreateString(CED_EXTENSION_API));
    cJSON *workspace = cJSON_AddObjectToObject(p, "workspace");
    char *uri = ced_lsp_uri(i->manager->workspace);
    cJSON_AddStringToObject(workspace, "uri", uri ? uri : ""); free(uri);
    cJSON_AddBoolToObject(workspace, "trusted", true);
    cJSON *grants = cJSON_AddArrayToObject(p, "grantedCapabilities");
    if (i->record.granted & CED_EXT_READ) cJSON_AddItemToArray(grants, cJSON_CreateString("documents.read"));
    if (i->record.granted & CED_EXT_EDIT) cJSON_AddItemToArray(grants, cJSON_CreateString("documents.edit"));
    cJSON *services = cJSON_AddArrayToObject(p, "hostMethods");
    for (size_t n = 0; methods[n]; n++) cJSON_AddItemToArray(services, cJSON_CreateString(methods[n]));
    cJSON *limits = cJSON_AddObjectToObject(p, "limits");
    static const struct { const char *key; int value; } values[] = {
        {"maxHeaderBytes",1024},{"maxMessageBytes",8388608},{"maxJsonDepth",64},
        {"maxQueuedBytes",8388608},{"maxPendingRequests",8},{"maxDocumentBytes",2097152},
        {"maxEditsPerTransaction",1000},{"maxSubscriptions",1},{"maxCommandArgumentsBytes",65536},
        {"maxCommandResultBytes",65536},{"maxStateBytes",262144},{"maxPresentationBytes",512},
        {"maxLogMessageBytes",8192},{"maxLogBytesPerSecond",65536},{"maxMessagesPer10Seconds",5},
        {"initializeTimeoutMs",3000},{"activateTimeoutMs",2000},{"commandTimeoutMs",5000},
        {"serviceTimeoutMs",1000},{"deactivateTimeoutMs",500}};
    for (size_t n = 0; n < sizeof values / sizeof *values; n++) cJSON_AddNumberToObject(limits, values[n].key, values[n].value);
    i->state = EXT_INITIALIZING;
    request(i, "ced/initialize", p, 3);
}

static bool execute_command(CedExtensionInstance *i, const char *id, const CedCommandContext *context,
                            cJSON *arguments, char *error) {
    CedExtensions *m = i->manager;
    const char *failure = NULL;
    if (!m->trusted) failure = "Trust this workspace in Manage Extensions before running extensions.";
    else if (m->job.running || m->reload || i->state == EXT_STOPPING) failure = "Extension manager is busy.";
    else if (i->state == EXT_FAILED) failure = "Restart the failed extension in Manage Extensions.";
    else if (*i->command) failure = "Busy: an extension command is already running.";
    size_t active = 0;
    for (size_t n = 0; n < m->count; n++) if (m->instances[n].process) active++;
    if (!failure && !i->process && active >= 4) failure = "ResourceLimit: four extensions are already running.";
    if (failure) { snprintf(error, CED_ERROR, "%s", failure); return false; }
    *i->error = 0;
    snprintf(i->command, sizeof i->command, "%s", id);
    i->invocation_editor = ced_extensions_editor(i, arguments ? (CedCommandContext){0} : *context);
    if (!i->invocation_editor) { *i->command = 0; snprintf(error, CED_ERROR, "ResourceLimit: editor context allocation"); return false; }
    i->invocation_document = !arguments && context->has_document ? context->document_id : 0;
    if (i->process) {
        i->invocation_arguments = arguments;
        i->tab_action = arguments != NULL;
        invoke(i);
        return true;
    }
    snprintf(i->session, sizeof i->session, "session-%ld-%llu", (long)getpid(), (unsigned long long)++m->next_session);
    i->last_request = 0;
    i->activation_started = ced_ext_now(); i->activation_ms = 0;
    i->violations = 0; i->violation_window = ced_ext_now();
    i->process = ced_extension_process_start(m->profile, &i->record, i->session, error);
    if (!i->process) { *i->command = 0; i->invocation_document = 0; cJSON_Delete(i->invocation_editor); i->invocation_editor = NULL; return false; }
    i->invocation_arguments = arguments;
    i->tab_action = arguments != NULL;
    initialize(i);
    return true;
}

static bool execute(void *user, const char *id, const CedCommandContext *context, char *error) {
    return execute_command(user, id, context, NULL, error);
}

bool ced_extensions_tab_action(CedExtensionInstance *i, size_t tab_index, const char *action, const char *value) {
    if (tab_index >= i->record.manifest.tab_count || !i->record.valid || !i->record.enabled) return false;
    CedExtensionTab *tab = &i->record.manifest.tabs[tab_index];
    CedExtensionCommand *command = &i->record.manifest.commands[tab->command];
    CedCommandContext context = i->manager->host.context(i->manager->host.user);
    if (!ced_command_condition(&command->when, &context)) {
        report(i->manager, "This tab is unavailable in the current context.");
        return false;
    }
    cJSON *args = cJSON_CreateArray();
    cJSON *event = cJSON_CreateObject();
    if (!args || !event || !cJSON_AddStringToObject(event, "tabId", tab->id) ||
        !cJSON_AddStringToObject(event, "action", action) || !cJSON_AddStringToObject(event, "value", value)) {
        cJSON_Delete(args); cJSON_Delete(event);
        report(i->manager, "Could not prepare the extension tab action.");
        return false;
    }
    char error[CED_ERROR] = {0};
    if (ced_ext_json_append(args, event) && execute_command(i, command->id, &context, args, error)) {
        i->tabs[tab_index].requested = true;
        return true;
    }
    cJSON_Delete(args);
    report(i->manager, *error ? error : "Could not send the extension tab action.");
    return false;
}

static bool tab_text(const char *text, size_t limit, bool multiline) {
    if (!text || strlen(text) > limit) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p < 32 && !(multiline && (*p == '\n' || *p == '\t'))) || *p == 127) return false;
    return true;
}

static bool valid_tab_choices(const cJSON *choices, int limit, size_t label_limit) {
    static const char *const fields[] = {"id", "label", NULL};
    char error[CED_ERROR];
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) > limit) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, choices) {
        const char *id = ced_ext_json_string(entry, "id");
        if (!ced_ext_json_fields(entry, fields, error) ||
            !tab_text(id, 128, false) || !*id ||
            !tab_text(ced_ext_json_string(entry, "label"), label_limit, false)) return false;
        for (const cJSON *previous = choices->child; previous != entry; previous = previous->next)
            if (!strcmp(id, ced_ext_json_string(previous, "id"))) return false;
    }
    return true;
}

static bool valid_tab_selectors(const cJSON *selectors) {
    static const char *const fields[] = {"id", "label", "selected", "options", "disabled", "placement", NULL};
    char error[CED_ERROR];
    if (!cJSON_IsArray(selectors) || cJSON_GetArraySize(selectors) > 2) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, selectors) {
        const char *id = ced_ext_json_string(entry, "id");
        const char *selected = ced_ext_json_string(entry, "selected");
        const cJSON *disabled = cJSON_GetObjectItemCaseSensitive(entry, "disabled");
        const cJSON *options = cJSON_GetObjectItemCaseSensitive(entry, "options");
        if (!ced_ext_json_fields(entry, fields, error) ||
            !tab_text(id, 128, false) || !*id || !strcmp(id, "refresh") ||
            !tab_text(ced_ext_json_string(entry, "label"), 80, false) ||
            !tab_text(selected, 128, false) || (disabled && !cJSON_IsBool(disabled)) ||
            !valid_tab_choices(options, CED_EXTENSION_TAB_CHOICES, 128)) return false;
        for (const cJSON *previous = selectors->child; previous != entry; previous = previous->next)
            if (!strcmp(id, ced_ext_json_string(previous, "id"))) return false;
        bool found = !*selected;
        const cJSON *choice;
        cJSON_ArrayForEach(choice, options)
            if (!strcmp(selected, ced_ext_json_string(choice, "id"))) found = true;
        if (!found) return false;
        if (cJSON_GetObjectItemCaseSensitive(entry, "placement")) {
            const char *placement = ced_ext_json_string(entry, "placement");
            if (!placement || strcmp(placement, "composer")) return false;
        }
    }
    return true;
}

static bool valid_tab_chat(const cJSON *chat) {
    static const char *const fields[] = {"author", "messages", "status", NULL};
    static const char *const message_fields[] = {"role", "text", NULL};
    static const char *const agent_fields[] = {"role", "text", "id", NULL};
    static const char *const reasoning_fields[] = {"role", "text", "id", "status", NULL};
    static const char *const tool_fields[] = {"role", "text", "id", "status", "input", "output", NULL};
    char error[CED_ERROR];
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(chat, "messages");
    const cJSON *status_item = cJSON_GetObjectItemCaseSensitive(chat, "status");
    const char *status = ced_ext_json_string(chat, "status");
    if (!ced_ext_json_fields(chat, fields, error) ||
        !tab_text(ced_ext_json_string(chat, "author"), 80, false) ||
        !cJSON_IsArray(messages) || cJSON_GetArraySize(messages) > 64 ||
        (status_item && (!status || (strcmp(status, "pending") && strcmp(status, "running") &&
                                     strcmp(status, "failed"))))) return false;
    size_t bytes = 0;
    const cJSON *message;
    cJSON_ArrayForEach(message, messages) {
        const char *role = ced_ext_json_string(message, "role");
        const char *text = ced_ext_json_string(message, "text");
        if (!role) return false;
        bool tool = !strcmp(role, "tool"), reasoning = !strcmp(role, "reasoning");
        bool agent = !strcmp(role, "agent");
        if (!tool && !reasoning && !agent && strcmp(role, "user")) return false;
        const char *const *allowed = tool ? tool_fields : reasoning ? reasoning_fields : agent ? agent_fields : message_fields;
        if (!ced_ext_json_fields(message, allowed, error) ||
            !tab_text(text, tool ? 256 : 9000 - bytes, !tool)) return false;
        if (tool || reasoning || (agent && cJSON_GetObjectItemCaseSensitive(message, "id"))) {
            const char *id = ced_ext_json_string(message, "id");
            const char *entry_status = ced_ext_json_string(message, "status");
            if (!tab_text(id, 128, false) || !*id || !*text) return false;
            if (reasoning && cJSON_GetObjectItemCaseSensitive(message, "status") &&
                (!entry_status || strcmp(entry_status, "running"))) return false;
            if (tool && (!entry_status || (strcmp(entry_status, "pending") && strcmp(entry_status, "running") &&
                strcmp(entry_status, "done") && strcmp(entry_status, "failed")))) return false;
            for (const cJSON *previous = messages->child; previous != message; previous = previous->next) {
                const char *other_id = ced_ext_json_string(previous, "id");
                if (other_id && !strcmp(other_id, id)) return false;
            }
        }
        if (tool) {
            const cJSON *input = cJSON_GetObjectItemCaseSensitive(message, "input");
            const cJSON *output = cJSON_GetObjectItemCaseSensitive(message, "output");
            if ((input && (!cJSON_IsString(input) || !tab_text(input->valuestring, 1024, true))) ||
                (output && (!cJSON_IsString(output) || !tab_text(output->valuestring, 1024, true)))) return false;
            bytes += (input ? strlen(input->valuestring) : 0) + (output ? strlen(output->valuestring) : 0);
        }
        bytes += strlen(text);
        if (bytes > 9000) return false;
    }
    return true;
}

static bool valid_tab_model(const cJSON *model) {
    static const char *const fields[] = {"heading", "text", "actions", "input", "refresh", "selectors", "chat", NULL};
    static const char *const input_fields[] = {"id", "label", "placeholder", "disabled", "stop", NULL};
    char error[CED_ERROR];
    if (!ced_ext_json_fields(model, fields, error)) return false;
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(model, "actions");
    const cJSON *input = cJSON_GetObjectItemCaseSensitive(model, "input");
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(model, "refresh");
    const cJSON *selectors = cJSON_GetObjectItemCaseSensitive(model, "selectors");
    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(model, "chat");
    if (!tab_text(ced_ext_json_string(model, "heading"), 256, false) ||
        !tab_text(ced_ext_json_string(model, "text"), 24000, true) ||
        !valid_tab_choices(actions, 32, 256) || (refresh && !cJSON_IsBool(refresh)) ||
        (selectors && !valid_tab_selectors(selectors)) || (chat && !valid_tab_chat(chat))) return false;
    const cJSON *action;
    cJSON_ArrayForEach(action, actions)
        if (!strcmp(ced_ext_json_string(action, "id"), "refresh")) return false;
    if (!input) return true;
    const char *id = ced_ext_json_string(input, "id");
    const cJSON *disabled = cJSON_GetObjectItemCaseSensitive(input, "disabled");
    const cJSON *stop_item = cJSON_GetObjectItemCaseSensitive(input, "stop");
    const char *stop = ced_ext_json_string(input, "stop");
    return ced_ext_json_fields(input, input_fields, error) &&
        tab_text(id, 128, false) && *id && strcmp(id, "refresh") &&
        tab_text(ced_ext_json_string(input, "label"), 80, false) &&
        tab_text(ced_ext_json_string(input, "placeholder"), 160, false) &&
        (!disabled || cJSON_IsBool(disabled)) &&
        (!stop_item || (chat && tab_text(stop, 128, false) && *stop && strcmp(stop, "refresh") && strcmp(stop, id)));
}

static void receive_tab(CedExtensionInstance *i, cJSON *result) {
    cJSON *model = cJSON_GetObjectItemCaseSensitive(result, "tab");
    for (size_t n = 0; n < i->record.manifest.tab_count; n++) {
        const CedExtensionTab *tab = &i->record.manifest.tabs[n];
        if (strcmp(i->command, i->record.manifest.commands[tab->command].id)) continue;
        CedExtensionTabState *state = &i->tabs[n];
        bool submitted = state->submitted;
        state->submitted = false;
        if (!valid_tab_model(model)) {
            snprintf(i->error, sizeof i->error, "Invalid extension tab model.");
            report(i->manager, i->error);
            return;
        }
        const cJSON *old_composer = cJSON_GetObjectItemCaseSensitive(state->model, "input");
        const cJSON *new_composer = cJSON_GetObjectItemCaseSensitive(model, "input");
        const char *old_input = ced_ext_json_string(old_composer, "id");
        const char *new_input = ced_ext_json_string(new_composer, "id");
        state->chat_animate = old_input && new_input && !strcmp(old_input, new_input) &&
            (submitted || ced_ext_json_string(old_composer, "stop") || ced_ext_json_string(new_composer, "stop"));
        if (submitted || (old_input && (!new_input || strcmp(old_input, new_input)))) *state->input = 0;
        if (!cJSON_GetObjectItemCaseSensitive(model, "chat") ||
            (old_input && new_input && strcmp(old_input, new_input))) state->chat_open = false;
        cJSON_Delete(state->model);
        state->model = cJSON_DetachItemViaPointer(result, model);
        state->requested = true;
        state->next_refresh = ced_ext_now() + .75;
        if (!i->tab_action)
            snprintf(i->manager->reveal_tab, sizeof i->manager->reveal_tab, "%s", tab->id);
        return;
    }
}

static bool handshake(CedExtensionInstance *i, cJSON *result) {
    const cJSON *identity = cJSON_GetObjectItemCaseSensitive(result, "extension");
    const char *id = ced_ext_json_string(identity, "id"), *version = ced_ext_json_string(identity, "version");
    const char *api = ced_ext_json_string(result, "apiVersion");
    cJSON *required = cJSON_GetObjectItemCaseSensitive(result, "requiredHostMethods");
    if (!id || strcmp(id, i->record.manifest.id) || !version || strcmp(version, i->record.manifest.version) ||
        !api || strcmp(api, CED_EXTENSION_API) || !cJSON_IsArray(required) || cJSON_GetArraySize(required) > 16) return false;
    cJSON *method;
    cJSON_ArrayForEach(method, required) {
        bool found = false;
        for (size_t n = 0; methods[n]; n++) if (cJSON_IsString(method) && !strcmp(method->valuestring, methods[n])) found = true;
        if (!found) return false;
    }
    return true;
}

static void response(CedExtensionInstance *i, const cJSON *message) {
    const char *id = ced_ext_json_string(message, "id");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(message, "result");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(message, "error");
    if (!ced_ext_rpc_id(id,"ced:",NULL) || cJSON_GetObjectItemCaseSensitive(message,"method") ||
        cJSON_GetObjectItemCaseSensitive(message,"params") || (!!result == !!error) || (error && !ced_ext_rpc_failure_valid(error))) {
        snprintf(i->error, sizeof i->error, "Invalid extension response envelope");
        ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED; return;
    }
    if (!*i->pending || strcmp(id, i->pending)) return;
    char log[240]; snprintf(log, sizeof log, "%s %s: %s after %.1f ms\n", id, i->pending_method,
        error ? "error" : "success", (ced_ext_now() - i->request_started) * 1000);
    ced_extension_process_log(i->process, log);
    *i->pending = 0;
    if (error) {
        for (size_t n = 0; n < i->record.manifest.tab_count; n++) i->tabs[n].submitted = false;
        const char *text = ced_ext_json_string(error, "message");
        snprintf(i->error, sizeof i->error, "%s", text ? text : "Invalid extension response");
        snprintf(log,sizeof log,"%s %s: error code %.0f\n",id,i->pending_method,cJSON_GetObjectItemCaseSensitive(error,"code")->valuedouble);
        ced_extension_process_log(i->process,log);
        if (i->state != EXT_ACTIVE) { ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED; }
        *i->command = 0; i->invocation_document = 0; report(i->manager, i->error); return;
    }
    if (i->state == EXT_INITIALIZING) {
        if (!handshake(i, result)) {
            snprintf(i->error, sizeof i->error, "Unsupported API, identity, or required host method");
            ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED; report(i->manager, i->error); return;
        }
        cJSON *params = cJSON_CreateObject(), *reason = cJSON_AddObjectToObject(params, "reason");
        char event[280]; snprintf(event, sizeof event, "onCommand:%s", i->command);
        cJSON_AddStringToObject(reason, "event", event);
        i->state = EXT_ACTIVATING; request(i, "ced/activate", params, 2);
    } else if (i->state == EXT_ACTIVATING) {
        if (!cJSON_IsObject(result)) { ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED; return; }
        i->activation_ms = (ced_ext_now() - i->activation_started) * 1000;
        i->state = EXT_ACTIVE; invoke(i);
    } else if (i->state == EXT_ACTIVE) {
        receive_tab(i, result);
        *i->command = 0; i->invocation_document = 0; i->tab_action = false;
    }
}

static void *catalog_worker(void *user) {
    CedExtensionJob *j = user;
    j->ok = true; *j->error = 0;
    switch (j->kind) {
        case EXT_INSPECT: j->ok = ced_extension_inspect(j->argument, &j->review, j->error); break;
        case EXT_INSTALL: j->ok = ced_extension_install(j->profile, &j->review, j->error); break;
        case EXT_ENABLE: case EXT_DISABLE: j->ok = ced_extension_set_enabled(j->profile, j->argument, j->kind == EXT_ENABLE, j->error); break;
        case EXT_TRUST: case EXT_REVOKE: j->ok = ced_extension_set_trusted(j->profile, j->workspace, j->kind == EXT_TRUST, j->error); break;
        case EXT_UNINSTALL: j->ok = ced_extension_uninstall(j->profile, j->argument, j->error); break;
        case EXT_LOAD: break;
    }
    if (j->ok && j->kind != EXT_INSPECT) j->ok = ced_extension_catalog_load(j->profile, j->workspace, &j->catalog, j->error);
    if (!j->ok && !*j->error) snprintf(j->error, sizeof j->error, "Could not complete the extension operation.");
    pthread_mutex_lock(&j->mutex); j->done = true; bool abandoned = j->abandoned; pthread_mutex_unlock(&j->mutex);
    if (abandoned) {
        CedExtensions *owner = j->owner;
        ced_extension_catalog_free(&j->catalog); ced_extension_record_free(&j->review);
        ced_extension_record_free(&owner->review); pthread_mutex_destroy(&j->mutex); free(owner);
    }
    return NULL;
}

bool ced_extensions_job(CedExtensions *m, CedExtensionJobKind kind, const char *argument) {
    CedExtensionJob *j = &m->job;
    if (j->running) { report(m, "Extension manager is busy."); return false; }
    /* Uninstall only after the instance releases its shared package lease. */
    if (kind == EXT_UNINSTALL) {
        for (size_t n = 0; n < m->count; n++) if (m->instances[n].process && !strcmp(argument, m->instances[n].record.manifest.id)) {
            ced_extensions_stop(&m->instances[n], "uninstall");
            report(m, "Extension is stopping. Choose Uninstall again when it has stopped."); return false;
        }
    }
    j->kind = kind; j->generation = m->generation; j->done = false;
    snprintf(j->profile, sizeof j->profile, "%s", m->profile);
    snprintf(j->workspace, sizeof j->workspace, "%s", m->workspace);
    snprintf(j->argument, sizeof j->argument, "%s", argument ? argument : "");
    if (kind == EXT_INSTALL) { j->review = m->review; memset(&m->review, 0, sizeof m->review); m->review_open = false; }
    if (kind != EXT_INSPECT) for (size_t n = 0; n < m->count; n++) ced_extensions_stop(&m->instances[n], "restart");
    if (pthread_create(&j->thread, NULL, catalog_worker, j)) { ced_extension_record_free(&j->review); report(m, "Could not start extension catalog worker."); return false; }
    j->running = true; return true;
}

static void publish(CedExtensions *m) {
    *m->reveal_tab = 0;
    for (size_t n = 0; n < m->count; n++) {
        clear_tab_state(&m->instances[n]);
        ced_commands_remove_owner(m->commands, &m->instances[n]);
        ced_extension_record_free(&m->instances[n].record);
        cJSON_Delete(m->instances[n].invocation_editor);
    }
    memset(m->instances, 0, sizeof m->instances);
    m->trusted = m->job.catalog.workspace_trusted;
    m->count = m->job.catalog.count;
    for (size_t n = 0; n < m->count; n++) {
        CedExtensionInstance *i = &m->instances[n];
        i->manager = m; i->record = m->job.catalog.entries[n]; i->state = EXT_REGISTERED;
        memset(&m->job.catalog.entries[n], 0, sizeof i->record);
        if (!i->record.valid || !i->record.enabled) continue;
        for (size_t c = 0; c < i->record.manifest.command_count; c++) {
            CedExtensionCommand *command = &i->record.manifest.commands[c]; char error[CED_ERROR] = {0};
            if (!ced_commands_add(m->commands, command->id, command->title, command->category, NULL,
                command->when, i, execute, i, error)) report(m, error);
        }
    }
    memset(&m->job.catalog, 0, sizeof m->job.catalog);
}

void ced_extensions_tick(CedExtensions *m) {
    if (!m) return;
    bool stopped = true;
    double dispatch_deadline = ced_ext_now() + .002;
    size_t first = m->count ? m->next_instance % m->count : 0;
    for (size_t offset = 0; offset < m->count; offset++) {
        size_t n = (first + offset) % m->count;
        CedExtensionInstance *i = &m->instances[n];
        if (!i->process) continue;
        /* Bound work per frame; excess remains in the charged worker queue. */
        for (unsigned k = 0; k < 4; k++) {
            if (ced_ext_now() >= dispatch_deadline) break;
            CedExtensionMessage *message = ced_extension_process_receive(i->process);
            if (!message) break;
            m->next_instance = (n + 1) % m->count;
            const char *rpc = ced_ext_json_string(message->json, "jsonrpc");
            if (!cJSON_IsObject(message->json)) {
                ced_extension_process_send(i->process,ced_ext_rpc_error(NULL,-32600,"InvalidRequest","RPC batches and primitive messages are not supported"));
                snprintf(i->error,sizeof i->error,"Invalid RPC message");
                ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED;
            } else if (cJSON_GetObjectItemCaseSensitive(message->json, "method")) ced_extensions_broker(i, message->json);
            else if (!rpc || strcmp(rpc,"2.0")) {
                snprintf(i->error,sizeof i->error,"Invalid RPC response version");
                ced_extensions_stop(i,"shutdown"); i->state = EXT_FAILED;
            } else if (!cJSON_GetObjectItemCaseSensitive(message->json, "method") && i->state == EXT_ACTIVE && message->body_length > 65536) {
                snprintf(i->error, sizeof i->error, "ResourceLimit: command response exceeds 64 KiB");
                ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED;
            } else response(i, message->json);
            ced_extension_message_free(message);
        }
        if (*i->pending && i->state != EXT_STOPPING && i->state != EXT_FAILED && ced_ext_now() > i->deadline) {
            snprintf(i->error, sizeof i->error, "Timeout: extension did not respond in time.");
            ced_extensions_stop(i, "shutdown"); i->state = EXT_FAILED; report(m, i->error);
        }
        char error[CED_ERROR] = {0};
        if (ced_extension_process_done(i->process, error)) {
            ced_extension_process_destroy(i->process); i->process = NULL;
            if (i->state != EXT_STOPPING) {
                i->state = EXT_FAILED;
                if (!*i->error) snprintf(i->error, sizeof i->error, "%s", *error ? error : "Extension exited unexpectedly");
                report(m, i->error);
            } else i->state = EXT_REGISTERED;
            *i->pending = 0; *i->command = 0; i->invocation_document = 0;
            cJSON_Delete(i->invocation_editor); i->invocation_editor = NULL;
            clear_tab_state(i);
        } else stopped = false;
    }
    CedExtensionJob *j = &m->job;
    if (j->running) {
        pthread_mutex_lock(&j->mutex); bool done = j->done; pthread_mutex_unlock(&j->mutex);
        if (done && (j->kind == EXT_INSPECT || stopped)) {
            pthread_join(j->thread, NULL); j->running = false;
            if (j->generation != m->generation) { ced_extension_catalog_free(&j->catalog); ced_extension_record_free(&j->review); m->reload = true; }
            else if (!j->ok) {
                if (j->kind != EXT_INSPECT) m->trusted = false;
                report(m, j->error); ced_extension_catalog_free(&j->catalog); ced_extension_record_free(&j->review);
            }
            else if (j->kind == EXT_INSPECT) {
                ced_extension_record_free(&m->review); m->review = j->review;
                memset(&j->review, 0, sizeof j->review); m->review_open = true;
            } else { publish(m); ced_extension_record_free(&j->review); *m->error = 0; }
        }
    }
    if (m->reload && !j->running && stopped) { m->reload = false; ced_extensions_job(m, EXT_LOAD, NULL); }
}

void ced_extensions_workspace(CedExtensions *m, const char *workspace) {
    if (!m) return;
    for (size_t n = 0; n < m->count; n++) { ced_extensions_stop(&m->instances[n], "workspaceChanged"); m->instances[n].grant_count = 0; }
    ++m->generation; m->trusted = false; m->reload = true;
    *m->reveal_tab = 0;
    snprintf(m->workspace, sizeof m->workspace, "%s", workspace);
    snprintf(m->workspace_id, sizeof m->workspace_id, "workspace-%ld-%llu", (long)getpid(), (unsigned long long)m->generation);
}

CedExtensions *ced_extensions_create(CedCommands *commands, CedExtensionHost host, const char *profile, const char *workspace) {
    CedExtensions *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->host = host; m->commands = commands;
    m->job.owner = m;
    pthread_mutex_init(&m->job.mutex, NULL);
    snprintf(m->profile, sizeof m->profile, "%s", profile);
    ced_extensions_workspace(m, workspace);
    return m;
}

const char *ced_extensions_status(const CedExtensionInstance *i) {
    if (!i->record.valid) return "Invalid";
    if (!i->record.enabled) return "Disabled";
    const char *names[] = {"Registered", "Initializing", "Activating", "Active", "Stopping", "Failed"};
    return names[i->state];
}

void ced_extensions_destroy(CedExtensions *m) {
    if (!m) return;
    for (size_t n = 0; n < m->count; n++) ced_extensions_stop(&m->instances[n], "shutdown");
    for (size_t n = 0; n < m->count; n++) {
        CedExtensionInstance *i = &m->instances[n];
        ced_extension_process_destroy(i->process);
        ced_commands_remove_owner(m->commands, i); ced_extension_record_free(&i->record);
    }
    if (m->job.running) {
        /* A selected directory can live on a slow filesystem. The job owns
         * its inputs/results and can finish without retaining UI resources. */
        pthread_mutex_lock(&m->job.mutex);
        bool done = m->job.done; pthread_t thread = m->job.thread;
        if (!done) m->job.abandoned = true;
        pthread_mutex_unlock(&m->job.mutex);
        if (!done) { pthread_detach(thread); return; }
        pthread_join(thread, NULL);
    }
    ced_extension_catalog_free(&m->job.catalog); ced_extension_record_free(&m->job.review);
    ced_extension_record_free(&m->review); pthread_mutex_destroy(&m->job.mutex); free(m);
}
