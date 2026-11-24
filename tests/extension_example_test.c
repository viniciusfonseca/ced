#include "extension_temp.h"
#include "extensions/manager.h"
#include "file_operations.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static CedExtensionProcess *process;
static CedExtensionInstance *instance;
static void send_request(const char *id, const char *method, cJSON *params) {
    cJSON *request=cJSON_CreateObject();
    cJSON_AddStringToObject(request,"jsonrpc","2.0"); cJSON_AddStringToObject(request,"id",id);
    cJSON_AddStringToObject(request,"method",method);
    assert(ced_ext_json_add(params,"context",ced_extensions_context(instance)));
    assert(ced_ext_json_add(request,"params",params)); assert(ced_extension_process_send(process,request));
}
static CedExtensionMessage *receive(void) {
    double deadline=ced_ext_now()+5; CedExtensionMessage *message;
    while (!(message=ced_extension_process_receive(process))) {
        assert(ced_ext_now()<deadline);
        struct timespec delay={.tv_nsec=5000000}; nanosleep(&delay,NULL);
    }
    return message;
}
static void expect_success(const char *id) {
    CedExtensionMessage *message=receive();
    assert(!strcmp(ced_ext_json_string(message->json,"id"),id));
    assert(cJSON_GetObjectItemCaseSensitive(message->json,"result")); ced_extension_message_free(message);
}
static void invoke(const char *id) {
    cJSON *params=cJSON_CreateObject(); cJSON_AddStringToObject(params,"commandId","example.text-tools.uppercase");
    cJSON_AddNullToObject(params,"arguments");
    cJSON *editor=cJSON_AddObjectToObject(params,"editor"),*document=cJSON_AddObjectToObject(editor,"document");
    cJSON_AddStringToObject(document,"documentId","test-doc"); cJSON_AddNumberToObject(document,"version",7);
    cJSON *selection=cJSON_AddObjectToObject(editor,"selection");
    cJSON_AddNumberToObject(selection,"anchor",0); cJSON_AddNumberToObject(selection,"cursor",1);
    cJSON_AddNumberToObject(editor,"selectionVersion",3); send_request(id,"commands/invoke",params);
}
static void expect_error(const char *id,int code,const char *reason,bool revision) {
    CedExtensionMessage *message=receive(); assert(!strcmp(ced_ext_json_string(message->json,"id"),id));
    cJSON *error=cJSON_GetObjectItemCaseSensitive(message->json,"error"),*data=cJSON_GetObjectItemCaseSensitive(error,"data");
    assert(ced_ext_rpc_failure_valid(error) && cJSON_GetObjectItemCaseSensitive(error,"code")->valueint==code);
    assert(!strcmp(ced_ext_json_string(data,"reason"),reason));
    if(revision) assert(cJSON_GetObjectItemCaseSensitive(data,"currentVersion")->valueint==8);
    ced_extension_message_free(message);
}
static void snapshot(unsigned version) {
    CedExtensionMessage *message=receive(); assert(!strcmp(ced_ext_json_string(message->json,"method"),"documents/get"));
    cJSON *result=cJSON_CreateObject(),*document=cJSON_AddObjectToObject(result,"document");
    cJSON_AddNumberToObject(document,"version",version); cJSON_AddStringToObject(result,"text","abc");
    assert(ced_extension_process_send(process,ced_ext_rpc_result(ced_ext_json_string(message->json,"id"),result)));
    ced_extension_message_free(message);
}
int main(int argc,char **argv) {
    assert(argc==2); char root[CED_PATH],parent[CED_PATH],profile[CED_PATH],error[CED_ERROR]={0};
    extension_temp("ced-extension-example-XXXXXX", root, parent);
    assert(ced_join(profile,sizeof profile,root,"profile"));
    CedExtensionRecord approved={0}; assert(ced_extension_inspect(argv[1],&approved,error));
    assert(ced_extension_install(profile,&approved,error)); ced_extension_record_free(&approved);
    CedExtensionCatalog catalog={0}; assert(ced_extension_catalog_load(profile,root,&catalog,error));
    CedExtensions *manager=calloc(1,sizeof *manager); assert(manager); instance=&manager->instances[0]; instance->manager=manager;
    manager->generation=1; strcpy(manager->workspace_id,"test-workspace"); strcpy(instance->session,"test-example");
    process=ced_extension_process_start(profile,&catalog.entries[0],instance->session,error); assert(process);
    cJSON *params=cJSON_CreateObject(),*identity=cJSON_AddObjectToObject(params,"extension");
    cJSON_AddStringToObject(identity,"id","example.text-tools"); cJSON_AddStringToObject(identity,"version","0.1.0");
    cJSON *versions=cJSON_AddArrayToObject(params,"apiVersions"); cJSON_AddItemToArray(versions,cJSON_CreateString("1.0"));
    cJSON *methods=cJSON_AddArrayToObject(params,"hostMethods");
    cJSON_AddItemToArray(methods,cJSON_CreateString("documents/get")); cJSON_AddItemToArray(methods,cJSON_CreateString("documents/applyEdits"));
    cJSON *grants=cJSON_AddArrayToObject(params,"grantedCapabilities");
    cJSON_AddItemToArray(grants,cJSON_CreateString("documents.read")); cJSON_AddItemToArray(grants,cJSON_CreateString("documents.edit"));
    send_request("ced:1","ced/initialize",params); expect_success("ced:1");
    send_request("ced:2","ced/activate",cJSON_CreateObject()); expect_success("ced:2");
    invoke("ced:3"); CedExtensionMessage *message=receive();
    assert(!strcmp(ced_ext_json_string(message->json,"method"),"documents/get"));
    cJSON *reply=ced_ext_rpc_error(ced_ext_json_string(message->json,"id"),-32002,"StaleDocument","The document changed.");
    cJSON *data=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply,"error"),"data"); cJSON_AddNumberToObject(data,"currentVersion",8);
    assert(ced_extension_process_send(process,reply)); ced_extension_message_free(message);
    expect_error("ced:3",-32002,"StaleDocument",true);
    invoke("ced:4"); message=receive();
    assert(ced_extension_process_send(process,ced_ext_rpc_error(ced_ext_json_string(message->json,"id"),-32001,"PermissionDenied","Access revoked.")));
    ced_extension_message_free(message); expect_error("ced:4",-32001,"PermissionDenied",false);
    invoke("ced:5"); snapshot(8); expect_error("ced:5",-32002,"StaleDocument",true);
    invoke("ced:6"); snapshot(7); message=receive();
    assert(!strcmp(ced_ext_json_string(message->json,"method"),"documents/applyEdits"));
    assert(ced_extension_process_send(process,ced_ext_rpc_error(ced_ext_json_string(message->json,"id"),-32012,"StaleEditorContext","Selection moved.")));
    ced_extension_message_free(message); expect_error("ced:6",-32012,"StaleEditorContext",false);
    params=cJSON_CreateObject(); cJSON_AddStringToObject(params,"commandId","unknown.command"); cJSON_AddNullToObject(params,"arguments");
    send_request("ced:7","commands/invoke",params); expect_error("ced:7",-32601,"MethodNotFound",false);
    params=cJSON_CreateObject(); cJSON_AddStringToObject(params,"reason","shutdown");
    send_request("ced:8","ced/deactivate",params); ced_extension_process_stop(process); expect_success("ced:8");
    double deadline=ced_ext_now()+5;
    while(!ced_extension_process_done(process,NULL)) { assert(ced_ext_now()<deadline); struct timespec delay={.tv_nsec=5000000}; nanosleep(&delay,NULL); }
    ced_extension_process_destroy(process); ced_extension_catalog_free(&catalog); free(manager);
    assert(ced_rpc_memory_used()==0 && ced_file_delete(parent,root,error));
    puts("OK: real example preserves host error code/reason/revision, checks captured snapshots and never retries failed edits"); return 0;
}
