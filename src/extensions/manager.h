#ifndef CED_EXTENSION_MANAGER_H
#define CED_EXTENSION_MANAGER_H

#include "process.h"
#include "graph.h"
#include <pthread.h>

typedef struct {
    CedDocument *document;
    const char *language;
    size_t anchor, cursor;
} CedExtensionDocument;
typedef struct {
    void *user;
    CedCommandContext (*context)(void *user);
    bool (*document)(void *user, size_t index, CedExtensionDocument *document);
    bool (*commit)(void *user, CedDocument *document, uint64_t version,
                   const CedEditTransaction *transaction, char *error);
    void (*message)(void *user, const char *message);
    void (*open_log)(void *user, const char *path);
} CedExtensionHost;

#define CED_EXTENSION_TAB_CHOICES 256

typedef enum { EXT_REGISTERED, EXT_INITIALIZING, EXT_ACTIVATING, EXT_ACTIVE, EXT_STOPPING, EXT_FAILED } CedExtensionState;
struct CedExtensions;
typedef struct {
    cJSON *model;
    char input[8193];
    double next_refresh;
    float chat_bottom;
    bool chat_open, chat_following, chat_animate;
    bool requested, submitted;
} CedExtensionTabState;

typedef struct {
    struct CedExtensions *manager;
    CedExtensionRecord record;
    CedExtensionProcess *process;
    CedExtensionState state;
    char session[96], pending[64], command[257], error[CED_ERROR];
    char pending_method[64];
    cJSON *invocation_editor, *invocation_arguments;
    bool tab_action, retain_on_workspace_change, rebind_workspace;
    uint64_t render_request;
    bool render_click;
    CedExtensionTabState tabs[CED_EXTENSION_TAB_LIMIT];
    double deadline, request_started, message_window, violation_window;
    double activation_started, activation_ms;
    unsigned messages, violations;
    uint64_t invocation_document;
    uint64_t last_request, document_grants[32];
    size_t grant_count;
} CedExtensionInstance;
#define CED_EXTENSION_RENDER_EDGE 1024
#define CED_EXTENSION_RENDER_RESULT_LIMIT (4 * ((CED_EXTENSION_RENDER_EDGE * CED_EXTENSION_RENDER_EDGE * 4 + 2) / 3) + 2048)
typedef struct {
    unsigned char *pixels;
    int width, height;
    bool interactive;
} CedExtensionImage;
typedef struct {
    CedExtensionInstance *owner;
    const CedExtensionRenderer *renderer;
    uint64_t document, version, key, serial, graph_version;
    CedExtensionImage image;
    CedExtensionGraph *graph;
    char error[CED_ERROR];
} CedExtensionPreview;

typedef enum { EXT_LOAD, EXT_INSPECT, EXT_INSTALL, EXT_ENABLE, EXT_DISABLE, EXT_TRUST, EXT_REVOKE, EXT_UNINSTALL } CedExtensionJobKind;
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool running, done, ok, abandoned;
    struct CedExtensions *owner;
    CedExtensionJobKind kind;
    uint64_t generation;
    char profile[CED_PATH], workspace[CED_PATH], argument[CED_PATH], error[CED_ERROR];
    CedExtensionRecord review;
    CedExtensionCatalog catalog;
} CedExtensionJob;
typedef struct CedExtensions {
    CedCommands *commands;
    CedExtensionHost host;
    CedExtensionInstance instances[CED_EXTENSION_LIMIT];
    size_t count, next_instance;
    char profile[CED_PATH], workspace[CED_PATH], workspace_id[96], error[CED_ERROR];
    char reveal_tab[257];
    uint64_t generation, next_request, next_session, next_preview, next_image;
    CedExtensionPreview preview;
    bool trusted, reload, review_open, manage_open, trust_open;
    CedExtensionRecord review;
    CedExtensionJob job;
} CedExtensions;

/* workspace is the canonical root already resolved by CedWorkspace. */
CedExtensions *ced_extensions_create(CedCommands *commands, CedExtensionHost host, const char *profile, const char *workspace);
void ced_extensions_workspace(CedExtensions *manager, const char *workspace);
void ced_extensions_tick(CedExtensions *manager);
void ced_extensions_destroy(CedExtensions *manager);
bool ced_extensions_job(CedExtensions *manager, CedExtensionJobKind kind, const char *argument);
void ced_extensions_stop(CedExtensionInstance *instance, const char *reason);
bool ced_extensions_grant_document(CedExtensionInstance *instance, uint64_t document_id, bool allow);
unsigned ced_extensions_document_access(const CedExtensions *manager, const CedDocument *document);
void ced_extensions_document_changed(CedExtensions *manager, const CedDocument *document, unsigned previous_access);
void ced_extensions_document_closed(CedExtensions *manager, uint64_t document_id);
void ced_extensions_violation(CedExtensionInstance *instance, const char *reason);
const CedExtensionRecord *ced_extensions_language_provider(const CedExtensions *manager, unsigned language);
const char *ced_extensions_status(const CedExtensionInstance *instance);
bool ced_extensions_tab_action(CedExtensionInstance *instance, size_t tab, const char *action, const char *value);
const CedExtensionRenderer *ced_extensions_renderer(CedExtensions *manager, const char *path,
                                                    CedExtensionInstance **owner);
const CedExtensionPreview *ced_extensions_preview(CedExtensionInstance *instance, const CedExtensionRenderer *renderer,
                                                   const CedDocument *document);
bool ced_extensions_preview_click(CedExtensionInstance *instance, const CedExtensionRenderer *renderer,
                                  const CedDocument *document, int x, int y);
bool ced_extensions_graph_edit(CedExtensionInstance *instance, const CedExtensionRenderer *renderer,
                               const CedDocument *document, const cJSON *edit);
void ced_extensions_preview_clear(CedExtensions *manager);
bool ced_extension_image_decode(const cJSON *value, CedExtensionImage *image, char *error);
void ced_extension_image_free(CedExtensionImage *image);
/* Broker helpers, private to the extension subsystem. */
cJSON *ced_extensions_context(const CedExtensionInstance *instance);
cJSON *ced_extensions_editor(CedExtensionInstance *instance, CedCommandContext context);
bool ced_extensions_authorized(const CedExtensionInstance *instance, const CedDocument *document);
void ced_extensions_broker(CedExtensionInstance *instance, const cJSON *message);

#endif
