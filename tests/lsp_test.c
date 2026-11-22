#include "lsp.h"
#include "output.h"
#include "documents.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int allocations_left, json_live;
static bool allocation_failed;

static void *json_alloc(size_t size) {
    if (allocations_left == 0) {
        allocations_left = -1;
        allocation_failed = true;
        return NULL;
    }
    if (allocations_left > 0) allocations_left--;
    void *p = malloc(size);
    if (p) json_live++;
    return p;
}

static void json_free(void *p) {
    if (p) { json_live--; free(p); }
}

static void test_json_ownership(void) {
    cJSON_Hooks hooks = {json_alloc, json_free};
    cJSON_InitHooks(&hooks);
    bool exhausted = false;
    for (int i = 0; i < 512; i++) {
        allocations_left = i;
        allocation_failed = false;
        char error[CED_ERROR];
        CedLsp *lsp = ced_lsp_start("/tmp", "/usr/bin/true", error, sizeof error);
        ced_lsp_stop(lsp);
        if (json_live) fprintf(stderr, "JSON leak at allocation %d: %d live\n", i, json_live);
        assert(json_live == 0);
        if (!allocation_failed) { exhausted = true; break; }
    }
    assert(exhausted);
    cJSON_InitHooks(NULL);
    puts("OK: JSON ownership under allocation failures");
}

static void tick(CedLsp *lsp) {
    ced_lsp_poll(lsp);
    struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
}

static void test_output(void) {
    ced_output_clear();
    char error[CED_ERROR], snapshot[CED_OUTPUT_CAP];
    uint64_t revision=0;
    const char *argv[]={"/bin/sh","-c","printf 'launcher diagnostic\\n' >&2; exit 7",NULL};
    CedLspServer server={.root="/tmp",.label="Example LSP",.language=CED_LSP_C,.argv=argv};
    CedLsp *lsp=ced_lsp_start_server(&server,error,sizeof error);
    assert(lsp);
    for (int i=0;i<100 && ced_lsp_running(lsp);i++) tick(lsp);
    assert(!ced_lsp_running(lsp));
    ced_lsp_stop(lsp);
    ced_output_copy(snapshot,&revision);
    assert(strstr(snapshot,"[Example LSP] launcher diagnostic\n"));
    ced_output_text("Test","é🙂\r\nline\x1b\x01");
    ced_output_copy(snapshot,&revision);
    assert(strstr(snapshot,"[Test] é🙂\nline\n"));
    char line[8192];
    memset(line,'x',sizeof line);
    for (int i=0;i<20;i++) ced_output_write("Flood",line,sizeof line);
    ced_output_text("Test","latest");
    ced_output_copy(snapshot,&revision);
    assert(strlen(snapshot)<CED_OUTPUT_CAP && ced_utf8_valid(snapshot,strlen(snapshot)));
    assert(!strstr(snapshot,"launcher diagnostic") && strstr(snapshot,"[Test] latest"));
    ced_output_clear();
    ced_output_copy(snapshot,&revision);
    assert(!*snapshot);
}

static CedLspResult *result(CedLsp *lsp) {
    for (int i = 0; i < 800; i++) {
        tick(lsp);
        if (ced_lsp_result(lsp)->ready) return ced_lsp_result(lsp);
        assert(ced_lsp_running(lsp));
    }
    fprintf(stderr, "LSP response timed out\n"); abort();
}

int main(int argc, char **argv) {
    test_json_ownership();
    test_output();
    const char *unicode = "\té🙂x\nabc";
    CedLspPosition p = ced_lsp_position(unicode, 7);
    assert(p.line == 0 && p.character == 4);
    assert(ced_lsp_offset(unicode, p) == 7);
    assert(ced_lsp_offset(unicode, (CedLspPosition){0, 3}) == 3);
    assert(ced_lsp_offset(unicode, (CedLspPosition){1, 2}) == 11);
    assert(ced_lsp_offset(unicode, (CedLspPosition){50, 50}) == (int)strlen(unicode));
    char *uri = ced_lsp_uri("/tmp/a b#%ü.c"), decoded[CED_PATH];
    assert(uri && strstr(uri, "%20") && strstr(uri, "%23") && strstr(uri, "%25"));
    assert(ced_lsp_path(uri, decoded, sizeof decoded)); assert(!strcmp(decoded, "/tmp/a b#%ü.c")); free(uri);
    assert(!ced_lsp_path("https://example.com", decoded, sizeof decoded));
    assert(!ced_lsp_path("file:///tmp/%00.c", decoded, sizeof decoded));
    assert(!ced_lsp_path("file:///tmp/%xz", decoded, sizeof decoded));
    char error[CED_ERROR];
    assert(!ced_lsp_start("/tmp", "/does/not/exist/clangd", error, sizeof error));
    CedLsp *dead = ced_lsp_start("/tmp", "/usr/bin/true", error, sizeof error);
    assert(dead);
    for (int i = 0; i < 100 && ced_lsp_running(dead); i++) tick(dead);
    assert(!ced_lsp_running(dead)); ced_lsp_stop(dead);
    puts("OK: UTF-16 positions, escaped URIs, missing/dead server");

    char temp[] = "/tmp/ced-lsp-XXXXXX", root[CED_PATH], path[CED_PATH];
    assert(mkdtemp(temp)); assert(realpath(temp, root));
    assert(ced_join(path, sizeof path, root, "sample ü.c"));
    const char *text = "int answer(void) { return 42; }\nint main(void) { return answer(); }\n";
    FILE *f = fopen(path, "w"); assert(f); fputs(text, f); fclose(f);
    CedLsp *lsp = ced_lsp_start(root, argc > 1 ? argv[1] : "clangd", error, sizeof error);
    if (!lsp) { fprintf(stderr, "%s\n", error); return 77; }
    for (int i = 0; i < 800 && !ced_lsp_ready(lsp); i++) { tick(lsp); assert(ced_lsp_running(lsp)); }
    assert(ced_lsp_ready(lsp));
    assert(ced_lsp_sync(lsp, path, text));
    int offset = (int)(strstr(strchr(text, '\n'), "answer") - text) + 2;
    assert(ced_lsp_request(lsp, CED_LSP_DEFINITION, path, text, offset));
    CedLspResult *r = result(lsp);
    assert(!r->error[0]); assert(!strcmp(r->target, path)); assert(r->position.line == 0);
    assert(ced_lsp_request(lsp, CED_LSP_HOVER, path, text, offset));
    r = result(lsp); assert(!r->error[0] && strstr(r->hover, "answer"));
    offset = (int)(strstr(strchr(text, '\n'), "answer") - text) + 3;
    assert(ced_lsp_request(lsp, CED_LSP_COMPLETION, path, text, offset));
    r = result(lsp);
    bool middle_completion = false;
    for (int i = 0; i < r->count; i++) if (strstr(r->completions[i].label, "answer")) {
        assert(r->completions[i].start == offset - 3);
        assert(r->completions[i].end == offset + 3);
        middle_completion = true;
    }
    assert(middle_completion);
    text = "int answer(void) { return 42; }\nint main(void) { return ans; }\n";
    offset = (int)(strstr(strchr(text, '\n'), "ans") - text) + 3;
    assert(ced_lsp_request(lsp, CED_LSP_COMPLETION, path, text, offset));
    r = result(lsp); assert(!r->error[0]);
    bool found = false;
    for (int i = 0; i < r->count; i++) {
        if (strstr(r->completions[i].label, "answer")) {
            found = true;
            assert(r->completions[i].start == offset - 3);
            assert(r->completions[i].end >= r->completions[i].start);
        }
    }
    assert(found);
    text = "int main(void) { return missing_name; }\n";
    assert(ced_lsp_sync(lsp, path, text));
    int count = 0;
    const CedDiagnostic *diagnostics = NULL;
    for (int i = 0; i < 800 && !count; i++) { tick(lsp); diagnostics = ced_lsp_diagnostics(lsp, path, &count); }
    assert(count > 0);
    found = false;
    for (int i = 0; i < count; i++) if (diagnostics[i].severity == 1 && strstr(diagnostics[i].message, "missing_name")) found = true;
    assert(found);
    assert(ced_lsp_sync(lsp, path, "int main(void) { return 0; }\n"));
    ced_lsp_saved(lsp, path);
    for (int i = 0; i < 100; i++) tick(lsp);
    ced_lsp_diagnostics(lsp, path, &count); assert(count == 0);
    ced_lsp_close(lsp, path); assert(!ced_lsp_diagnostics(lsp, path, &count) && count == 0);
    /* Removing a middle document must preserve its neighbors and free a slot. */
    char other[CED_PATH], last[CED_PATH];
    assert(ced_join(other, sizeof other, root, "other.c"));
    assert(ced_join(last, sizeof last, root, "last.c"));
    assert(ced_lsp_sync(lsp, path, text));
    assert(ced_lsp_sync(lsp, other, text));
    assert(ced_lsp_sync(lsp, last, text));
    ced_lsp_close(lsp, other);
    assert(ced_lsp_diagnostics(lsp, path, &count));
    assert(ced_lsp_diagnostics(lsp, last, &count));
    assert(!ced_lsp_diagnostics(lsp, other, &count));
    assert(ced_lsp_sync(lsp, other, text));
    ced_lsp_stop(lsp);
    const char *remove[] = {"rm", "-rf", "--", root, NULL};
    char *output = NULL; assert(!ced_command(remove, &output, NULL)); free(output);
    puts("OK: real clangd initialize, unsaved changes, completion, hover, definition, diagnostics, didClose and shutdown");
    return 0;
}
