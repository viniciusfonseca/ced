#ifndef CED_LSP_H
#define CED_LSP_H

#include "workspace.h"

#define CED_LSP_DIAGNOSTICS 128
#define CED_LSP_COMPLETIONS 80
typedef struct CedLsp CedLsp;
typedef enum { CED_LSP_C, CED_LSP_JAVA, CED_LSP_KOTLIN, CED_LSP_NONE } CedLspLanguage;
typedef struct {
    const char *root, *label;
    CedLspLanguage language;
    const char *const *argv;
    const char *settings_json, *maven_settings;
} CedLspServer;
typedef struct { int line, character; } CedLspPosition; /* zero-based UTF-16 */
typedef struct {
    int start, end, severity;
    char message[512];
} CedDiagnostic;
typedef struct {
    char label[160], detail[256], text[1024];
    int start, end;
} CedCompletion;
typedef enum { CED_LSP_COMPLETION, CED_LSP_HOVER, CED_LSP_DEFINITION } CedLspRequest;
typedef struct {
    bool ready;
    CedLspRequest kind;
    char path[CED_PATH], error[512];
    int offset;
    CedCompletion completions[CED_LSP_COMPLETIONS];
    int count;
    char hover[16384];
    char target[CED_PATH];
    CedLspPosition position;
} CedLspResult;

CedLsp *ced_lsp_start(const char *root, const char *executable, char *error, size_t cap);
CedLsp *ced_lsp_start_server(const CedLspServer *server, char *error, size_t cap);
void ced_lsp_stop(CedLsp *lsp);
void ced_lsp_poll(CedLsp *lsp);
bool ced_lsp_ready(const CedLsp *lsp);
bool ced_lsp_running(const CedLsp *lsp);
bool ced_lsp_pending(const CedLsp *lsp);
const char *ced_lsp_status(const CedLsp *lsp);
bool ced_lsp_sync(CedLsp *lsp, const char *path, const char *text);
void ced_lsp_close(CedLsp *lsp, const char *path);
void ced_lsp_saved(CedLsp *lsp, const char *path);
bool ced_lsp_request(CedLsp *lsp, CedLspRequest kind, const char *path, const char *text, int offset);
CedLspResult *ced_lsp_result(CedLsp *lsp);
const CedDiagnostic *ced_lsp_diagnostics(CedLsp *lsp, const char *path, int *count);
CedLspLanguage ced_lsp_language_kind(const char *path);
bool ced_lsp_language(const char *path);
char *ced_lsp_uri(const char *path);
bool ced_lsp_path(const char *uri, char *out, size_t cap);
CedLspPosition ced_lsp_position(const char *text, int offset);
int ced_lsp_offset(const char *text, CedLspPosition position);

#endif
