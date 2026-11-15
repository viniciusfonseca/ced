/* Native reference client. stdout is exclusively framed JSON-RPC. */
#include "extensions/json.h"
#include "rpc_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static CedRpcReader reader;
static unsigned long long counter;
static cJSON *context;
static bool stopping;
static char invocation_id[64];

static bool offers(const cJSON *params, const char *key, const char *value) {
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsArray(array)) return false;
    for (const cJSON *item = array->child; item; item = item->next)
        if (cJSON_IsString(item) && !strcmp(item->valuestring, value)) return true;
    return false;
}

static bool send_json(cJSON *message) {
    char *json = cJSON_PrintUnformatted(message); cJSON_Delete(message);
    if (!json) return false;
    size_t length; char *frame = ced_rpc_frame(json, strlen(json), &length); cJSON_free(json);
    if (!frame) return false;
    size_t used = 0;
    while (used < length) {
        ssize_t n = write(STDOUT_FILENO, frame + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    ced_rpc_frame_free(frame, length); return used == length;
}

static cJSON *receive(void) {
    char error[CED_ERROR];
    for (;;) {
        char *body; size_t length;
        int status = ced_rpc_next(&reader, &body, &length);
        if (status < 0) return NULL;
        if (status > 0) {
            cJSON *message = ced_ext_json_parse(body, length, 64, error);
            free(body); ced_rpc_release(length + 1); return message;
        }
        char bytes[4096]; ssize_t n = read(STDIN_FILENO, bytes, sizeof bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || !ced_rpc_feed(&reader, bytes, (size_t)n)) return NULL;
    }
}

static cJSON *service(const char *method, cJSON *params) {
    char id[64]; snprintf(id, sizeof id, "ext:%llu", ++counter);
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request,"jsonrpc","2.0"); cJSON_AddStringToObject(request,"id",id);
    cJSON_AddStringToObject(request,"method",method);
    cJSON_AddItemToObject(params,"context",cJSON_Duplicate(context,true)); cJSON_AddItemToObject(request,"params",params);
    if (!send_json(request)) return NULL;
    cJSON *message;
    while ((message = receive())) {
        const char *response_id = ced_ext_json_string(message,"id");
        if (response_id && !strcmp(response_id,id)) {
            const char *rpc = ced_ext_json_string(message,"jsonrpc");
            cJSON *result = cJSON_GetObjectItemCaseSensitive(message,"result"), *error = cJSON_GetObjectItemCaseSensitive(message,"error");
            if (!rpc || strcmp(rpc,"2.0") || (!!result == !!error) || (error && !ced_ext_rpc_failure_valid(error)) ||
                cJSON_GetObjectItemCaseSensitive(message,"method") || cJSON_GetObjectItemCaseSensitive(message,"params")) {
                cJSON_Delete(message); stopping = true; return NULL;
            }
            return message;
        }
        const char *incoming = ced_ext_json_string(message,"method");
        /* Cancellation/deactivation stop descendant work immediately. */
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(message, "params");
        const char *cancelled = ced_ext_json_string(p, "requestId");
        if (incoming && !strcmp(incoming,"ced/deactivate")) {
            if (response_id) send_json(ced_ext_rpc_result(response_id,cJSON_CreateObject()));
            stopping = true; cJSON_Delete(message); return NULL;
        }
        if (incoming && !strcmp(incoming,"ced/cancel") && cancelled && !strcmp(cancelled,invocation_id)) {
            stopping = true; cJSON_Delete(message); return NULL;
        }
        cJSON_Delete(message);
    }
    return NULL;
}

static cJSON *uppercase(const cJSON *params) {
    const cJSON *editor = cJSON_GetObjectItemCaseSensitive(params,"editor");
    const cJSON *document = cJSON_GetObjectItemCaseSensitive(editor,"document");
    const cJSON *selection = cJSON_GetObjectItemCaseSensitive(editor,"selection");
    const char *id = ced_ext_json_string(document,"documentId");
    uint64_t version, token, anchor, cursor;
    if (!id || !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(document,"version"),&version) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(editor,"selectionVersion"),&token) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(selection,"anchor"),&anchor) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(selection,"cursor"),&cursor)) return NULL;
    cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p,"documentId",id);
    cJSON *snapshot = service("documents/get",p);
    if (cJSON_GetObjectItemCaseSensitive(snapshot,"error")) return snapshot;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(snapshot,"result");
    const char *text = ced_ext_json_string(result,"text"); uint64_t actual;
    if (!text || !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(result,"document"),"version"),&actual)) { cJSON_Delete(snapshot); return NULL; }
    if (actual != version) {
        cJSON_Delete(snapshot);
        cJSON *reply=ced_ext_rpc_error(NULL,-32002,"StaleDocument","Document changed before the snapshot was received.");
        cJSON *data=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply,"error"),"data");
        if (!reply || !cJSON_AddNumberToObject(data,"currentVersion",(double)actual)) { cJSON_Delete(reply); return NULL; }
        return reply;
    }
    size_t start = (size_t)(anchor < cursor ? anchor : cursor), end = (size_t)(anchor > cursor ? anchor : cursor);
    if (!ced_utf8_boundary(text,strlen(text),start) || !ced_utf8_boundary(text,strlen(text),end)) { cJSON_Delete(snapshot); return NULL; }
    char *replacement = malloc(end-start+1);
    if (!replacement) { cJSON_Delete(snapshot); return NULL; }
    memcpy(replacement,text+start,end-start); replacement[end-start]=0;
    for (size_t n=0;n<end-start;n++) if (replacement[n]>='a' && replacement[n]<='z') replacement[n]-='a'-'A';
    p = cJSON_CreateObject(); cJSON_AddStringToObject(p,"documentId",id);
    cJSON_AddNumberToObject(p,"expectedVersion",(double)version); cJSON_AddNumberToObject(p,"expectedSelectionVersion",(double)token);
    cJSON_AddStringToObject(p,"label","Uppercase ASCII Selection");
    cJSON *edits = cJSON_AddArrayToObject(p,"edits"), *edit = cJSON_CreateObject();
    cJSON_AddNumberToObject(edit,"start",(double)start); cJSON_AddNumberToObject(edit,"end",(double)end);
    cJSON_AddStringToObject(edit,"text",replacement); cJSON_AddItemToArray(edits,edit);
    cJSON_AddItemToObject(p,"selection",cJSON_Duplicate(selection,true));
    free(replacement); cJSON_Delete(snapshot);
    return service("documents/applyEdits",p);
}

int main(void) {
    bool initialized=false, active=false; cJSON *message;
    while ((message=receive())) {
        const char *method=ced_ext_json_string(message,"method"), *id=ced_ext_json_string(message,"id");
        const cJSON *p=cJSON_GetObjectItemCaseSensitive(message,"params");
        if (!method) { cJSON_Delete(message); continue; }
        if (!strcmp(method,"ced/initialize") && !initialized) {
            const cJSON *identity=cJSON_GetObjectItemCaseSensitive(p,"extension");
            const char *name=ced_ext_json_string(identity,"id"), *version=ced_ext_json_string(identity,"version");
            if (!name || strcmp(name,"example.text-tools") || !version || strcmp(version,"0.1.0")) { cJSON_Delete(message); break; }
            if (!offers(p,"apiVersions","1.0") || !offers(p,"hostMethods","documents/get") ||
                !offers(p,"hostMethods","documents/applyEdits") || !offers(p,"grantedCapabilities","documents.read") ||
                !offers(p,"grantedCapabilities","documents.edit")) {
                send_json(ced_ext_rpc_error(id,-32008,"UnsupportedCapability","Required API, service or document grant is missing."));
                cJSON_Delete(message); break;
            }
            context=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(p,"context"),true);
            cJSON *result=cJSON_CreateObject(), *extension=cJSON_AddObjectToObject(result,"extension");
            cJSON_AddStringToObject(extension,"id","example.text-tools"); cJSON_AddStringToObject(extension,"version","0.1.0");
            cJSON_AddStringToObject(result,"apiVersion","1.0");
            cJSON *required=cJSON_AddArrayToObject(result,"requiredHostMethods");
            cJSON_AddItemToArray(required,cJSON_CreateString("documents/get")); cJSON_AddItemToArray(required,cJSON_CreateString("documents/applyEdits"));
            send_json(ced_ext_rpc_result(id,result)); initialized=true;
        } else if (!cJSON_Compare(cJSON_GetObjectItemCaseSensitive(p,"context"),context,true)) {
            if (id) send_json(ced_ext_rpc_error(id,-32003,"InvalidContext","Invalid context"));
            cJSON_Delete(message); break;
        } else if (!strcmp(method,"ced/activate") && initialized && !active) { active=true; send_json(ced_ext_rpc_result(id,cJSON_CreateObject())); }
        else if (!strcmp(method,"ced/deactivate")) { send_json(ced_ext_rpc_result(id,cJSON_CreateObject())); cJSON_Delete(message); break; }
        else if (!strcmp(method,"commands/invoke") && active) {
            const char *command=ced_ext_json_string(p,"commandId");
            if (!id || !command || strcmp(command,"example.text-tools.uppercase")) {
                if (id) send_json(ced_ext_rpc_error(id,-32601,"MethodNotFound","Unknown command"));
                cJSON_Delete(message); continue;
            }
            if (!cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(p,"arguments"))) {
                send_json(ced_ext_rpc_error(id,-32602,"InvalidParams","Expected null command arguments"));
                cJSON_Delete(message); continue;
            }
            snprintf(invocation_id,sizeof invocation_id,"%s",id ? id : "");
            cJSON *answer=uppercase(p);
            cJSON *result=cJSON_DetachItemFromObjectCaseSensitive(answer,"result");
            cJSON *error=cJSON_GetObjectItemCaseSensitive(answer,"error");
            if (result) send_json(ced_ext_rpc_result(id,result));
            else if (!stopping) {
                if (ced_ext_rpc_failure_valid(error)) {
                    cJSON_DeleteItemFromObjectCaseSensitive(answer,"id");
                    if (cJSON_AddStringToObject(answer,"id",id)) { send_json(answer); answer=NULL; }
                    else stopping=true;
                } else send_json(ced_ext_rpc_error(id,-32011,"Unavailable","Selection or host response unavailable; no retry was attempted."));
            }
            cJSON_Delete(answer);
        } else if (id) send_json(ced_ext_rpc_error(id,-32601,"MethodNotFound","Unknown method or lifecycle state"));
        cJSON_Delete(message);
        if (stopping) break;
    }
    cJSON_Delete(context); ced_rpc_reader_free(&reader); return 0;
}
