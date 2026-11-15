/* Deliberately hostile native peers used by lifecycle tests. */
#include "extensions/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static cJSON *receive(void) {
    char header[1024]; size_t length=0;
    if (!fgets(header,sizeof header,stdin) || sscanf(header,"Content-Length: %zu",&length)!=1 || !length || length>65536) return NULL;
    if (!fgets(header,sizeof header,stdin) || strcmp(header,"\r\n")) return NULL;
    char *body=malloc(length+1); if (!body) return NULL;
    if (fread(body,1,length,stdin)!=length) { free(body); return NULL; }
    body[length]=0; cJSON *message=cJSON_Parse(body); free(body); return message;
}
static void send_json(cJSON *message) {
    cJSON_AddBoolToObject(message,"futureOptional",true);
    char *body=cJSON_PrintUnformatted(message); cJSON_Delete(message); if (!body) exit(2);
    printf("Content-Length: %zu\r\n\r\n%s",strlen(body),body); fflush(stdout); cJSON_free(body);
}
int main(void) {
    cJSON *message=receive(); if (!message) return 1;
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(message,"params");
    const cJSON *extension=cJSON_GetObjectItemCaseSensitive(p,"extension");
    const char *name=ced_ext_json_string(extension,"id"), *id=ced_ext_json_string(message,"id");
    if (!name || !id) return 2;
    if (!strcmp(name,"fixture.crash")) _exit(7);
    if (!strcmp(name,"fixture.malformed")) { fputs("Content-Length: -1\r\n\r\n",stdout); fflush(stdout); for (;;) pause(); }
    if (!strcmp(name,"fixture.hang")) { signal(SIGTERM,SIG_IGN); for (;;) pause(); }
    if (!strcmp(name,"fixture.flood")) {
        char bytes[8192]; memset(bytes,'x',sizeof bytes);
        for (int n=0;n<256;n++) if (write(STDERR_FILENO,bytes,sizeof bytes)<0) break;
    }
    cJSON *result=cJSON_CreateObject();
    cJSON_AddItemToObject(result,"extension",cJSON_Duplicate(extension,true));
    cJSON_AddStringToObject(result,"apiVersion",!strcmp(name,"fixture.incompatible") ? "99.0" : "1.0");
    cJSON_AddArrayToObject(result,"requiredHostMethods"); send_json(ced_ext_rpc_result(id,result)); cJSON_Delete(message);
    while ((message=receive())) {
        const char *method=ced_ext_json_string(message,"method"); id=ced_ext_json_string(message,"id");
        if (id) send_json(ced_ext_rpc_result(id,cJSON_CreateObject()));
        bool done=method && !strcmp(method,"ced/deactivate"); cJSON_Delete(message); if (done) break;
    }
    return 0;
}
