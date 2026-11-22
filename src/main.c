#include "workspace.h"
#include "preferences.h"
#include "editor_font.h"
#include "lsp.h"
#include "terminal_view.h"
#include "editor_assist.h"
#include "file_operations.h"
#include "process_memory.h"
#include "editor_logo.h"
#include "commands.h"
#include "editor_bridge.h"
#ifdef CED_ENABLE_EXTENSIONS
#include "extensions/extension_ui.h"
#endif
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DOCS 32

#ifdef __APPLE__
void ced_macos_set_icon(void);
void ced_macos_titlebar(cui_app *app);
float ced_macos_layout_titlebar(cui_app *app, cui_rect toolbar);
void *ced_macos_window_events(cui_app *app, cui_id drag_region);
void ced_macos_remove_window_events(void *handle);
#endif

typedef struct {
    CedDocument file;
    int id;
    char title[CED_PATH + 40];
    bool focus;
    bool lsp_synced;
    int cursor, anchor;
    bool markdown_preview;
} EditorTab;

typedef enum { CED_FILE_CREATE, CED_FILE_RENAME, CED_FILE_DELETE, CED_DIRECTORY_CREATE } FileOperation;
typedef struct {
    FileOperation operation;
    bool open, execute, directory;
    int serial;
    char path[CED_PATH], name[256], error[CED_ERROR];
} FileAction;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool running, done, ok, pull;
    char root[CED_PATH], error[CED_ERROR];
} GitSync;

typedef struct {
    bool open, execute;
    char root[CED_PATH], path[CED_PATH], error[CED_ERROR];
} GitDiscard;

typedef struct {
    CedWorkspace workspace;
    CedGit git;
    GitSync git_sync;
    GitDiscard git_discard;
    CedSearch search;
    EditorTab docs[MAX_DOCS];
    int count, selected, next_id, panel, close_index;
    int close_keep_id;
    float split;
    int font_size;
    CedPreferences preferences;
    CedRecentFolders recent_folders;
    bool open_menu_requested;
    CedEditorFont editor_font;
    bool preferences_loaded, preferences_reload;
    double next_preferences_poll;
    char preferences_error[CED_ERROR];
    uint64_t memory_usage;
    double next_memory_sample;
    bool sidebar, close_dialog, commit_dialog, palette;
    bool diff, diff_staged;
    char diff_path[CED_PATH], *before, *after;
    char query[256], commit[1024], message[CED_ERROR];
    bool search_include_ignored;
    char recovery[CED_PATH];
    int goto_line, goto_column;
    cui_code_editor_result caret;
    cui_app *app;
    cui_texture logo;
    CedLsp *lsp;
    bool lsp_attempted, problems_open;
    CedEditorAssist assist;
    int goto_offset;
    CedTerminal *terminal;
    CedTerminalView terminal_view;
    bool terminal_visible, terminal_right;
    char terminal_tab[CED_TERMINAL_TAB_CAPACITY];
    float terminal_bottom_split, terminal_right_split;
    FileAction file_action;
    CedCommands commands;
    cui_command palette_items[1024];
    char palette_ids[1024][257];
    uint64_t selection_version, selection_document;
    size_t selection_anchor, selection_cursor;
#ifdef CED_ENABLE_EXTENSIONS
    CedExtensions *extensions;
    char sidebar_tab[257];
#endif
} Editor;
static void dispatch(Editor *e, const char *id);
static void remove_tab(Editor *e, int index);

static bool extension_modal(const Editor *e) {
#ifdef CED_ENABLE_EXTENSIONS
    return e->extensions && (e->extensions->manage_open || e->extensions->trust_open || e->extensions->review_open);
#else
    (void)e; return false;
#endif
}

static void notify(Editor *e, const char *message) {
    snprintf(e->message, sizeof e->message, "%s", message);
}

static void refresh_git(Editor *e) {
    char error[CED_ERROR];
    if (!ced_git_refresh(&e->git, e->workspace.root, error)) {
        if (e->git.available) notify(e, error);
    }
}

static void refresh(Editor *e) {
    char root[CED_PATH], error[CED_ERROR];
    snprintf(root, sizeof root, "%s", e->workspace.root);
    if (!ced_workspace_open(&e->workspace, root, error)) notify(e, error);
    refresh_git(e);
}

static void *git_sync_worker(void *user) {
    GitSync *job = user;
    CedGit git = {0};
    snprintf(git.root, sizeof git.root, "%s", job->root);
    bool ok = ced_git_sync(&git, job->pull, job->error);
    pthread_mutex_lock(&job->mutex);
    job->ok = ok;
    job->done = true;
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

static void start_git_sync(Editor *e, bool pull) {
    GitSync *job = &e->git_sync;
    if (job->running) return;
    snprintf(job->root, sizeof job->root, "%s", e->git.root);
    job->pull = pull; job->done = false; job->error[0] = 0;
    if (pthread_create(&job->thread, NULL, git_sync_worker, job)) {
        notify(e, "Could not start Git operation.");
        return;
    }
    job->running = true;
    notify(e, pull ? "Pulling..." : "Pushing...");
}

static void poll_git_sync(cui_ctx *ui, Editor *e) {
    GitSync *job = &e->git_sync;
    if (!job->running) return;
    pthread_mutex_lock(&job->mutex);
    bool done = job->done;
    pthread_mutex_unlock(&job->mutex);
    if (!done) { cui_request_wakeup(ui, 0.1f); return; }
    pthread_join(job->thread, NULL);
    job->running = false;
    if (job->ok && job->pull) { e->diff = false; refresh(e); }
    else refresh_git(e);
    notify(e, job->ok ? (job->pull ? "Pull completed." : "Push completed.") : job->error);
}

static void tab_title(EditorTab *tab) {
    snprintf(tab->title, sizeof tab->title, "%s##document-%d",
             tab->file.path[0] ? ced_basename(tab->file.path) : "Untitled", tab->id);
}

static EditorTab *new_tab(Editor *e) {
    if (e->count >= MAX_DOCS) { notify(e, "Maximum of 32 open tabs reached."); return NULL; }
    EditorTab *tab = &e->docs[e->count];
    memset(tab, 0, sizeof *tab);
    tab->id = ++e->next_id;
    tab->focus = true;
    e->selected = e->count++;
    e->diff = false;
    return tab;
}

static void new_document(Editor *e) {
    EditorTab *tab = new_tab(e);
    if (!tab) return;
    tab->file.text = calloc(CED_TEXT_CAP, 1);
    if (!tab->file.text) { e->count--; e->selected = e->count - 1; notify(e, "Out of memory."); return; }
    ced_document_identify(&tab->file, (uint64_t)tab->id);
    tab_title(tab);
}

static bool open_document(Editor *e, const char *path, int line, int column) {
    char canonical[CED_PATH], error[CED_ERROR];
    if (!realpath(path, canonical)) { notify(e, "File no longer available."); return false; }
    for (int i = 0; i < e->count; i++) {
        if (!strcmp(e->docs[i].file.path, canonical)) {
            e->selected = i;
            e->diff = false;
            e->docs[i].focus = true;
            e->goto_line = line; e->goto_column = column;
            return true;
        }
    }
    CedDocument file = {0};
    if (!ced_document_open(&file, canonical, error)) { notify(e, error); return false; }
    EditorTab *tab = new_tab(e);
    if (!tab) { ced_document_free(&file); return false; }
    tab->file = file;
    ced_document_identify(&tab->file, (uint64_t)tab->id);
    tab_title(tab);
    e->goto_line = line; e->goto_column = column;
    return true;
}

static void choose_file(Editor *e) {
    char path[CED_PATH];
    if (cui_app_file_dialog(e->app, &(cui_file_dialog_opts){.kind = CUI_FILE_OPEN, .title = "Open File"}, path, sizeof path))
        open_document(e, path, 0, 0);
}

static bool save_document(Editor *e, int index, bool save_as) {
    if (index < 0 || index >= e->count) return false;
    EditorTab *tab = &e->docs[index];
    char path[CED_PATH], error[CED_ERROR];
    snprintf(path, sizeof path, "%s", tab->file.path);
    bool chosen = save_as || !path[0];
    if (chosen && !cui_app_file_dialog(e->app, &(cui_file_dialog_opts){
            .kind = CUI_FILE_SAVE, .title = "Save File", .default_name = path[0] ? ced_basename(path) : "untitled.txt"}, path, sizeof path)) return false;
    char canonical[CED_PATH];
    if (realpath(path, canonical)) snprintf(path, sizeof path, "%s", canonical);
    for (int i = 0; i < e->count; i++) {
        if (i != index && !strcmp(e->docs[i].file.path, path)) { notify(e, "File already open in another tab."); return false; }
    }
    /* A native Save dialog confirms replacing an existing destination. */
    char settings_path[CED_PATH];
    bool preferences_file = ced_preferences_path(settings_path,error) && !strcmp(path,settings_path);
    if (preferences_file) {
        CedPreferences candidate;
        if (!ced_preferences_validate(tab->file.text,strlen(tab->file.text),&candidate,error)) { notify(e,error); return false; }
    }
    char previous_path[CED_PATH];
    snprintf(previous_path, sizeof previous_path, "%s", tab->file.path);
#ifdef CED_ENABLE_EXTENSIONS
    unsigned previous_access = ced_extensions_document_access(e->extensions, &tab->file);
#endif
    if (!ced_document_save(&tab->file, path, chosen, error)) { notify(e, error); return false; }
    if (preferences_file) e->preferences_reload = true;
#ifdef CED_ENABLE_EXTENSIONS
    ced_extensions_document_changed(e->extensions, &tab->file, previous_access);
#endif
    if (strcmp(previous_path, tab->file.path)) ced_lsp_close(e->lsp, previous_path);
    tab->lsp_synced = ced_lsp_sync(e->lsp, tab->file.path, tab->file.text);
    ced_lsp_saved(e->lsp, tab->file.path);
    tab_title(tab);
    notify(e, "Saved.");
    refresh(e);
    return true;
}

static void perform_git_discard(Editor *e) {
    GitDiscard *action = &e->git_discard;
    if (!action->execute) return;
    action->execute = false;
    if (strcmp(action->root,e->git.root)) { notify(e,"The repository changed. Discard was cancelled."); return; }
    if (e->git_sync.running) { snprintf(action->error,sizeof action->error,"Wait for the Git operation to finish."); action->open = true; return; }
    if (!ced_git_discard(&e->git,action->path,action->error)) { action->open = true; refresh_git(e); return; }
    char path[CED_PATH];
    if (ced_join(path,sizeof path,action->root,action->path)) {
        for (int n = e->count - 1; n >= 0; n--) {
            EditorTab *tab = &e->docs[n];
            if (strcmp(tab->file.path,path)) continue;
            struct stat st;
            if (lstat(path,&st) || !S_ISREG(st.st_mode)) { remove_tab(e,n); continue; }
            CedDocument restored = {0};
            if (!ced_document_open(&restored,path,action->error)) { notify(e,action->error); continue; }
#ifdef CED_ENABLE_EXTENSIONS
            ced_extensions_document_closed(e->extensions,tab->file.id);
#endif
            ced_lsp_close(e->lsp,tab->file.path);
            ced_document_free(&tab->file); tab->file = restored;
            tab->id = ++e->next_id;
            ced_document_identify(&tab->file,(uint64_t)tab->id);
            tab->anchor = tab->cursor = 0; tab->lsp_synced = false; tab->focus = n == e->selected;
            tab_title(tab);
        }
    }
    e->diff = false; e->goto_line = 0; e->goto_offset = -1;
    ced_assist_reset(&e->assist);
    refresh(e);
    if (!*action->error) notify(e,"Changes discarded.");
}

static void remove_tab(Editor *e, int index) {
#ifdef CED_ENABLE_EXTENSIONS
    ced_extensions_document_closed(e->extensions, e->docs[index].file.id);
#endif
    ced_lsp_close(e->lsp, e->docs[index].file.path);
    ced_document_free(&e->docs[index].file);
    memmove(e->docs + index, e->docs + index + 1, sizeof *e->docs * (size_t)(e->count - index - 1));
    e->count--;
    memset(&e->docs[e->count], 0, sizeof *e->docs);
    if (e->selected > index) e->selected--;
    if (e->selected >= e->count) e->selected = e->count - 1;
    if (e->selected >= 0) e->docs[e->selected].focus = true;
}

static void request_close(Editor *e, int index) {
    if (index < 0 || index >= e->count) return;
    if (ced_document_dirty(&e->docs[index].file)) {
        e->close_index = index;
        e->close_dialog = true;
    } else remove_tab(e, index);
}

static void close_tabs_except(Editor *e, int keep_id) {
    for (int i = e->count - 1; i >= 0; i--)
        if (e->docs[i].id != keep_id) remove_tab(e, i);
    if (e->count) {
        e->selected = 0;
        e->docs[0].focus = true;
    }
    e->diff = false;
    ced_assist_reset(&e->assist);
}

static void request_close_tabs(Editor *e, int keep_id) {
    for (int i = 0; i < e->count; i++) {
        if (e->docs[i].id != keep_id && ced_document_dirty(&e->docs[i].file)) {
            e->close_index = -1;
            e->close_keep_id = keep_id;
            e->close_dialog = true;
            return;
        }
    }
    close_tabs_except(e, keep_id);
}

static void poll_preferences(cui_ctx *ui, Editor *e) {
    double now=cui_time(ui);
    if (!e->preferences_reload && now<e->next_preferences_poll) return;
    bool force=e->preferences_reload; e->preferences_reload=false; e->next_preferences_poll=now+1;
    CedPreferences settings; char error[CED_ERROR];
    if (!ced_preferences_load(&settings,error)) {
        if (strcmp(error,e->preferences_error)) notify(e,error);
        snprintf(e->preferences_error,sizeof e->preferences_error,"%s",error); return;
    }
    bool visibility_changed=force || !e->preferences_loaded || settings.terminal_visible!=e->preferences.terminal_visible;
    bool tab_changed=force || !e->preferences_loaded || strcmp(settings.terminal_tab,e->preferences.terminal_tab);
    bool settings_changed=visibility_changed || tab_changed || settings.font_size!=e->preferences.font_size ||
        settings.terminal_right!=e->preferences.terminal_right || strcmp(settings.font_family,e->preferences.font_family) || strcmp(settings.font_path,e->preferences.font_path);
    bool font_changed=force || !e->preferences_loaded || *e->preferences_error || strcmp(settings.font_family,e->preferences.font_family) || strcmp(settings.font_path,e->preferences.font_path);
    if (font_changed && !ced_editor_font_apply(ui,&e->editor_font,&settings,error)) {
        if (strcmp(error,e->preferences_error)) notify(e,error);
        snprintf(e->preferences_error,sizeof e->preferences_error,"%s",error);
    } else if (font_changed) {
        if (*e->preferences_error && !strcmp(e->message,e->preferences_error)) notify(e,"Preferences reloaded.");
        *e->preferences_error=0;
    }
    e->font_size=settings.font_size; e->terminal_right=settings.terminal_right;
    if (visibility_changed) e->terminal_visible=settings.terminal_visible;
    if (tab_changed) snprintf(e->terminal_tab,sizeof e->terminal_tab,"%s",settings.terminal_tab);
    e->preferences=settings; e->preferences_loaded=true;
    if (!settings_changed) return;
    /* Refresh only clean settings buffers; never replace unsaved JSON edits. */
    char path[CED_PATH];
    if (!ced_preferences_path(path,error)) return;
    for (int n=0;n<e->count;n++) {
        EditorTab *tab=&e->docs[n];
        if (strcmp(tab->file.path,path) || ced_document_dirty(&tab->file)) continue;
        CedDocument next={0};
        if (!ced_document_open(&next,path,error)) continue;
        if (!strcmp(next.text,tab->file.text)) { ced_document_free(&next); continue; }
#ifdef CED_ENABLE_EXTENSIONS
        ced_extensions_document_closed(e->extensions,tab->file.id);
#endif
        ced_lsp_close(e->lsp,tab->file.path); ced_document_free(&tab->file); tab->file=next;
        tab->id=++e->next_id; ced_document_identify(&tab->file,(uint64_t)tab->id);
        size_t cursor=(size_t)tab->cursor;
        if (cursor>next.length) cursor=next.length;
        while (cursor && !ced_utf8_boundary(next.text,next.length,cursor)) cursor--;
        tab->anchor=tab->cursor=(int)cursor; tab->lsp_synced=false; tab->focus=n==e->selected;
        tab_title(tab);
    }
}

/* CUI has no public close-veto callback. Keep unsaved buffers in a private
 * recovery file when the OS closes the window; restore on the next launch. */
static void recovery_path(Editor *e) {
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[CED_PATH];
    if (!ced_join(dir, sizeof dir, home, ".ced")) return;
    if (mkdir(dir, 0700) && errno != EEXIST) return;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)e->workspace.root; *p; p++) hash = (hash ^ *p) * UINT64_C(1099511628211);
    snprintf(e->recovery, sizeof e->recovery, "%s/recovery-%016llx", dir, (unsigned long long)hash);
}

static bool write_recovery(Editor *e) {
    if (!e->recovery[0]) return false;
    bool dirty = false;
    for (int i = 0; i < e->count; i++) dirty |= ced_document_dirty(&e->docs[i].file);
    if (!dirty) { unlink(e->recovery); return true; }
    char temp[CED_PATH];
    int n = snprintf(temp, sizeof temp, "%s.XXXXXX", e->recovery);
    if (n < 0 || (size_t)n >= sizeof temp) return false;
    int fd = mkstemp(temp);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(temp); return false; }
    bool ok = fwrite("CED1", 1, 4, f) == 4;
    for (int i = 0; i < e->count && ok; i++) {
        CedDocument *d = &e->docs[i].file;
        if (!ced_document_dirty(d)) continue;
        uint32_t header[] = {(uint32_t)strlen(d->path), (uint32_t)strlen(d->text), d->crlf};
        ok = fwrite(header, sizeof header, 1, f) == 1 &&
             fwrite(d->path, 1, header[0], f) == header[0] && fwrite(d->text, 1, header[1], f) == header[1];
    }
    if (fflush(f) || fsync(fd)) ok = false;
    if (fclose(f)) ok = false;
    if (ok && rename(temp, e->recovery)) ok = false;
    if (!ok) unlink(temp);
    return ok;
}

static void restore_recovery(Editor *e) {
    FILE *f = fopen(e->recovery, "rb");
    if (!f) return;
    char magic[4];
    int restored = 0;
    if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "CED1", 4)) {
        uint32_t header[3];
        while (e->count < MAX_DOCS && fread(header, sizeof header, 1, f) == 1) {
            if (header[0] >= CED_PATH || header[1] >= CED_TEXT_CAP) break;
            char path[CED_PATH] = {0};
            char *text = calloc(CED_TEXT_CAP, 1);
            if (!text) break;
            if (fread(path, 1, header[0], f) != header[0] || fread(text, 1, header[1], f) != header[1]) { free(text); break; }
            EditorTab *tab = new_tab(e);
            if (!tab) { free(text); break; }
            char error[CED_ERROR];
            if (path[0]) ced_document_open(&tab->file, path, error);
            free(tab->file.text);
            tab->file.text = text;
            snprintf(tab->file.path, sizeof tab->file.path, "%s", path);
            tab->file.crlf = header[2] != 0;
            ced_document_identify(&tab->file, (uint64_t)tab->id);
            tab_title(tab);
            restored++;
        }
    }
    fclose(f);
    if (restored) notify(e, "Recovered unsaved files.");
}

static bool open_folder(Editor *e, const char *path) {
    if (e->git_sync.running) { notify(e, "Wait for the Git operation to finish."); return false; }
    if (!strcmp(path, e->workspace.root)) return true;
    if (!write_recovery(e)) { notify(e, "Recovery failed. Save your files before switching folders."); return false; }
    CedWorkspace next = {0};
    char error[CED_ERROR];
    if (!ced_workspace_open(&next, path, error)) { notify(e, error); return false; }
#ifdef CED_ENABLE_EXTENSIONS
    ced_extensions_workspace(e->extensions, next.root);
    if (!ced_extension_tab_find(e->extensions, e->sidebar_tab, NULL, NULL)) *e->sidebar_tab = 0;
#endif
    ced_lsp_stop(e->lsp); e->lsp = NULL; e->lsp_attempted = false;
    ced_assist_reset(&e->assist);
    ced_terminal_stop(e->terminal); e->terminal = NULL;
    e->terminal_view = (CedTerminalView){.autofocus = true};
    ced_search_cancel(&e->search);
    while (e->count) remove_tab(e, e->count - 1);
    ced_workspace_free(&e->workspace);
    e->workspace = next;
    e->diff = false;
    e->query[0] = 0;
    e->search.count = 0;
    refresh_git(e);
    recovery_path(e);
    restore_recovery(e);
    if (!e->count) notify(e, "Folder opened.");
    if (!ced_recent_folder_record(e->workspace.root)) notify(e, "Could not save recent folders.");
    return true;
}

static void choose_folder(Editor *e) {
    if (e->git_sync.running) { notify(e, "Wait for the Git operation to finish."); return; }
    char path[CED_PATH];
    if (cui_app_file_dialog(e->app, &(cui_file_dialog_opts){.kind = CUI_FILE_FOLDER, .title = "Open Folder"}, path, sizeof path))
        open_folder(e, path);
}

static void open_folder_menu(cui_ctx *ui, Editor *e) {
    const char *id = "open-folder-menu";
    bool open = cui_popover_is_open(ui, id);
    bool clicked = cui_toolbar_button(ui, CUI_ICON_FOLDER_OPEN, .tooltip = "Open Folder", .flyout = true);
    bool keyboard = cui_key_pressed(ui, CUI_KEY_ENTER) || cui_key_pressed(ui, CUI_KEY_SPACE);
    if ((!open && clicked) || e->open_menu_requested) {
        char error[CED_ERROR];
        if (!ced_recent_folders_load(&e->recent_folders, error)) notify(e, error);
        cui_popover_open(ui, id);
        e->open_menu_requested = false;
        ced_assist_reset(&e->assist);
    } else if (clicked && !keyboard) {
        cui_popover_close(ui, id);
    }
    if (ced_anchored_menu_begin(ui, id, 360)) {
        const char *recent = NULL;
        const char *user_home = getenv("HOME");
        char home[CED_PATH];
        size_t home_length = user_home && *user_home && realpath(user_home, home) ? strlen(home) : 0;
        for (size_t i = 0; i < e->recent_folders.count; i++) {
            const char *path = e->recent_folders.paths[i];
            const char *label = path;
            char abbreviated[CED_PATH + 1];
            if (home_length && (home[home_length - 1] == '/'
                    ? !strncmp(path, home, home_length) : ced_path_within(path, home))) {
                const char *suffix = path + home_length;
                snprintf(abbreviated, sizeof abbreviated, "~%s%s", *suffix && *suffix != '/' ? "/" : "", suffix);
                label = abbreviated;
            }
            cui_push_id_int(ui, (int)i);
            if (ced_menu_item(ui, label, .icon = CUI_ICON_FOLDER, .disabled = e->git_sync.running)) recent = path;
            cui_pop_id(ui);
        }
        if (e->recent_folders.count) cui_menu_separator(ui);
        bool browse = ced_menu_item(ui, "Open Folder…", .icon = CUI_ICON_FOLDER_OPEN, .disabled = e->git_sync.running);
        cui_menu_end(ui);
        if (recent) open_folder(e, recent);
        else if (browse) dispatch(e, "ced.workspace.open");
    }
}

static void lsp_tick(Editor *e) {
    if (!e->lsp_attempted) {
        for (int i = 0; i < e->count; i++) {
            if (!e->docs[i].file.path[0] || !ced_lsp_language(e->docs[i].file.path)) continue;
            char error[CED_ERROR];
            e->lsp_attempted = true;
            e->lsp = ced_lsp_start(e->workspace.root, getenv("CED_CLANGD"), error, sizeof error);
            if (!e->lsp) notify(e, error);
            break;
        }
    }
    ced_lsp_poll(e->lsp);
    if (ced_lsp_ready(e->lsp)) {
        for (int i = 0; i < e->count; i++) {
            EditorTab *tab = &e->docs[i];
            if (!tab->lsp_synced && tab->file.path[0] && ced_lsp_language(tab->file.path))
                tab->lsp_synced = ced_lsp_sync(e->lsp, tab->file.path, tab->file.text);
        }
    }
    CedLspResult *r = ced_lsp_result(e->lsp);
    if (!r || !r->ready) return;
    r->ready = false;
    if (e->selected < 0 || e->diff) return;
    EditorTab *tab = &e->docs[e->selected];
    if (strcmp(tab->file.path, r->path)) return;
    if (r->kind == CED_LSP_HOVER) {
        if (e->assist.hover_tab == tab->id && e->assist.hover_offset == r->offset && e->assist.hover_requested &&
            !e->palette && !e->close_dialog && !e->commit_dialog && !e->git_discard.open && !r->error[0])
            ced_assist_show_hover(&e->assist, r->hover);
        return;
    }
    if (tab->cursor != r->offset) return;
    if (r->error[0]) { notify(e, r->error); return; }
    if (r->kind == CED_LSP_COMPLETION) {
        if (e->assist.completion_tab != tab->id || e->assist.completion_cursor != tab->cursor ||
            (!e->assist.geometry.focused && !tab->focus)) return;
        ced_assist_show_completions(&e->assist, r);
        if (!r->count) notify(e, "No completions.");
    } else {
        if (!r->target[0]) { notify(e, "No definition found."); return; }
        if (open_document(e, r->target, 0, 0)) e->goto_offset = ced_lsp_offset(e->docs[e->selected].file.text, r->position);
    }
}

static void lsp_request(cui_ctx *ui, Editor *e, CedLspRequest kind) {
    if (e->selected < 0 || e->diff) return;
    EditorTab *tab = &e->docs[e->selected];
    if (!tab->file.path[0] || !ced_lsp_language(tab->file.path)) { notify(e, "LSP requires a saved C or C++ file."); return; }
    ced_assist_reset(&e->assist);
    if (kind == CED_LSP_COMPLETION) {
        e->assist.completion_tab = tab->id; e->assist.completion_cursor = tab->cursor;
    } else if (kind == CED_LSP_HOVER) ced_assist_hover_at_caret(ui, &e->assist, tab->id, tab->cursor);
    tab->focus = true;
    if (!ced_lsp_request(e->lsp, kind, tab->file.path, tab->file.text, tab->cursor))
        notify(e, ced_lsp_status(e->lsp));
}

static void apply_completion(cui_ctx *ui, Editor *e, int index) {
    if (e->selected < 0 || index < 0 || index >= e->assist.count) return;
    EditorTab *tab = &e->docs[e->selected];
    if (tab->id != e->assist.completion_tab || tab->cursor != e->assist.completion_cursor) return;
    CedCompletion *c = &e->assist.completions[index];
    cui_push_id_int(ui, tab->id);
    CedTextEdit edit = {(size_t)c->start, (size_t)c->end, c->text};
    CedEditTransaction transaction;
    char error[CED_ERROR];
    bool applied = ced_document_prepare_edit(&tab->file, tab->file.version, &edit, 1,
        (size_t)tab->anchor, (size_t)tab->cursor, true,
        (size_t)c->start + strlen(c->text), (size_t)c->start + strlen(c->text), &transaction, error);
    if (applied) {
        applied = ced_editor_commit(ui, "source", &tab->file, tab->file.version, &transaction, error);
        ced_edit_transaction_free(&transaction);
    }
    cui_pop_id(ui);
    if (applied) {
        tab->anchor = tab->cursor = e->goto_offset = c->start + (int)strlen(c->text);
        tab->focus = true; tab->lsp_synced = false;
    }
    ced_assist_reset(&e->assist);
}

static void set_terminal_panel(Editor *e, bool visible, const char *tab) {
    bool tab_changed = strcmp(e->terminal_tab, tab) != 0;
    if (!tab_changed && e->terminal_visible == visible) return;
    if (e->terminal_visible != visible) {
        e->terminal_visible = visible;
        if (visible) e->terminal_view.autofocus = true;
        else if (e->selected >= 0) e->docs[e->selected].focus = true;
    }
    if (tab_changed) snprintf(e->terminal_tab, sizeof e->terminal_tab, "%s", tab);
    if (!ced_terminal_state_save(visible, e->terminal_tab)) notify(e, "Could not save editor settings.");
    else e->preferences_reload = true;
}

static void toggle_terminal(Editor *e) {
    set_terminal_panel(e, !e->terminal_visible, e->terminal_tab);
}

static void request_file_action(Editor *e, FileOperation operation, const char *path, bool directory) {
    FileAction *a = &e->file_action;
    a->operation = operation; a->directory = directory;
    if ((operation == CED_FILE_CREATE || operation == CED_DIRECTORY_CREATE) && !directory) {
        if (!ced_parent_path(path, a->path, sizeof a->path)) { notify(e, "Invalid parent folder."); return; }
    } else snprintf(a->path, sizeof a->path, "%s", path);
    snprintf(a->name, sizeof a->name, "%s", operation == CED_FILE_RENAME ? ced_basename(path) : "");
    a->error[0] = 0; a->open = true; a->execute = false; a->serial++;
    ced_assist_reset(&e->assist);
}

static void file_context_menu(cui_ctx *ui, Editor *e, const char *path, bool directory, bool root) {
    cui_context_menu(ui, "file-context") {
        if (ced_menu_item(ui, "New File...", .icon = CUI_ICON_PLUS)) request_file_action(e, CED_FILE_CREATE, path, directory);
        if (ced_menu_item(ui, "New Folder...", .icon = CUI_ICON_FOLDER)) request_file_action(e, CED_DIRECTORY_CREATE, path, directory);
        if (!root) {
            if (ced_menu_item(ui, "Rename...", .icon = CUI_ICON_SQUARE_PEN)) request_file_action(e, CED_FILE_RENAME, path, directory);
            cui_menu_separator(ui);
            if (ced_menu_item(ui, "Delete", .icon = CUI_ICON_TRASH_2, .danger = true)) request_file_action(e, CED_FILE_DELETE, path, directory);
        }
    }
}

static void perform_file_action(Editor *e) {
    FileAction *a = &e->file_action;
    if (!a->execute) return;
    a->execute = false;
    char result[CED_PATH] = {0};
    bool ok = false;
    if (a->operation == CED_FILE_CREATE) {
        ok = ced_file_create(e->workspace.root, a->path, a->name, result, a->error);
    } else if (a->operation == CED_DIRECTORY_CREATE) {
        ok = ced_directory_create(e->workspace.root, a->path, a->name, result, a->error);
    } else if (a->operation == CED_FILE_RENAME) {
#ifdef CED_ENABLE_EXTENSIONS
        unsigned previous_access[MAX_DOCS];
        for (int n = 0; n < e->count; n++) previous_access[n] = ced_extensions_document_access(e->extensions, &e->docs[n].file);
#endif
        char parent[CED_PATH];
        bool fits = ced_parent_path(a->path, parent, sizeof parent) && ced_join(result, sizeof result, parent, a->name);
        for (int i = 0; fits && i < e->count; i++)
            if (ced_path_within(e->docs[i].file.path, a->path))
                fits = strlen(result) + strlen(e->docs[i].file.path + strlen(a->path)) < CED_PATH;
        if (!fits) snprintf(a->error, sizeof a->error, "Path too long for an open file.");
        else ok = ced_file_rename(e->workspace.root, a->path, a->name, result, a->error);
        if (ok) {
            for (int i = 0; i < e->count; i++) {
                EditorTab *tab = &e->docs[i];
                if (!ced_path_within(tab->file.path, a->path)) continue;
                char renamed[CED_PATH];
                snprintf(renamed, sizeof renamed, "%s%s", result, tab->file.path + strlen(a->path));
                ced_lsp_close(e->lsp, tab->file.path);
                snprintf(tab->file.path, sizeof tab->file.path, "%s", renamed);
#ifdef CED_ENABLE_EXTENSIONS
                ced_extensions_document_changed(e->extensions, &tab->file, previous_access[i]);
#endif
                tab->lsp_synced = false; tab_title(tab);
            }
            /* Let the regular refresh preserve expansion under the new names. */
            for (int i = 0; i < e->workspace.count; i++) {
                CedEntry *entry = &e->workspace.entries[i];
                char absolute[CED_PATH];
                if (!ced_join(absolute, sizeof absolute, e->workspace.root, entry->path) || !ced_path_within(absolute, a->path)) continue;
                char renamed[CED_PATH];
                int n = snprintf(renamed, sizeof renamed, "%s%s", result, absolute + strlen(a->path));
                size_t root_length = strlen(e->workspace.root);
                if (strcmp(e->workspace.root, "/")) root_length++;
                if (n >= 0 && (size_t)n < sizeof renamed) {
                    char *copy = strdup(renamed + root_length);
                    if (copy) { free(entry->path); entry->path = copy; }
                }
            }
        }
    } else {
        ok = ced_file_delete(e->workspace.root, a->path, a->error);
        if (ok) for (int i = e->count - 1; i >= 0; i--)
            if (ced_path_within(e->docs[i].file.path, a->path)) remove_tab(e, i);
    }
    /* A recursive delete may partially succeed before a permission error. */
    ced_search_cancel(&e->search);
    e->search.count = 0;
    ced_assist_reset(&e->assist);
    e->diff = false;
    refresh(e);
    if (e->query[0]) ced_search_start(&e->search, &e->workspace, e->query, e->search_include_ignored);
    if (!ok) { a->open = true; notify(e, a->error); return; }
    if (a->operation == CED_FILE_CREATE || a->operation == CED_DIRECTORY_CREATE) {
        for (int i = 0; i < e->workspace.count; i++) {
            char path[CED_PATH];
            CedEntry *entry = &e->workspace.entries[i];
            if (entry->directory && ced_join(path, sizeof path, e->workspace.root, entry->path) && ced_path_within(result, path)) entry->expanded = true;
        }
        if (a->operation == CED_FILE_CREATE) open_document(e, result, 0, 0);
    }
    notify(e, a->operation == CED_FILE_CREATE ? "File created." : a->operation == CED_DIRECTORY_CREATE ? "Folder created." : a->operation == CED_FILE_RENAME ? "Renamed." : "Deleted.");
}

static int tree_node(cui_ctx *ui, Editor *e, int index) {
    CedEntry *head = &e->workspace.entries[index];
    int tail = ced_workspace_compact_directory(&e->workspace, index);
    CedEntry *entry = &e->workspace.entries[tail];
    const char *label = entry->path + (ced_basename(head->path) - head->path);
    bool selected = false;
    char path[CED_PATH];
    if (!ced_join(path, sizeof path, e->workspace.root, entry->path)) return entry->subtree_end;
    if (e->selected >= 0) selected = !strcmp(path, e->docs[e->selected].file.path);
    cui_push_id(ui, head->path);
    bool was_expanded = head->expanded;
    bool expanded = ced_tree_begin(ui, label, &head->expanded,
        &(cui_tree_opts){.leaf = !entry->directory, .selected = selected,
                        .icon = entry->directory ? CUI_ICON_FOLDER : CUI_ICON_FILE_TEXT}, entry->ignored);
    if (head->expanded != was_expanded) {
        for (int i = index + 1; i <= tail; i++) e->workspace.entries[i].expanded = head->expanded;
    }
    if (!entry->directory && cui_tree_clicked(ui)) open_document(e, path, 0, 0);
    file_context_menu(ui, e, path, entry->directory, false);
    if (expanded) {
        for (int next = tail + 1; next < entry->subtree_end;)
            next = tree_node(ui, e, next);
        cui_tree_end(ui);
    }
    cui_pop_id(ui);
    return entry->subtree_end;
}

static void search_panel(cui_ctx *ui, Editor *e) {
    bool changed = cui_search(ui, "project-search", e->query, sizeof e->query, .placeholder = "Search files...");
    bool options_changed = cui_checkbox_ex(ui, "Include Git-ignored files", &e->search_include_ignored, NULL);
    if ((changed || options_changed) && !ced_search_start(&e->search, &e->workspace, e->query, e->search_include_ignored))
        notify(e, "Could not start search.");
    if (e->search.running) {
        cui_secondary(ui, "Searching...");
    } else if (e->query[0]) {
        cui_textf(ui, "%d results%s", e->search.count, e->search.truncated ? " (limit reached)" : "");
        cui_scroll(ui, .id = "search-results", .height = CUI_FILL, .gap = 2) {
            for (int i = 0; i < e->search.count; i++) {
                CedHit *hit = &e->search.hits[i];
                char label[CED_PATH + 30];
                snprintf(label, sizeof label, "%s:%d", hit->path, hit->line);
                cui_push_id_int(ui, i);
                if (ced_list_item(ui, label, .subtitle = hit->preview, .icon = CUI_ICON_SEARCH)) {
                    char path[CED_PATH];
                    if (ced_join(path, sizeof path, e->workspace.root, hit->path)) open_document(e, path, hit->line, hit->column);
                }
                cui_pop_id(ui);
            }
            if (!e->search.count) cui_secondary(ui, "No results.");
        }
    }
}

static void open_diff(Editor *e, CedChange *change, bool staged) {
    char error[CED_ERROR], *before, *after;
    if (!ced_git_versions(&e->git, change, staged, &before, &after, error)) { notify(e, error); return; }
    free(e->before); free(e->after);
    e->before = before; e->after = after;
    e->diff = true; e->diff_staged = staged;
    snprintf(e->diff_path, sizeof e->diff_path, "%s", change->path);
}

static void git_panel(cui_ctx *ui, Editor *e) {
    if (!e->git.available) {
        cui_empty_state(ui, "No Git repository", .icon = CUI_ICON_GIT_BRANCH);
        return;
    }
    cui_push_disabled(ui, e->git_sync.running);
    cui_text(ui, e->git.branch, .font = CUI_FONT_MEDIUM);
    cui_input(ui, "commit-message", e->commit, sizeof e->commit, .placeholder = "Commit message");
    bool staged_any = false, unstaged_any = false;
    for (int i = 0; i < e->git.count; i++) {
        staged_any |= e->git.changes[i].index != ' ' && e->git.changes[i].index != '?';
        unstaged_any |= e->git.changes[i].worktree != ' ';
    }
    if (cui_button(ui, "Commit...", .icon = CUI_ICON_CHECK, .variant = CUI_BUTTON_PRIMARY,
                   .full_width = true, .disabled = !staged_any || !e->commit[0])) e->commit_dialog = true;
    int bulk = -1;
    cui_row(ui, .gap = 4, .width = CUI_FILL) {
        if (cui_button(ui, "Push", .icon = CUI_ICON_UPLOAD, .size = CUI_SIZE_SM, .full_width = true)) start_git_sync(e, false);
        if (cui_button(ui, "Pull", .icon = CUI_ICON_DOWNLOAD, .size = CUI_SIZE_SM, .full_width = true)) start_git_sync(e, true);
    }
    cui_scroll(ui, .id = "git-changes", .height = CUI_FILL, .gap = 2) {
        cui_theme *theme = cui_theme_get(ui);
        float radius = theme->radius_control;
        theme->radius_control = 0;
        if (!e->git.count) cui_secondary(ui, "No changes.");
        bool mutated = false;
        for (int group = 0; group < 2 && !mutated; group++) {
            bool staged = group == 0;
            cui_push_id(ui, staged ? "staged-header" : "changes-header");
            cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, staged ? "STAGED CHANGES" : "CHANGES", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL);
                if (cui_icon_button(ui, staged ? CUI_ICON_MINUS : CUI_ICON_PLUS,
                                    .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                    .tooltip = staged ? "Unstage All" : "Stage All",
                                    .disabled = !(staged ? staged_any : unstaged_any) && !e->git.truncated))
                    bulk = staged ? 1 : 0;
            }
            cui_pop_id(ui);
            if (bulk >= 0) break;
            for (int i = 0; i < e->git.count; i++) {
                CedChange *change = &e->git.changes[i];
                bool staged = group == 0;
                char status = staged ? change->index : change->worktree;
                if (status == ' ' || (staged && status == '?')) continue;
                char label[CED_PATH + 8];
                snprintf(label, sizeof label, "%c  %s", status, change->path);
                cui_push_id_int(ui, group * CED_MAX_CHANGES + i);
                cui_row(ui, .gap = 2) {
                    cui_column(ui, .width = CUI_FILL) {
                        if (ced_nav_item(ui, label, .selected = e->diff && e->diff_staged == staged && !strcmp(e->diff_path, change->path))) open_diff(e, change, staged);
                    }
                    if (!staged && cui_icon_button(ui, CUI_ICON_TRASH_2, .size = CUI_SIZE_SM,
                            .variant = CUI_BUTTON_GHOST, .tooltip = "Discard Changes")) {
                        GitDiscard *action = &e->git_discard;
                        snprintf(action->root,sizeof action->root,"%s",e->git.root);
                        snprintf(action->path,sizeof action->path,"%s",change->path);
                        *action->error = 0; action->open = true;
                    }
                    if (cui_icon_button(ui, staged ? CUI_ICON_MINUS : CUI_ICON_PLUS, .size = CUI_SIZE_SM,
                                        .tooltip = staged ? "Unstage Changes" : "Stage Changes")) {
                        char error[CED_ERROR];
                        if (!ced_git_stage(&e->git, change->path, staged, error)) notify(e, error);
                        else notify(e, staged ? "Changes unstaged." : "Changes staged.");
                        mutated = true;
                    }
                }
                cui_pop_id(ui);
                if (mutated) break;
            }
        }
        if (bulk >= 0) {
            char error[CED_ERROR];
            if (!ced_git_stage_all(&e->git, bulk == 1, error)) notify(e, error);
            else notify(e, bulk == 1 ? "All changes unstaged." : "All changes staged.");
            mutated = true;
        }
        if (mutated) refresh_git(e);
        if (e->git.truncated) cui_secondary(ui, "Showing first 4096 changes.");
        theme->radius_control = radius;
    }
    cui_pop_disabled(ui);
}

static void sidebar(cui_ctx *ui, Editor *e) {
    static const struct { const char *label, *icon; } tabs[] = {
        {"Files", CUI_ICON_FOLDER}, {"Search", CUI_ICON_SEARCH}, {"Git", CUI_ICON_GIT_BRANCH},
#ifdef CED_ENABLE_EXTENSIONS
        {"Extensions", CUI_ICON_PACKAGE},
#endif
    };
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED, .clip = true) {
        int old = e->panel;
#ifdef CED_ENABLE_EXTENSIONS
        if (!ced_extension_tab_find(e->extensions, e->sidebar_tab, NULL, NULL)) *e->sidebar_tab = 0;
#endif
        cui_scroll(ui, .id = "sidebar-tabs", .horizontal = true, .width = CUI_FILL, .height = CUI_PX(32)) {
            cui_row(ui, .width = CUI_FILL, .gap = 0, .pad = 0) {
                cui_theme *theme = cui_theme_get(ui);
                float radius = theme->radius_control;
                theme->radius_control = 0;
                for (int i = 0; i < (int)(sizeof tabs / sizeof *tabs); i++) {
                    bool selected = e->panel == i;
#ifdef CED_ENABLE_EXTENSIONS
                    selected &= !*e->sidebar_tab;
#endif
                    if (cui_icon_button(ui, tabs[i].icon, .tooltip = tabs[i].label, .size = CUI_SIZE_SM, .selected = selected)) {
                        e->panel = i;
#ifdef CED_ENABLE_EXTENSIONS
                        *e->sidebar_tab = 0;
#endif
                    }
                }
#ifdef CED_ENABLE_EXTENSIONS
                ced_extension_tab_buttons(ui, e->extensions, false, e->sidebar_tab);
#endif
                theme->radius_control = radius;
            }
        }
        if (old != e->panel && e->panel == 2) refresh_git(e);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 10) {
#ifdef CED_ENABLE_EXTENSIONS
            if (*e->sidebar_tab) {
                ced_extension_tab_body(ui, e->extensions, e->sidebar_tab);
            } else if (e->panel == 3) {
                if (ced_extensions_panel(ui, e->extensions)) dispatch(e, "ced.extensions.install");
            } else
#endif
            {
                cui_row(ui, .gap = 4) {
                    cui_column(ui, .width = CUI_FILL) {
                        ced_nav_item(ui, ced_basename(e->workspace.root), .icon = CUI_ICON_FOLDER);
                        file_context_menu(ui, e, e->workspace.root, true, true);
                    }
                    if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM, .tooltip = "Refresh Files and Git")) {
                        refresh(e);
                        if (e->panel == 1 && e->query[0])
                            ced_search_start(&e->search, &e->workspace, e->query, e->search_include_ignored);
                    }
                }
                if (e->panel == 0) {
                    cui_scroll(ui, .id = "files", .height = CUI_FILL) {
                        for (int i = 0; i < e->workspace.count;) i = tree_node(ui, e, i);
                        if (!e->workspace.count) cui_secondary(ui, "Empty folder.");
                    }
                    if (e->workspace.truncated) cui_secondary(ui, "Partial listing: limit reached or access denied.");
                } else if (e->panel == 1) search_panel(ui, e);
                else git_panel(ui, e);
            }
        }
    }
}

static int compare_marks(const void *a, const void *b) {
    const cui_text_mark *left = a, *right = b;
    return left->start < right->start ? -1 : left->start > right->start;
}

static void problems_panel(cui_ctx *ui, Editor *e, EditorTab *tab, const CedDiagnostic *diagnostics, int count) {
    if (!e->problems_open || !count) return;
    cui_column(ui, .height = CUI_PX(150), .gap = 2, .pad = 8, .bg = CUI_COLOR_SURFACE_MUTED) {
        cui_row(ui, .gap = 4) {
            cui_text(ui, "Problems", .font = CUI_FONT_MEDIUM, .size = 12, .width = CUI_FILL);
            if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .tooltip = "Hide Problems")) e->problems_open = false;
        }
        cui_scroll(ui, .id = "diagnostics", .height = CUI_FILL) {
            for (int i = 0; i < count; i++) {
                CedLspPosition pos = ced_lsp_position(tab->file.text, diagnostics[i].start);
                char label[560];
                snprintf(label, sizeof label, "%d:%d  %.*s", pos.line + 1, pos.character + 1,
                         (int)strcspn(diagnostics[i].message, "\r\n"), diagnostics[i].message);
                cui_push_id_int(ui, i);
                if (ced_nav_item(ui, label, .icon = diagnostics[i].severity == 1 ? CUI_ICON_TRIANGLE_ALERT : CUI_ICON_INFO)) {
                    e->goto_offset = diagnostics[i].start; tab->focus = true;
                }
                cui_tooltip(ui, diagnostics[i].message);
                cui_pop_id(ui);
            }
        }
    }
}

static void toggle_markdown_preview(Editor *e) {
    if (e->selected < 0 || e->diff || !ced_markdown_file(e->docs[e->selected].file.path)) return;
    EditorTab *tab = &e->docs[e->selected];
    tab->markdown_preview = !tab->markdown_preview;
    tab->focus = !tab->markdown_preview;
    ced_assist_reset(&e->assist);
}

static void markdown_preview(cui_ctx *ui, Editor *e, EditorTab *tab) {
    cui_push_id_int(ui, tab->id);
    cui_scroll(ui, .id = "markdown-preview", .height = CUI_FILL, .pad = 24, .gap = 12) {
        cui_markdown_result result = cui_markdown(ui, "document-markdown", tab->file.text, .size = (float)e->font_size);
        if (result.link) {
            if (!strncmp(result.link, "https://", 8) || !strncmp(result.link, "http://", 7)) cui_app_open_url(result.link);
            else if (result.link[0] != '#' && !strstr(result.link, ":")) {
                char parent[CED_PATH], path[CED_PATH];
                if (ced_parent_path(tab->file.path, parent, sizeof parent) && ced_join(path, sizeof path, parent, result.link) && open_document(e, path, 0, 0))
                    e->docs[e->selected].markdown_preview = ced_markdown_file(path);
            }
        }
    }
    cui_pop_id(ui);
}

static void editor_pane(cui_ctx *ui, Editor *e) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_SURFACE, .clip = true) {
        if (e->count) {
            cui_doc_tab tabs[MAX_DOCS];
            for (int i = 0; i < e->count; i++) tabs[i] = (cui_doc_tab){
                .title = e->docs[i].title, .icon = CUI_ICON_FILE_TEXT,
                .tooltip = e->docs[i].file.path, .dirty = ced_document_dirty(&e->docs[i].file)};
            cui_doc_tabs_result result = cui_doc_tabs(ui, "documents", tabs, e->count, &e->selected, .shortcuts = true);
            int close_action = ced_editor_tabs_close_menu(ui, "documents", tabs, e->count);
            int keep_id = close_action >= 0 ? e->docs[close_action].id : 0;
            if (result.changed) { e->diff = false; e->docs[e->selected].focus = true; ced_assist_reset(&e->assist); }
            if (result.move_from >= 0) {
                EditorTab moved = e->docs[result.move_from];
                if (result.move_from < result.move_to)
                    memmove(e->docs + result.move_from, e->docs + result.move_from + 1, sizeof moved * (size_t)(result.move_to - result.move_from));
                else memmove(e->docs + result.move_to + 1, e->docs + result.move_to, sizeof moved * (size_t)(result.move_from - result.move_to));
                e->docs[result.move_to] = moved;
            }
            if (close_action != CED_TAB_MENU_NONE) request_close_tabs(e, keep_id);
            else if (result.close >= 0) request_close(e, result.close);
        }
        if (e->diff) {
            cui_row(ui, .pad = 8, .gap = 8) {
                cui_text(ui, e->diff_path, .width = CUI_FILL, .nowrap = true);
                if (cui_button(ui, "Edit File", .size = CUI_SIZE_SM)) {
                    char path[CED_PATH];
                    if (ced_join(path, sizeof path, e->git.root, e->diff_path)) open_document(e, path, 0, 0);
                }
                if (cui_icon_button(ui, CUI_ICON_X, .tooltip = "Close Diff")) e->diff = false;
            }
            cui_diff_view(ui, "git-diff", e->before, e->after, .height = -1, .language = ced_language(e->diff_path),
                          .old_title = e->diff_staged ? "HEAD" : "Index", .new_title = e->diff_staged ? "Index" : "Working Tree",
                          .size = e->font_size);
        } else if (e->selected >= 0 && e->selected < e->count) {
            EditorTab *tab = &e->docs[e->selected];
            bool assist_enabled = !e->close_dialog && !e->commit_dialog && !e->git_discard.open && !e->file_action.open && !e->palette && !extension_modal(e) && ced_lsp_language(tab->file.path);
            if (!assist_enabled) ced_assist_reset(&e->assist);
            int accepted = assist_enabled ? ced_assist_keys(ui, &e->assist) : -1;
            if (accepted >= 0) apply_completion(ui, e, accepted);
            if (ced_markdown_file(tab->file.path)) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_END) {
                    if (cui_button(ui, tab->markdown_preview ? "Edit" : "Preview",
                            .icon = tab->markdown_preview ? CUI_ICON_CODE : CUI_ICON_EYE, .size = CUI_SIZE_SM)) dispatch(e,"ced.markdown.toggle");
                }
            }
            if (tab->markdown_preview && ced_markdown_file(tab->file.path)) {
                ced_assist_reset(&e->assist);
                markdown_preview(ui, e, tab);
            } else {
                cui_push_id_int(ui, tab->id);
                bool complete = assist_enabled && ced_editor_completion_key(ui, "source");
                if (tab->focus && e->goto_line <= 0 && e->goto_offset < 0)
                    cui_code_editor_select(ui, "source", tab->cursor, tab->cursor);
                if (e->goto_line > 0) {
                    cui_code_editor_goto(ui, "source", e->goto_line, e->goto_column);
                    e->goto_line = 0;
                }
                if (e->goto_offset >= 0) {
                    cui_code_editor_select(ui, "source", e->goto_offset, e->goto_offset);
                    e->goto_offset = -1;
                }
                int diagnostic_count = 0;
                const CedDiagnostic *diagnostics = ced_lsp_diagnostics(e->lsp, tab->file.path, &diagnostic_count);
                cui_text_mark marks[CED_LSP_DIAGNOSTICS];
                for (int i = 0; i < diagnostic_count; i++) marks[i] = (cui_text_mark){diagnostics[i].start, diagnostics[i].end, diagnostics[i].severity == 1 ? 4 : 3};
                qsort(marks, (size_t)diagnostic_count, sizeof *marks, compare_marks);
                e->caret = ced_editor_render(ui, "source", &tab->file, &(cui_code_editor_opts){
                    .lang = ced_language(tab->file.path), .height = -1, .size = e->font_size, .tab_size = 4, .autofocus = tab->focus,
                    .marks = marks, .mark_count = diagnostic_count});
                tab->cursor = e->caret.cursor;
                tab->anchor = e->caret.anchor;
                if (e->caret.changed) tab->lsp_synced = false;
                tab->focus = false;
                if (e->caret.refused) notify(e, "Edit exceeds the 2 MiB file limit.");
                CedEditorGeometry geometry = ced_editor_geometry(ui, "source", tab->file.text, (float)e->font_size);
                cui_pop_id(ui);
                CedLspResult *pending = ced_lsp_result(e->lsp);
                bool completing = e->assist.completion_open || (ced_lsp_pending(e->lsp) && pending &&
                    pending->kind == CED_LSP_COMPLETION && e->assist.completion_tab == tab->id);
                if (e->caret.changed && completing && tab->cursor > 0) {
                    unsigned char ch = (unsigned char)tab->file.text[tab->cursor - 1];
                    complete |= (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch >= 128;
                }
                int hover = ced_assist_hover(ui, &e->assist, tab->id, geometry, e->caret.changed,
                    assist_enabled && ced_lsp_ready(e->lsp) && !ced_lsp_pending(e->lsp) && !complete);
                if (!assist_enabled) ced_assist_reset(&e->assist);
                if (complete) lsp_request(ui, e, CED_LSP_COMPLETION);
                else if (hover >= 0) ced_lsp_request(e->lsp, CED_LSP_HOVER, tab->file.path, tab->file.text, hover);
                if (!e->caret.changed) problems_panel(ui, e, tab, diagnostics, diagnostic_count);
            }
        } else {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_image(ui, e->logo, .width = CUI_PX(192), .height = CUI_PX(192));
            }
        }
    }
}

static void set_terminal_position(Editor *e, bool right) {
    if (e->terminal_right == right) return;
    e->terminal_right = right;
    if (!ced_terminal_right_save(right)) notify(e, "Could not save editor settings.");
    else e->preferences_reload=true;
}

static void terminal_panel(cui_ctx *ui, Editor *e) {
    bool shell = true;
#ifdef CED_ENABLE_EXTENSIONS
    char selected[CED_TERMINAL_TAB_CAPACITY] = "";
    const CedExtensionTab *tab = ced_extension_tab_find(e->extensions, e->terminal_tab, NULL, NULL);
    if (tab && tab->terminal) snprintf(selected, sizeof selected, "%s", e->terminal_tab);
    else if (*e->terminal_tab && e->extensions &&
             (e->extensions->reload || (e->extensions->job.running && e->extensions->job.kind == EXT_LOAD))) {
        cui_secondary(ui, "Loading extensions…");
        return;
    }
    shell = !*selected;
#endif
    if (shell && !e->terminal) {
        char error[CED_ERROR];
        e->terminal = ced_terminal_start(e->workspace.root, getenv("SHELL"), error, sizeof error);
        if (!e->terminal) { notify(e, error); e->terminal_visible = false; return; }
        e->terminal_view.autofocus = true;
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED, .clip = true) {
        cui_row(ui, .width = CUI_FILL, .pad_x = 8, .pad_y = 4, .gap = 6, .align = CUI_ALIGN_CENTER, .wrap = true) {
            cui_column(ui, .width = CUI_FILL, .min_width = 120) {
                cui_scroll(ui, .id = "terminal-tabs", .horizontal = true, .width = CUI_FILL, .height = CUI_PX(32)) {
                    cui_row(ui, .width = CUI_FIT, .gap = 0) {
                        cui_theme *theme = cui_theme_get(ui);
                        float radius = theme->radius_control;
                        theme->radius_control = 0;
                        if (cui_button(ui, "Shell", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST, .selected = shell)) {
#ifdef CED_ENABLE_EXTENSIONS
                            selected[0] = 0;
                            set_terminal_panel(e, true, selected);
#endif
                            e->terminal_view.autofocus = true;
                        }
#ifdef CED_ENABLE_EXTENSIONS
                        if (ced_extension_tab_buttons(ui, e->extensions, true, selected))
                            set_terminal_panel(e, true, selected);
                        shell = !*selected;
#endif
                        theme->radius_control = radius;
                    }
                }
            }
            cui_row(ui, .width = CUI_FIT, .gap = 6, .align = CUI_ALIGN_CENTER) {
                bool right = e->terminal_right;
                if (ced_aux_panel_button(ui, "move-auxiliary-panel", right, false,
                        right ? "Move Auxiliary Panel to Bottom" : "Move Auxiliary Panel Right")) {
                    set_terminal_position(e, !right);
                    cui_request_redraw(ui);
                }
                if (shell && cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM, .tooltip = "Restart Terminal")) {
                    ced_terminal_stop(e->terminal); e->terminal = NULL;
                }
            }
        }
#ifdef CED_ENABLE_EXTENSIONS
        if (!shell) {
            e->terminal_view.focused = false;
            ced_extension_tab_body(ui, e->extensions, selected);
        }
#endif
        if (shell && e->terminal) {
            if (!e->terminal->alive) cui_secondary(ui, "Shell exited.");
            ced_terminal_view(ui, e->terminal, &e->terminal_view, (float)e->font_size);
        }
    }
}

static void document_area(cui_ctx *ui, Editor *e) {
    if (!e->terminal_visible) { e->terminal_view.focused = false; editor_pane(ui, e); return; }
    bool right = e->terminal_right;
    float *ratio = right ? &e->terminal_right_split : &e->terminal_bottom_split;
    cui_split(ui, right ? "terminal-right-split" : "terminal-bottom-split", ratio,
              .vertical = !right, .min_first = right ? 220 : 140, .min_second = right ? 260 : 140, .height = CUI_FILL) {
        editor_pane(ui, e);
        cui_split_next(ui);
        terminal_panel(ui, e);
    }
}

static void dialogs(cui_ctx *ui, Editor *e) {
    GitDiscard *discard = &e->git_discard;
    cui_dialog(ui, "Discard Changes?", &discard->open, .width = 520) {
        cui_text(ui, discard->path, .size = 13, .width = CUI_FILL);
        cui_text(ui, "Discard unstaged changes to this file? New untracked files will be deleted. Staged changes will be kept.", .size = 13);
        char path[CED_PATH];
        if (ced_join(path,sizeof path,discard->root,discard->path)) {
            for (int n = 0; n < e->count; n++)
                if (!strcmp(e->docs[n].file.path,path) && ced_document_dirty(&e->docs[n].file)) {
                    cui_text(ui, "Unsaved edits in the open editor will also be discarded.", .size = 13);
                    break;
                }
        }
        if (*discard->error) cui_text(ui, discard->error, .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_DEFAULT)) discard->open = false;
            if (cui_button(ui, "Discard Changes", .variant = CUI_BUTTON_DANGER, .disabled = e->git_sync.running)) {
                discard->open = false; discard->execute = true;
            }
        }
    }
    bool close_multiple = e->close_index < 0;
    int keep_id = close_multiple ? e->close_keep_id : 0;
    if (e->close_dialog && keep_id) {
        bool found = false;
        for (int i = 0; i < e->count; i++)
            if (e->docs[i].id == keep_id) found = true;
        if (!found) e->close_dialog = false;
    }
    const char *close_title = keep_id ? "Close Other Tabs?" : close_multiple ? "Close All Tabs?" : "Save Changes?";
    cui_dialog(ui, close_title, &e->close_dialog,
        .description = "Unsaved changes will be lost if discarded.", .width = 480) {
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_DEFAULT)) e->close_dialog = false;
            if (cui_button(ui, close_multiple ? "Discard All" : "Discard", .variant = CUI_BUTTON_DANGER)) {
                if (close_multiple) close_tabs_except(e, keep_id);
                else remove_tab(e, e->close_index);
                e->close_dialog = false;
            }
            if (cui_button(ui, close_multiple ? "Save All" : "Save", .variant = CUI_BUTTON_PRIMARY)) {
                if (close_multiple) {
                    bool saved = true;
                    for (int i = 0; i < e->count && saved; i++) {
                        if (e->docs[i].id != keep_id && ced_document_dirty(&e->docs[i].file))
                            saved = save_document(e, i, false);
                    }
                    if (saved) { close_tabs_except(e, keep_id); e->close_dialog = false; }
                } else if (save_document(e, e->close_index, false)) {
                    remove_tab(e, e->close_index);
                    e->close_dialog = false;
                }
            }
        }
    }
    cui_dialog(ui, "Create Commit?", &e->commit_dialog, .width = 480) {
        cui_text(ui, e->commit, .size = 14);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_DEFAULT)) e->commit_dialog = false;
            if (cui_button(ui, "Create Commit", .variant = CUI_BUTTON_PRIMARY)) {
                char error[CED_ERROR];
                if (ced_git_commit(&e->git, e->commit, error)) { notify(e, "Commit created."); e->commit[0] = 0; }
                else notify(e, error);
                refresh_git(e);
                e->commit_dialog = false;
            }
        }
    }
    FileAction *a = &e->file_action;
    const char *title = a->operation == CED_FILE_CREATE ? "New File" : a->operation == CED_DIRECTORY_CREATE ? "New Folder" : a->operation == CED_FILE_RENAME ? "Rename" : "Delete?";
    cui_dialog(ui, title, &a->open, .width = 500) {
        cui_text(ui, a->path, .size = 12, .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL);
        bool submitted = false;
        if (a->operation != CED_FILE_DELETE) {
            cui_push_id_int(ui, a->serial);
            submitted = (cui_input(ui, "file-name", a->name, sizeof a->name, .placeholder = a->operation == CED_DIRECTORY_CREATE ? "Folder name" : "File name", .autofocus = true,
                .error = a->error[0] ? a->error : NULL) & CUI_SUBMITTED) != 0;
            cui_pop_id(ui);
        } else {
            cui_text(ui, a->directory ? "Delete this folder and all its contents? This cannot be undone." : "Delete this file? This cannot be undone.", .size = 13);
            int unsaved = 0;
            for (int i = 0; i < e->count; i++)
                if (ced_path_within(e->docs[i].file.path, a->path) && ced_document_dirty(&e->docs[i].file)) unsaved++;
            if (unsaved) cui_textf(ui, "Unsaved changes in %d open file%s will be discarded.", unsaved, unsaved == 1 ? "" : "s");
            if (a->error[0]) cui_text(ui, a->error, .color = CUI_COLOR_TEXT_MUTED, .size = 12);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancel", .variant = CUI_BUTTON_DEFAULT)) a->open = false;
            bool clicked = cui_button(ui, a->operation == CED_FILE_RENAME ? "Rename" : a->operation == CED_FILE_DELETE ? "Delete" : "Create",
                .variant = a->operation == CED_FILE_DELETE ? CUI_BUTTON_DANGER : CUI_BUTTON_PRIMARY,
                .disabled = a->operation != CED_FILE_DELETE && !a->name[0]);
            if (clicked || (submitted && a->name[0])) { a->execute = true; a->open = false; }
        }
    }
}

static CedCommandContext editor_context(void *user) {
    Editor *e = user;
    EditorTab *tab = e->selected >= 0 && e->selected < e->count ? &e->docs[e->selected] : NULL;
    bool has_document = tab && !e->diff && !tab->markdown_preview;
    uint64_t document = has_document ? tab->file.id : 0;
    size_t anchor = has_document ? (size_t)tab->anchor : 0, cursor = has_document ? (size_t)tab->cursor : 0;
    if (!e->selection_version || e->selection_document != document || e->selection_anchor != anchor || e->selection_cursor != cursor) {
        e->selection_version++; e->selection_document = document;
        e->selection_anchor = anchor; e->selection_cursor = cursor;
    }
    bool trusted = false;
#ifdef CED_ENABLE_EXTENSIONS
    trusted = e->extensions && e->extensions->trusted;
#endif
    const char *language = has_document ? ced_language(tab->file.path) : NULL;
    return (CedCommandContext){document, has_document ? tab->file.version : 0, e->selection_version,
        anchor, cursor, has_document, has_document && !e->terminal_view.focused && !extension_modal(e), trusted, language ? language : "text"};
}

#ifdef CED_ENABLE_EXTENSIONS
static bool extension_document(void *user, size_t index, CedExtensionDocument *out) {
    Editor *e = user;
    if (index >= (size_t)e->count) return false;
    EditorTab *tab = &e->docs[index];
    const char *language = ced_language(tab->file.path);
    *out = (CedExtensionDocument){&tab->file, language ? language : "text", (size_t)tab->anchor, (size_t)tab->cursor};
    return true;
}

static bool extension_commit(void *user, CedDocument *document, uint64_t version,
                             const CedEditTransaction *transaction, char *error) {
    Editor *e = user;
    for (int n = 0; n < e->count; n++) if (&e->docs[n].file == document) {
        EditorTab *tab = &e->docs[n]; cui_ctx *ui = cui_app_ui(e->app);
        cui_push_id_int(ui, tab->id);
        bool ok = ced_editor_commit(ui, "source", document, version, transaction, error);
        cui_pop_id(ui);
        if (ok && transaction->changed) {
            tab->anchor = (int)transaction->anchor; tab->cursor = (int)transaction->cursor;
            tab->lsp_synced = false; ced_assist_reset(&e->assist); editor_context(e);
        }
        return ok;
    }
    snprintf(error, CED_ERROR, "Unavailable"); return false;
}
static void extension_message(void *user, const char *message) { notify(user, message); }
static void extension_open_log(void *user, const char *path) { open_document(user, path, 0, 0); }
#endif

static const struct { const char *id, *title, *category, *shortcut; } builtin_commands[] = {
    {"ced.file.new", "New File", "File", "Cmd/Ctrl N"},
    {"ced.file.open", "Open File", "File", "Cmd/Ctrl O"},
    {"ced.workspace.open", "Open Folder", "File", "Cmd/Ctrl Shift O"},
    {"ced.file.save", "Save", "File", "Cmd/Ctrl S"},
    {"ced.file.saveAs", "Save As", "File", "Cmd/Ctrl Shift S"},
    {"ced.workspace.refresh", "Refresh Files and Git", "Project", ""},
    {"ced.theme.toggle", "Toggle Theme", "Appearance", ""},
    {"ced.language.complete", "Complete", "Language", "Ctrl Space"},
    {"ced.language.definition", "Go to Definition", "Language", "F12"},
    {"ced.language.hover", "Symbol Information", "Language", "Cmd/Ctrl Shift K"},
    {"ced.language.restart", "Restart Language Server", "Language", ""},
    {"ced.terminal.toggle", "Toggle Terminal", "Terminal", "Cmd/Ctrl J"},
    {"ced.terminal.right", "Move Terminal Right", "Terminal", ""},
    {"ced.terminal.bottom", "Move Terminal to Bottom", "Terminal", ""},
    {"ced.markdown.toggle", "Toggle Markdown Preview", "View", "Cmd/Ctrl Shift V"},
#ifdef CED_ENABLE_EXTENSIONS
    {"ced.extensions.install", "Install Extension from Folder", "Extensions", ""},
    {"ced.extensions.manage", "Manage Extensions", "Extensions", ""},
#endif
    {"ced.preferences.open", "Open Preferences (JSON)", "Preferences", "Cmd/Ctrl ,"},
    {"ced.preferences.reload", "Reload Preferences", "Preferences", ""},
    {"ced.workspace.openRecent", "Open Recent Folder", "File", ""},
};

static bool builtin_execute(void *user, const char *id, const CedCommandContext *context, char *error) {
    (void)context;
    Editor *e = user; cui_ctx *ui = cui_app_ui(e->app);
    if (!strcmp(id,"ced.preferences.open")) {
        char path[CED_PATH];
        if (!ced_preferences_ensure(path,error)) return false;
        return open_document(e,path,0,0);
    }
    if (!strcmp(id,"ced.preferences.reload")) { e->preferences_reload=true; return true; }
    if (!strcmp(id, "ced.workspace.openRecent")) {
        e->open_menu_requested = true;
        cui_request_wakeup(ui, 0);
        return true;
    }
    size_t action = 0;
    while (action < sizeof builtin_commands / sizeof *builtin_commands && strcmp(builtin_commands[action].id, id)) action++;
    switch (action) {
        case 0: new_document(e); break;
        case 1: choose_file(e); break;
        case 2: choose_folder(e); break;
        case 3: save_document(e, e->selected, false); break;
        case 4: save_document(e, e->selected, true); break;
        case 5: refresh(e); break;
        case 6: cui_set_theme(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_THEME_LIGHT : CUI_THEME_DARK); break;
        case 7: lsp_request(ui, e, CED_LSP_COMPLETION); break;
        case 8: lsp_request(ui, e, CED_LSP_DEFINITION); break;
        case 9: lsp_request(ui, e, CED_LSP_HOVER); break;
        case 10:
            ced_lsp_stop(e->lsp); e->lsp = NULL; e->lsp_attempted = false;
            ced_assist_reset(&e->assist);
            for (int n = 0; n < e->count; n++) e->docs[n].lsp_synced = false;
            break;
        case 11: toggle_terminal(e); break;
        case 12: set_terminal_position(e, true); if (!e->terminal_visible) toggle_terminal(e); break;
        case 13: set_terminal_position(e, false); if (!e->terminal_visible) toggle_terminal(e); break;
        case 14: toggle_markdown_preview(e); break;
#ifdef CED_ENABLE_EXTENSIONS
        case 15: {
            char source[CED_PATH];
            if (!e->extensions) goto disabled;
            if (cui_app_file_dialog(e->app, &(cui_file_dialog_opts){.kind=CUI_FILE_FOLDER,.title="Install Extension from Folder"}, source, sizeof source))
                return ced_extensions_job(e->extensions, EXT_INSPECT, source);
            break;
        }
        case 16:
            if (!e->extensions) goto disabled;
            e->extensions->manage_open = true; break;
#endif
        default: snprintf(error, CED_ERROR, "Unknown command"); return false;
    }
    return true;
#ifdef CED_ENABLE_EXTENSIONS
disabled:
    snprintf(error, CED_ERROR, "Extensions are disabled."); return false;
#endif
}

static void register_commands(Editor *e) {
    for (size_t n = 0; n < sizeof builtin_commands / sizeof *builtin_commands; n++) {
        char error[CED_ERROR];
        if (!ced_commands_add(&e->commands, builtin_commands[n].id, builtin_commands[n].title,
            builtin_commands[n].category, builtin_commands[n].shortcut, (CedCommandCondition){0},
            e, builtin_execute, e, error)) notify(e, error);
    }
}

static void dispatch(Editor *e, const char *id) {
    char error[CED_ERROR] = {0}; CedCommandContext context = editor_context(e);
    if (!ced_commands_execute(&e->commands, id, &context, error)) notify(e, error);
}

static void frame(cui_ctx *ui, Editor *e) {
    poll_git_sync(ui, e);
    double now = cui_time(ui);
    if (now >= e->next_memory_sample) {
        e->memory_usage = ced_memory_usage_bytes();
        e->next_memory_sample = now + 1;
    }
    cui_request_wakeup(ui, (float)(e->next_memory_sample - now));
    /* Tree paths and document buffers from the previous frame are no longer
     * needed by the renderer here, so filesystem actions can refresh them. */
    perform_file_action(e);
    perform_git_discard(e);
    poll_preferences(ui,e);
    for (int n = 0; n < e->count; n++) {
        cui_push_id_int(ui, e->docs[n].id); ced_editor_touch(ui, "source"); cui_pop_id(ui);
    }
    editor_context(e);
#ifdef CED_ENABLE_EXTENSIONS
    ced_extensions_tick(e->extensions);
    if (e->extensions && *e->extensions->reveal_tab) {
        const CedExtensionTab *tab = ced_extension_tab_find(e->extensions, e->extensions->reveal_tab, NULL, NULL);
        if (tab && tab->terminal) {
            set_terminal_panel(e, true, tab->id);
        } else if (tab) {
            snprintf(e->sidebar_tab, sizeof e->sidebar_tab, "%s", tab->id);
            e->sidebar = true;
        }
        *e->extensions->reveal_tab = 0;
    }
    if (e->extensions) cui_request_wakeup(ui, .03f);
#endif
    /* Release diff sources only after the previous frame has finished painting. */
    if (!e->diff) {
        free(e->before); free(e->after);
        e->before = e->after = NULL;
    }
    if (e->selected < 0 || e->diff || e->close_dialog || e->commit_dialog || e->git_discard.open || e->file_action.open || e->palette) ced_assist_reset(&e->assist);
    lsp_tick(e);
    if (ced_lsp_running(e->lsp)) cui_request_wakeup(ui, 0.05f);
    if (e->terminal) {
        ced_terminal_poll(e->terminal);
        if (e->terminal->alive) cui_request_wakeup(ui, e->terminal_visible ? 0.03f : 0.1f);
    }
    if (e->search.running && ced_search_done(&e->search)) ced_search_finish(&e->search);
    if (e->search.running) cui_request_wakeup(ui, 0.05f);
    if (!e->close_dialog && !e->commit_dialog && !e->git_discard.open && !e->file_action.open && !e->palette && !extension_modal(e)) {
        int previous_font_size = e->font_size;
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_MINUS) && e->font_size > CED_FONT_SIZE_MIN) e->font_size--;
        if ((cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_EQUAL) ||
             cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_EQUAL)) && e->font_size < CED_FONT_SIZE_MAX) e->font_size++;
        if (e->font_size != previous_font_size && !ced_font_size_save(e->font_size))
            notify(e, "Could not save editor settings.");
        else if (e->font_size != previous_font_size) e->preferences_reload=true;
        if (cui_shortcut(ui,CUI_MOD_PRIMARY,CUI_KEY_COMMA)) dispatch(e,"ced.preferences.open");
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_N)) dispatch(e, "ced.file.new");
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O)) dispatch(e, "ced.file.open");
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_O)) dispatch(e, "ced.workspace.open");
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S)) dispatch(e, "ced.file.save");
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_S)) dispatch(e, "ced.file.saveAs");
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_B)) e->sidebar = !e->sidebar;
#ifdef CED_ENABLE_EXTENSIONS
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_E) ||
            cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_F) ||
            cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_G))
            *e->sidebar_tab = 0;
#endif
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_E)) { e->panel = 0; e->sidebar = true; }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_F)) { e->panel = 1; e->sidebar = true; }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_G)) { e->panel = 2; e->sidebar = true; refresh_git(e); }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_P)) e->palette = true;
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_J)) dispatch(e, "ced.terminal.toggle");
        if (!e->terminal_view.focused) {
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_V)) dispatch(e, "ced.markdown.toggle");
            if (cui_shortcut(ui, 0, CUI_KEY_F12)) dispatch(e, "ced.language.definition");
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_K)) dispatch(e, "ced.language.hover");
        }
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
#ifdef __APPLE__
        cui_rect toolbar = cui_get_rect(ui, cui_make_id(ui, "main-toolbar"));
        if (toolbar.h == 0) cui_request_redraw(ui);
        float inset = ced_macos_layout_titlebar(e->app, toolbar);
#endif
        cui_toolbar(ui, "main-toolbar", .no_border = false) {
#ifdef __APPLE__
            if (inset > 0) cui_gap(ui, inset);
#endif
            cui_toolbar_button(ui, CUI_ICON_PANEL_LEFT, .tooltip = "Toggle Sidebar", .toggle = &e->sidebar);
            open_folder_menu(ui, e);
            if (cui_toolbar_button(ui, CUI_ICON_COMMAND, .tooltip = "Command Palette", .shortcut = "Cmd/Ctrl Shift P")) {
                ced_assist_reset(&e->assist);
                e->palette = true;
            }
#ifdef __APPLE__
            cui_box(ui, .id = "titlebar-drag", .width = CUI_FILL, .height = CUI_PX(cui_theme_get(ui)->control_sm)) {}
#else
            cui_toolbar_spacer(ui);
#endif
            cui_row(ui, .width = CUI_FIT, .pad_x = 6) {
                if (ced_aux_panel_button(ui, "auxiliary-panel", e->terminal_right, e->terminal_visible,
                        e->terminal_visible ? "Hide Auxiliary Panel" : "Show Auxiliary Panel"))
                    dispatch(e, "ced.terminal.toggle");
            }
            cui_theme_toggle(ui);
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL) {
            if (e->sidebar) {
                cui_split(ui, "workspace-split", &e->split, .min_first = 290, .min_second = 380, .height = CUI_FILL) {
                    sidebar(ui, e);
                    cui_split_next(ui);
                    document_area(ui, e);
                }
            } else document_area(ui, e);
        }
        cui_status_bar(ui, "status", .no_border = false) {
            cui_status_bar_text(ui, e->git.available ? e->git.branch : "No Git", .icon = CUI_ICON_GIT_BRANCH);
            cui_status_bar_text(ui, e->message, .tone = CUI_TONE_NEUTRAL);
            cui_status_bar_spacer(ui);
            if (e->lsp_attempted) cui_status_bar_text(ui, ced_lsp_status(e->lsp), .tone = CUI_TONE_NEUTRAL);
            if (!e->diff && e->selected >= 0) {
                int count;
                ced_lsp_diagnostics(e->lsp, e->docs[e->selected].file.path, &count);
                char problems[40]; snprintf(problems, sizeof problems, "Problems: %d", count);
                if (ced_lsp_language(e->docs[e->selected].file.path) && cui_status_bar_item(ui, problems, .icon = CUI_ICON_TRIANGLE_ALERT)) e->problems_open = !e->problems_open;
                if (!e->docs[e->selected].markdown_preview) {
                    char position[64];
                    snprintf(position, sizeof position, "Ln %d, Col %d", e->caret.line, e->caret.column);
                    cui_status_bar_text(ui, position, .tone = CUI_TONE_NEUTRAL);
                    cui_status_bar_text(ui, e->docs[e->selected].file.crlf ? "CRLF" : "LF", .tone = CUI_TONE_NEUTRAL);
                }
                cui_status_bar_text(ui, ced_language(e->docs[e->selected].file.path), .tone = CUI_TONE_NEUTRAL);
            }
            char memory[48];
            if (e->memory_usage)
                snprintf(memory, sizeof memory, "RAM: %.1f MB", (double)e->memory_usage / 1000000.0);
            else snprintf(memory, sizeof memory, "RAM: —");
            cui_status_bar_text(ui, memory, .tone = CUI_TONE_NEUTRAL);
        }
    }
    dialogs(ui, e);
    int completion = ced_assist_draw(ui, &e->assist);
    if (completion >= 0) apply_completion(ui, e, completion);
    CedCommandContext context = editor_context(e);
    int command_count = 0;
    for (size_t n = 0; n < e->commands.count && command_count < 1024; n++) {
        const CedCommandEntry *command = &e->commands.entries[n];
        if (!ced_command_condition(&command->when, &context)) continue;
        e->palette_items[command_count] = (cui_command){command->title, CUI_ICON_CODE, command->category, command->shortcut};
        snprintf(e->palette_ids[command_count], sizeof e->palette_ids[command_count], "%s", command->id);
        command_count++;
    }
    int chosen = ced_command_palette(ui, &e->palette, e->palette_items, command_count);
    if (chosen >= 0 && chosen < command_count) dispatch(e, e->palette_ids[chosen]);
#ifdef CED_ENABLE_EXTENSIONS
    ced_extensions_ui(ui, e->extensions);
#endif
}

static void editor_free(Editor *e) {
#ifdef CED_ENABLE_EXTENSIONS
    ced_extensions_destroy(e->extensions);
#endif
    ced_commands_free(&e->commands);
    if (e->git_sync.running) pthread_join(e->git_sync.thread, NULL);
    pthread_mutex_destroy(&e->git_sync.mutex);
    ced_search_cancel(&e->search);
    ced_lsp_stop(e->lsp);
    ced_terminal_stop(e->terminal);
    pthread_mutex_destroy(&e->search.mutex);
    for (int i = 0; i < e->count; i++) ced_document_free(&e->docs[i].file);
    ced_workspace_free(&e->workspace);
    ced_git_free(&e->git);
    free(e->before); free(e->after);
    if (e->app) {
        if (e->logo) cui_image_destroy(cui_app_ui(e->app), e->logo);
        cui_app_destroy(e->app);
    }
    ced_editor_font_free(&e->editor_font);
}

static int ced_main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        puts("Usage: ced [--disable-extensions] [folder] [file ...]");
        return 0;
    }
    bool disable_extensions = false;
    int first = 1;
    if (argc > first && !strcmp(argv[first], "--disable-extensions")) { disable_extensions = true; first++; }
    static Editor e;
    e.selected = -1;
    e.sidebar = true;
    e.split = 0.27f;
    ced_preferences_defaults(&e.preferences);
    e.font_size=e.preferences.font_size;
    e.goto_offset = -1; e.problems_open = true;
    e.terminal_bottom_split = 0.65f; e.terminal_right_split = 0.6f;
    pthread_mutex_init(&e.search.mutex, NULL);
    pthread_mutex_init(&e.git_sync.mutex, NULL);
    char error[CED_ERROR];
    const char *root = argc > first ? argv[first] : ".";
    if (!ced_workspace_open(&e.workspace, root, error)) { fprintf(stderr, "%s\n", error); editor_free(&e); return 1; }
    refresh_git(&e);
    recovery_path(&e);
    restore_recovery(&e);
    for (int i = first + 1; i < argc; i++) {
        char path[CED_PATH];
        bool absolute=argv[i][0]=='/';
#ifdef _WIN32
        absolute |= strlen(argv[i])>2 && argv[i][1]==':';
#endif
        if (absolute) open_document(&e, argv[i], 0, 0);
        else if (ced_join(path, sizeof path, e.workspace.root, argv[i])) open_document(&e, path, 0, 0);
    }
    e.app = cui_app_create(&(cui_app_desc){.title = "CED · Editor", .width = 1280, .height = 820,
        .min_width = 760, .min_height = 480, .theme = CUI_THEME_DARK, .accent = CUI_ACCENT_BLUE,
        .app_id = "dev.ced.Editor"});
    if (!e.app) { fprintf(stderr, "Could not create window.\n"); editor_free(&e); return 1; }
    e.logo = ced_editor_logo_create(cui_app_ui(e.app));
    e.preferences_reload=true;
    poll_preferences(cui_app_ui(e.app),&e);
    if (!ced_recent_folder_record(e.workspace.root)) notify(&e, "Could not save recent folders.");
    register_commands(&e);
#ifdef CED_ENABLE_EXTENSIONS
    const char *home = getenv("HOME"); char profile[CED_PATH];
    if (!disable_extensions && home && ced_join(profile, sizeof profile, home, ".ced"))
        e.extensions = ced_extensions_create(&e.commands,
            (CedExtensionHost){&e, editor_context, extension_document, extension_commit, extension_message, extension_open_log}, profile, e.workspace.root);
#else
    (void)disable_extensions;
#endif
#ifdef __APPLE__
    ced_macos_set_icon();
    ced_macos_titlebar(e.app);
    cui_ctx *ui = cui_app_ui(e.app);
    cui_push_id(ui, "main-toolbar");
    cui_id drag_region = cui_make_id(ui, "titlebar-drag");
    cui_pop_id(ui);
    void *window_monitor = ced_macos_window_events(e.app, drag_region);
#endif
    while (cui_app_frame(e.app)) frame(cui_app_ui(e.app), &e);
#ifdef __APPLE__
    ced_macos_remove_window_events(window_monitor);
#endif
    if (!write_recovery(&e)) {
        fprintf(stderr, "Could not write recovery file: %s\n", e.recovery);
        /* Last-resort copies in the workspace, without replacing user files. */
        for (int i = 0; i < e.count; i++) {
            if (!ced_document_dirty(&e.docs[i].file)) continue;
            char path[CED_PATH];
            if (!ced_join(path, sizeof path, e.workspace.root, ".ced-recovery-XXXXXX")) continue;
            int fd = mkstemp(path);
            if (fd >= 0) {
                FILE *f = fdopen(fd, "wb");
                if (f) { fputs(e.docs[i].file.text, f); fclose(f); fprintf(stderr, "Recovery: %s\n", path); }
                else close(fd);
            }
        }
    }
    editor_free(&e);
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    ced_windows_init();
    int count=0; wchar_t **wide=CommandLineToArgvW(GetCommandLineW(),&count);
    if (!wide) return 1;
    char **utf8=calloc((size_t)count+1,sizeof *utf8);
    if (!utf8) { LocalFree(wide); return 1; }
    bool valid=true;
    for (int i=0;i<count;i++) { utf8[i]=ced_windows_utf8(wide[i]); if (!utf8[i]) valid=false; }
    LocalFree(wide);
    int result=valid ? ced_main(count,utf8) : 1;
    for (int i=0;i<count;i++) free(utf8[i]);
    free(utf8); return result;
#else
    return ced_main(argc,argv);
#endif
}
