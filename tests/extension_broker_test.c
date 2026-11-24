#include "extension_temp.h"
#include "extensions/manager.h"
#include "extensions/process_internal.h"
#include "file_operations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* No worker runs here. cJSON allocation hooks are changed only in this
 * single-threaded test process; production never changes them per request. */
typedef struct { CedDocument document; unsigned commits; bool slow_message, reject_commit; } Host;
static Host host;
static CedExtensions *manager;
static CedExtensionInstance *instance;
static CedExtensionProcess *process;
static size_t allocations, fail_at;
static void inject(CedExtensionProcess *target, cJSON *json);
static void *fault_malloc(size_t size) { return ++allocations == fail_at ? NULL : malloc(size); }
static CedCommandContext context(void *user) {
    Host *h = user;
    return (CedCommandContext){h->document.id,h->document.version,4,0,1,true,true,true,"text"};
}
static bool document(void *user, size_t index, CedExtensionDocument *out) {
    if (index) return false;
    *out = (CedExtensionDocument){&((Host *)user)->document,"text",0,1}; return true;
}
static bool commit(void *user, CedDocument *doc, uint64_t version, const CedEditTransaction *edit, char *error) {
    Host *h = user; assert(doc->version == version);
    if (h->reject_commit) { snprintf(error,CED_ERROR,"Out of memory"); return false; }
    if (edit->changed) { memcpy(doc->text,edit->text,edit->length+1); ced_document_note_change(doc); h->commits++; }
    return true;
}
static void message(void *user, const char *text) {
    (void)text; Host *h = user;
    if (h->slow_message) { struct timespec delay = {.tv_nsec=5000000}; nanosleep(&delay,NULL); }
}
static CedExtensionMessage *take(MessageQueue *queue) {
    CedExtensionMessage *item = queue->first;
    if (item) {
        queue->first = item->next; if (!queue->first) queue->last = NULL;
        queue->count--; queue->bytes -= item->charge; item->next = NULL;
    }
    return item;
}
static cJSON *answer(void) {
    CedExtensionMessage *item = take(&process->outgoing); assert(item);
    cJSON *json = cJSON_Duplicate(item->json,true); assert(json); ced_extension_message_free(item); return json;
}
static void reset(void) {
    CedExtensionMessage *item;
    while ((item = take(&process->outgoing))) ced_extension_message_free(item);
    while ((item = take(&process->incoming))) ced_extension_message_free(item);
    assert(!process->reserved_count && !process->reserved_bytes);
    process->stop = process->done = false; *process->error = *process->pending_log = 0;
    *instance->pending = *instance->command = *instance->error = 0;
    instance->state = EXT_ACTIVE; instance->last_request = 0; instance->violations = 0;
    instance->grant_count = 0; instance->invocation_document = 0;
    instance->record.granted = CED_EXT_READ | CED_EXT_EDIT;
    strcpy(host.document.text,"abc"); host.document.version = 7; host.document.length = 3; host.commits = 0;
    host.reject_commit = false;
    assert(ced_rpc_memory_used() == 0);
}
static cJSON *request(const char *method, bool notification) {
    cJSON *json = cJSON_CreateObject(); cJSON_AddStringToObject(json,"jsonrpc","2.0");
    if (!notification) cJSON_AddStringToObject(json,"id","ext:1");
    cJSON_AddStringToObject(json,"method",method);
    cJSON *params = cJSON_AddObjectToObject(json,"params");
    assert(ced_ext_json_add(params,"context",ced_extensions_context(instance))); return json;
}
static cJSON *edit_request(unsigned version) {
    cJSON *json = request("documents/applyEdits",false), *params = cJSON_GetObjectItemCaseSensitive(json,"params");
    cJSON_AddStringToObject(params,"documentId","doc-1"); cJSON_AddNumberToObject(params,"expectedVersion",version);
    cJSON_AddStringToObject(params,"label","Test edit");
    cJSON *edits = cJSON_AddArrayToObject(params,"edits"), *edit = cJSON_CreateObject();
    cJSON_AddNumberToObject(edit,"start",0); cJSON_AddNumberToObject(edit,"end",1); cJSON_AddStringToObject(edit,"text","X");
    cJSON_AddItemToArray(edits,edit); return json;
}
static void dispatch(cJSON *json) { ced_extensions_broker(instance,json); cJSON_Delete(json); }
static cJSON *expect_error(int code) {
    cJSON *json = answer(), *error = cJSON_GetObjectItemCaseSensitive(json,"error");
    assert(cJSON_IsObject(error) && cJSON_GetObjectItemCaseSensitive(error,"code")->valueint == code);
    assert(ced_ext_rpc_failure_valid(error)); return json;
}
static void allocation_tests(void) {
    reset(); cJSON *json = edit_request(7);
    allocations = 0; fail_at = SIZE_MAX; cJSON_Hooks hooks = {fault_malloc,free}; cJSON_InitHooks(&hooks);
    ced_extensions_broker(instance,json); size_t count = allocations;
    cJSON_InitHooks(NULL); cJSON_Delete(json); assert(host.commits == 1 && count > 10);
    cJSON *reply = answer(); cJSON_Delete(reply);
    for (size_t failure = 1; failure <= count; failure++) {
        reset(); json = edit_request(7); allocations = 0; fail_at = failure;
        cJSON_InitHooks(&hooks); ced_extensions_broker(instance,json); cJSON_InitHooks(NULL); cJSON_Delete(json);
        assert(host.commits == 0 && host.document.version == 7 && !strcmp(host.document.text,"abc"));
        reply = expect_error(-32005); cJSON_Delete(reply);
        assert(!process->reserved_count && !process->reserved_bytes);
    }
    reset(); host.reject_commit = true; dispatch(edit_request(7));
    reply = expect_error(-32005); cJSON_Delete(reply);
    assert(!host.commits && !strcmp(host.document.text,"abc") && !process->reserved_count && !process->reserved_bytes);
    /* A saturated real queue also refuses the edit before the commit. */
    reset();
    for (int n = 0; n < 8; n++) assert(ced_extension_process_send(process,cJSON_CreateObject()));
    dispatch(edit_request(7)); assert(!host.commits && host.document.version == 7 && process->stop);
    /* Reserving the last slot succeeds and the reply needs no post-commit allocation. */
    reset();
    for (int n = 0; n < 7; n++) assert(ced_extension_process_send(process,cJSON_CreateObject()));
    dispatch(edit_request(7)); assert(host.commits == 1 && process->outgoing.count == 8 && !process->stop);
    for (int n = 0; n < 7; n++) { reply = answer(); cJSON_Delete(reply); }
    reply = answer(); assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply,"result"),"version")->valueint == 8); cJSON_Delete(reply);
}
static void protocol_tests(void) {
    reset(); dispatch(edit_request(6)); cJSON *reply = expect_error(-32002);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply,"error"),"data");
    assert(cJSON_GetObjectItemCaseSensitive(data,"currentVersion")->valueint == 7); cJSON_Delete(reply);
    reset(); dispatch(request("documents/get",false)); reply = expect_error(-32602); cJSON_Delete(reply);
    reset(); cJSON *json = request("documents/get",false), *params = cJSON_GetObjectItemCaseSensitive(json,"params");
    cJSON_AddNumberToObject(params,"documentId",7); dispatch(json); reply = expect_error(-32602); cJSON_Delete(reply);
    reset(); json = request("documents/get",false); params = cJSON_GetObjectItemCaseSensitive(json,"params");
    cJSON_AddStringToObject(params,"documentId","closed-id"); dispatch(json); reply = expect_error(-32011); cJSON_Delete(reply);
    reset(); json = edit_request(7); params = cJSON_GetObjectItemCaseSensitive(json,"params");
    cJSON *ctx = cJSON_GetObjectItemCaseSensitive(params,"context");
    cJSON_ReplaceItemInObjectCaseSensitive(ctx,"generation",cJSON_CreateNumber(999)); dispatch(json);
    reply = expect_error(-32003); cJSON_Delete(reply); assert(!host.commits);
    reset(); instance->record.granted = CED_EXT_READ; dispatch(edit_request(7));
    reply = expect_error(-32001); cJSON_Delete(reply); assert(!host.commits);
    reset(); json = request("documents/list",false); cJSON_AddStringToObject(json,"futureOptional","value"); dispatch(json);
    reply = answer(); assert(cJSON_GetObjectItemCaseSensitive(reply,"result") && instance->state == EXT_ACTIVE); cJSON_Delete(reply);
    reset(); json = edit_request(7); params = cJSON_GetObjectItemCaseSensitive(json,"params");
    cJSON_AddBoolToObject(params,"futureOption",true); dispatch(json); reply = answer(); cJSON_Delete(reply); assert(host.commits == 1);
    reset(); json = request("unknown/method",false); dispatch(json); reply = expect_error(-32601); cJSON_Delete(reply);
    reset(); json = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":\"ced:9\",\"result\":{},\"futureOptional\":true}");
    assert(ced_extension_process_send(process,json));
    CedExtensionMessage *incoming = take(&process->outgoing); process->incoming = (MessageQueue){incoming,incoming,1,incoming->charge};
    strcpy(instance->pending,"ced:9"); strcpy(instance->command,"test.run"); instance->deadline = ced_ext_now()+5;
    ced_extensions_tick(manager); assert(instance->state == EXT_ACTIVE && !*instance->pending && !*instance->command);
    reset();
    for (int n = 0; n < 8; n++) dispatch(request("unknown/notification",true));
    assert(instance->state == EXT_FAILED && process->stop && strstr(process->pending_log,"notification violation"));
    /* Only lifecycle requests may be queued, never replies to notifications. */
    while (process->outgoing.count) { reply = answer(); assert(ced_ext_json_string(reply,"method")); cJSON_Delete(reply); }
    reset(); json = request("log/write",true); dispatch(json);
    assert(instance->violations == 1 && !process->outgoing.count && *process->pending_log);
    reset();
    ced_extension_process_log(process,"commit record"); ced_extension_process_log(process,"response record");
    assert(strstr(process->pending_log,"commit record") && strstr(process->pending_log,"response record"));
    char flood[9000]; memset(flood,'x',sizeof flood-1); flood[sizeof flood-1]=0;
    ced_extension_process_log(process,flood); ced_extension_process_log(process,"last diagnostic");
    assert(strlen(process->pending_log)<=8192 && strstr(process->pending_log,"last diagnostic"));
    /* Forward compatibility never permits forbidden known fields or malformed replies. */
    const char *invalid[]={
        "{\"jsonrpc\":\"2.0\",\"id\":\"ced:9\",\"result\":{},\"error\":{\"code\":-1,\"message\":\"bad\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"ced:9\",\"error\":{\"code\":\"bad\",\"message\":\"bad\"}}",
        "{\"jsonrpc\":\"1.0\",\"id\":\"ced:9\",\"result\":{}}", "[]", NULL};
    for (size_t n=0;invalid[n];n++) {
        reset(); inject(process,cJSON_Parse(invalid[n]));
        strcpy(instance->pending,"ced:9"); strcpy(instance->command,"test.run"); instance->deadline=ced_ext_now()+5;
        ced_extensions_tick(manager); assert(instance->state==EXT_FAILED && !host.commits);
    }
}
static void revocation_tests(const char *inside) {
    reset(); assert(ced_extensions_grant_document(instance,host.document.id,true));
    strcpy(instance->pending,"ced:11"); strcpy(instance->command,"test.run"); instance->invocation_document = host.document.id;
    assert(ced_extensions_grant_document(instance,host.document.id,false));
    assert(instance->state == EXT_STOPPING && process->stop && !*instance->command && !instance->grant_count);
    reset(); unsigned access = ced_extensions_document_access(manager,&host.document); assert(access == 1);
    *host.document.path = 0; ced_extensions_document_changed(manager,&host.document,access);
    assert(instance->state == EXT_STOPPING && process->stop && !host.commits);
    snprintf(host.document.path,sizeof host.document.path,"%s",inside);
    reset(); instance->invocation_document = host.document.id;
    ced_extensions_document_closed(manager,host.document.id);
    assert(instance->state == EXT_STOPPING && process->stop);
}
static void inject(CedExtensionProcess *target, cJSON *json) {
    assert(ced_extension_process_send(target,json));
    CedExtensionMessage *item = take(&target->outgoing);
    assert(!target->incoming.first);
    target->incoming = (MessageQueue){item,item,1,item->charge};
}
static cJSON *show_message(void) {
    cJSON *json = request("window/showMessage",false), *params = cJSON_GetObjectItemCaseSensitive(json,"params");
    cJSON_AddStringToObject(params,"level","info"); cJSON_AddStringToObject(params,"message","Fair dispatch test"); return json;
}
static void frame_budget_test(void) {
    reset();
    CedExtensionProcess *other = calloc(1,sizeof *other); assert(other); pthread_mutex_init(&other->mutex,NULL);
    manager->count = 2; manager->next_instance = 0;
    CedExtensionInstance *second = &manager->instances[1];
    second->manager = manager; second->process = other; second->state = EXT_ACTIVE;
    strcpy(second->session,instance->session);
    inject(process,show_message()); inject(other,show_message());
    host.slow_message = true; ced_extensions_tick(manager); host.slow_message = false;
    assert(instance->last_request == 1 && second->last_request == 0 && manager->next_instance == 1);
    /* The next frame starts with the deferred instance. */
    ced_extensions_tick(manager); assert(second->last_request == 1 && !other->incoming.count);
    CedExtensionMessage *item;
    while ((item = take(&other->outgoing))) ced_extension_message_free(item);
    pthread_mutex_destroy(&other->mutex); free(other); memset(second,0,sizeof *second); manager->count = 1;
    reset();
}
static void contention_test(void) {
    reset();
    CedExtensionProcess *other=calloc(1,sizeof *other); assert(other); pthread_mutex_init(&other->mutex,NULL);
    CedExtensionInstance *second=&manager->instances[1]; manager->count=2;
    second->manager=manager; second->process=other; second->state=EXT_ACTIVE;
    second->record.granted=CED_EXT_READ|CED_EXT_EDIT; strcpy(second->session,"second-session");
    dispatch(edit_request(7)); assert(host.commits==1 && host.document.version==8);
    CedExtensionInstance *first=instance; instance=second; dispatch(edit_request(7)); instance=first;
    CedExtensionMessage *reply=take(&other->outgoing); assert(reply);
    cJSON *error=cJSON_GetObjectItemCaseSensitive(reply->json,"error"),*data=cJSON_GetObjectItemCaseSensitive(error,"data");
    assert(cJSON_GetObjectItemCaseSensitive(error,"code")->valueint==-32002);
    assert(cJSON_GetObjectItemCaseSensitive(data,"currentVersion")->valueint==8);
    assert(host.commits==1 && host.document.version==8 && !strcmp(host.document.text,"Xbc"));
    ced_extension_message_free(reply); pthread_mutex_destroy(&other->mutex); free(other);
    memset(second,0,sizeof *second); manager->count=1; reset();
}
int main(void) {
    char root[CED_PATH], inside[CED_PATH], parent[CED_PATH], error[CED_ERROR];
    extension_temp("ced-extension-broker-XXXXXX", root, parent);
    assert(ced_join(inside,sizeof inside,root,"notes.txt")); FILE *file = fopen(inside,"w"); assert(file); fputs("abc",file); fclose(file);
    host.document.text = calloc(CED_TEXT_CAP,1); host.document.saved = strdup("abc"); host.document.id = 1;
    strcpy(host.document.path,inside); assert(host.document.text && host.document.saved);
    manager = calloc(1,sizeof *manager); process = calloc(1,sizeof *process); assert(manager && process);
    manager->count = 1; manager->trusted = true; manager->generation = 1;
    strcpy(manager->workspace,root); strcpy(manager->workspace_id,"test-workspace");
    manager->host = (CedExtensionHost){&host,context,document,commit,message,NULL};
    pthread_mutex_init(&process->mutex,NULL);
    instance = &manager->instances[0]; instance->manager = manager; instance->process = process;
    strcpy(instance->session,"test-session"); instance->record.valid = instance->record.enabled = true;
    allocation_tests(); protocol_tests(); revocation_tests(inside); frame_budget_test(); contention_test(); reset();
    pthread_mutex_destroy(&process->mutex); free(process); free(manager); ced_document_free(&host.document);
    assert(ced_file_delete(parent,root,error));
    puts("OK: broker allocation failures, queue reservations, stale/error metadata, optional fields, notification abuse and document revocation");
    return 0;
}
