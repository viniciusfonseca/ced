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
/* A miniature .asln renderer. Its fixture format contains a list of steps.
 * The extension reads the editor's buffer and chooses all of the diagram's pixels. */
static cJSON *render_diagram(const cJSON *params) {
    const cJSON *editor = cJSON_GetObjectItemCaseSensitive(params, "editor");
    const cJSON *document = cJSON_GetObjectItemCaseSensitive(editor, "document");
    const char *id = ced_ext_json_string(document, "documentId");
    const cJSON *event = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(params, "arguments"), 0);
    const char *action = ced_ext_json_string(event, "action");
    if (!id || !action || (strcmp(action, "render") && strcmp(action, "click"))) return NULL;
    bool clicked = !strcmp(action, "click");
    uint64_t pixel_x = 0, pixel_y = 0;
    if (clicked && (!ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(event, "x"), &pixel_x) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(event, "y"), &pixel_y))) return NULL;

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    static unsigned counter;
    char request_id[40];
    snprintf(request_id, sizeof request_id, "ext:%u", ++counter);
    cJSON_AddStringToObject(request, "id", request_id);
    cJSON_AddStringToObject(request, "method", "documents/get");
    cJSON *get = cJSON_AddObjectToObject(request, "params");
    cJSON_AddStringToObject(get, "documentId", id);
    cJSON_AddItemToObject(get, "context", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(params, "context"), true));
    send_json(request);
    cJSON *answer = receive();
    const char *text = ced_ext_json_string(cJSON_GetObjectItemCaseSensitive(answer, "result"), "text");
    cJSON *model = text ? cJSON_Parse(text) : NULL;
    int steps = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(model, "steps"));
    cJSON_Delete(model); cJSON_Delete(answer);
    if (steps < 1 || steps > 4) return NULL;

    enum { WIDTH = 256, HEIGHT = 96, SIZE = WIDTH * HEIGHT * 4 };
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char *pixels = malloc(SIZE);
    char *encoded = malloc(SIZE / 3 * 4 + 1);
    if (!pixels || !encoded) { free(pixels); free(encoded); return NULL; }
    int spacing = WIDTH / steps;
    int selected = clicked && pixel_y >= 24 && pixel_y < 72 ? (int)pixel_x / spacing : -1;
    for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < WIDTH; x++) {
        int local = x % spacing;
        bool box = x / spacing < steps && local >= 8 && local < spacing - 8 && y >= 24 && y < 72;
        bool line = x >= 12 && x < spacing * steps - 12 && y >= 46 && y <= 49;
        int pos = (y * WIDTH + x) * 4;
        pixels[pos] = box ? (x / spacing == selected ? 240 : 52) : line ? 151 : 18;
        pixels[pos + 1] = box ? 125 : line ? 169 : 24;
        pixels[pos + 2] = box ? 210 : line ? 185 : 32;
        pixels[pos + 3] = 255;
    }
    for (int i = 0, j = 0; i < SIZE; i += 3, j += 4) {
        unsigned bits = ((unsigned)pixels[i] << 16) | ((unsigned)pixels[i + 1] << 8) | pixels[i + 2];
        for (int k = 0; k < 4; k++) encoded[j + k] = alphabet[(bits >> (18 - k * 6)) & 63];
    }
    encoded[SIZE / 3 * 4] = 0;
    cJSON *result = cJSON_CreateObject(), *render = cJSON_AddObjectToObject(result, "render");
    cJSON_AddNumberToObject(render, "width", WIDTH);
    cJSON_AddNumberToObject(render, "height", HEIGHT);
    cJSON_AddBoolToObject(render, "interactive", true);
    cJSON_AddStringToObject(render, "rgba", encoded);
    free(pixels); free(encoded);
    return result;
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
    bool renderer = !strcmp(name, "fixture.renderer");
    const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(p, "host"), "documentRenderProtocol");
    if (renderer && (!cJSON_IsNumber(protocol) || protocol->valueint != 1)) return 5;
    cJSON *result=cJSON_CreateObject();
    cJSON_AddItemToObject(result,"extension",cJSON_Duplicate(extension,true));
    cJSON_AddStringToObject(result,"apiVersion",!strcmp(name,"fixture.incompatible") ? "99.0" : "1.0");
    cJSON_AddArrayToObject(result,"requiredHostMethods"); send_json(ced_ext_rpc_result(id,result)); cJSON_Delete(message);
    while ((message=receive())) {
        const char *method=ced_ext_json_string(message,"method"); id=ced_ext_json_string(message,"id");
        if (id && renderer && method && !strcmp(method, "commands/invoke")) {
            cJSON *render = render_diagram(cJSON_GetObjectItemCaseSensitive(message, "params"));
            send_json(render ? ced_ext_rpc_result(id, render) : ced_ext_rpc_error(id, -32602, "InvalidParams", "Invalid diagram fixture"));
        } else if (id) send_json(ced_ext_rpc_result(id,cJSON_CreateObject()));
        bool done=method && !strcmp(method,"ced/deactivate"); cJSON_Delete(message); if (done) break;
    }
    return 0;
}
