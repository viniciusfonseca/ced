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
static void hang(void) {
#ifdef _WIN32
    Sleep(INFINITE);
#else
    for (;;) pause();
#endif
}

#ifdef _WIN32
static bool start_descendant(void) {
    wchar_t image[CED_PATH], arguments[] = L"fixture.exe --descendant";
    DWORD length = GetModuleFileNameW(NULL, image, CED_PATH);
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION child;
    if (!length || length >= CED_PATH ||
        !CreateProcessW(image, arguments, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child))
        return false;
    char marker[64];
    int size = snprintf(marker, sizeof marker, "descendant pid=%lu\n", (unsigned long)child.dwProcessId);
    write(STDERR_FILENO, marker, (size_t)size);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return true;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    ced_windows_init();
    if (GetEnvironmentVariableW(L"CED_EXTENSION_TEST_PRIVATE", NULL, 0)) return 3;
    if (argc == 2 && !strcmp(argv[1], "--descendant")) hang();
#else
    (void)argc; (void)argv;
#endif
    cJSON *message=receive(); if (!message) return 1;
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(message,"params");
    const cJSON *extension=cJSON_GetObjectItemCaseSensitive(p,"extension");
    const char *name=ced_ext_json_string(extension,"id"), *id=ced_ext_json_string(message,"id");
    if (!name || !id) return 2;
    if (!strcmp(name,"fixture.crash")) _exit(7);
    if (!strcmp(name,"fixture.malformed")) { fputs("Content-Length: -1\r\n\r\n",stdout); fflush(stdout); hang(); }
    if (!strcmp(name,"fixture.hang")) {
#ifdef _WIN32
        if (!start_descendant()) return 4;
#else
        signal(SIGTERM,SIG_IGN);
#endif
        hang();
    }
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
