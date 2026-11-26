#include "workspace.h"
#include "output.h"

#ifdef _WIN32
#include "windows_directory.h"
#include "windows_process.h"
#else
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _WIN32
extern char **environ;
#endif

static int command_run(const char *const argv[], char **output, size_t *length, int input_fd);

static bool fail(char *error, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error, CED_ERROR, format, args);
    va_end(args);
    return false;
}

bool ced_join(char *out, size_t cap, const char *root, const char *relative) {
    int n = snprintf(out, cap, "%s/%s", root, relative);
    return n >= 0 && (size_t)n < cap;
}

const char *ced_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool ced_markdown_file(const char *path) {
    const char *ext = strrchr(ced_basename(path), '.');
    return ext && (!strcasecmp(ext, ".md") || !strcasecmp(ext, ".markdown"));
}

const char *ced_language(const char *path) {
    if (ced_markdown_file(path)) return "markdown";
    const char *ext = strrchr(ced_basename(path), '.');
    if (!ext) return "text";
    static const char *pairs[][2] = {
        {".c", "c"}, {".h", "c"}, {".cpp", "cpp"}, {".hpp", "cpp"},
        {".cc", "cpp"}, {".m", "c"}, {".js", "js"}, {".jsx", "js"}, {".mjs", "js"},
        {".ts", "ts"}, {".tsx", "ts"}, {".py", "python"}, {".rs", "rust"},
        {".go", "go"}, {".java", "java"}, {".kt", "kotlin"}, {".kts", "kotlin"},
        {".json", "json"}, {".yaml", "yaml"}, {".yml", "yaml"}, {".sh", "shell"}, {".sql", "sql"},
        {".css", "css"}, {".html", "html"}, {".xml", "html"}
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if (!strcmp(ext, pairs[i][0])) return pairs[i][1];
    return "text";
}

static bool excluded(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git");
}

typedef struct { char *name; bool dir; } DirectoryItem;

static int compare_items(const void *left, const void *right) {
    const DirectoryItem *a = left, *b = right;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    return strcmp(a->name, b->name);
}

static CedEntry *find_entry(CedEntry **entries, size_t count, const char *path);

typedef struct {
    CedEntry **ignored, **previous;
    size_t ignored_count, previous_count;
    CedSearch *search;
    bool children_only;
} TreeScan;

static bool search_cancelled(CedSearch *search) {
    if (!search) return false;
    pthread_mutex_lock(&search->mutex);
    bool cancelled = search->cancelled;
    pthread_mutex_unlock(&search->mutex);
    return cancelled;
}

static bool scan(CedWorkspace *ws, const char *relative, int parent, int depth, size_t *capacity, const TreeScan *options, bool inherited_ignored) {
    if (search_cancelled(options->search)) return true;
    char path[CED_PATH];
    if (depth > 64 || !ced_join(path, sizeof path, ws->root, relative)) {
        ws->truncated = true;
        return true;
    }
    DIR *dir = opendir(path);
    if (!dir) { if (parent >= 0) ws->truncated = true; return parent >= 0; }
    DirectoryItem *items = NULL;
    size_t count = 0, item_capacity = 0;
    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(dir))) {
        if (count % 64 == 0 && search_cancelled(options->search)) break;
        if (excluded(entry->d_name)) continue;
        char child[CED_PATH];
        struct stat st;
        if (!ced_join(child, sizeof child, path, entry->d_name)) { ws->truncated = true; continue; }
        if (lstat(child, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) continue;
        if (count >= CED_MAX_ENTRIES) { ws->truncated = true; break; }
        if (count == item_capacity) {
            size_t capacity = item_capacity ? item_capacity * 2 : 32;
            if (capacity > CED_MAX_ENTRIES) capacity = CED_MAX_ENTRIES;
            DirectoryItem *next = realloc(items, capacity * sizeof *items);
            if (!next) { ok = false; break; }
            items = next;
            item_capacity = capacity;
        }
        items[count].name = strdup(entry->d_name);
        if (!items[count].name) { ok = false; break; }
        items[count++].dir = S_ISDIR(st.st_mode);
    }
    closedir(dir);
    if (count > 1) qsort(items, count, sizeof *items, compare_items);
    for (size_t i = 0; i < count && ok; i++) {
        if (ws->count >= CED_MAX_ENTRIES) { ws->truncated = true; break; }
        char child[CED_PATH];
        int n = snprintf(child, sizeof child, "%s%s%s", relative, *relative ? "/" : "", items[i].name);
        if (n < 0 || (size_t)n >= sizeof child) { ws->truncated = true; continue; }
        if ((size_t)ws->count == *capacity) {
            size_t cap = *capacity ? *capacity * 2 : 64;
            if (cap > CED_MAX_ENTRIES) cap = CED_MAX_ENTRIES;
            CedEntry *entries = realloc(ws->entries, cap * sizeof *entries);
            if (!entries) { ok = false; break; }
            ws->entries = entries;
            *capacity = cap;
        }
        int index = ws->count;
        char *copy = strdup(child);
        if (!copy) { ok = false; break; }
        bool ignored = inherited_ignored;
        if (!ignored && items[i].dir && options->ignored_count) {
            const CedEntry *candidate = find_entry(options->ignored, options->ignored_count, child);
            ignored = candidate && candidate->ignored;
        }
        bool expanded = false;
        if (items[i].dir && options->previous_count) {
            const CedEntry *previous = find_entry(options->previous, options->previous_count, child);
            expanded = previous && previous->expanded;
        }
        bool deferred = items[i].dir && ignored && !expanded && depth < 64;
        ws->entries[ws->count++] = (CedEntry){.path = copy, .parent = parent, .directory = items[i].dir,
                                             .ignored = ignored, .deferred = deferred, .expanded = expanded};
        if (items[i].dir && !deferred && !options->children_only)
            ok = scan(ws, child, index, depth + 1, capacity, options, ignored);
        if (items[i].dir && depth == 64) ws->truncated = true;
        ws->entries[index].subtree_end = ws->count;
    }
    for (size_t i = 0; i < count; i++) free(items[i].name);
    free(items);
    return ok;
}

int ced_workspace_compact_directory(const CedWorkspace *ws, int index) {
    int end = ws->entries[index].subtree_end;
    while (index + 1 < end) {
        const CedEntry *child = &ws->entries[index + 1];
        if (!child->directory || child->subtree_end != end ||
            (ws->entries[index].ignored && child->deferred)) break;
        index++;
    }
    return index;
}

static int compare_entry_paths(const void *left, const void *right) {
    const CedEntry *const *a = left, *const *b = right;
    return strcmp((*a)->path, (*b)->path);
}

static CedEntry *find_entry(CedEntry **entries, size_t count, const char *path) {
    CedEntry key = {.path = (char *)path}, *key_ptr = &key;
    CedEntry **entry = bsearch(&key_ptr, entries, count, sizeof *entries, compare_entry_paths);
    return entry ? *entry : NULL;
}

static CedEntry **sorted_entries(const CedWorkspace *ws) {
    if (!ws->count) return NULL;
    CedEntry **entries = malloc((size_t)ws->count * sizeof *entries);
    if (!entries) return NULL;
    for (int i = 0; i < ws->count; i++) entries[i] = &ws->entries[i];
    qsort(entries, (size_t)ws->count, sizeof *entries, compare_entry_paths);
    return entries;
}

static int check_ignored(const CedWorkspace *ws, const bool *skip, char **output, size_t *length) {
    /* A seekable input avoids pipe deadlocks and argv limits for large trees. */
    FILE *input = tmpfile();
    if (!input) return -1;
    bool ready = true;
    for (int i = 0; i < ws->count; i++) {
        if (skip && skip[i]) continue;
        const char *path = ws->entries[i].path;
        size_t size = strlen(path) + 1;
        /* A leading colon in a filename must not be treated as Git pathspec magic. */
        if (fwrite("./", 1, 2, input) != 2 || fwrite(path, 1, size, input) != size) {
            ready = false;
            break;
        }
    }
    if (fflush(input) || fseek(input, 0, SEEK_SET)) ready = false;
    const char *args[] = {"git", "-C", ws->root, "check-ignore", "--stdin", "-z", NULL};
    int status = ready ? command_run(args, output, length, fileno(input)) : -1;
    fclose(input);
    return status;
}

static bool skip_submodule_paths(const CedWorkspace *ws, CedEntry **entries, bool *skip) {
    const char *args[] = {"git", "-C", ws->root, "ls-files", "--stage", "-z", NULL};
    char *output = NULL;
    size_t length = 0;
    int status = command_run(args, &output, &length, -1);
    bool found = false;
    for (size_t offset = 0; !status && offset < length;) {
        char *record = output + offset, *end = memchr(record, 0, length - offset);
        if (!end) break;
        size_t size = (size_t)(end - record);
        if (size > 7 && !memcmp(record, "160000 ", 7)) {
            char *tab = memchr(record, '\t', size);
            CedEntry *entry = tab ? find_entry(entries, (size_t)ws->count, tab + 1) : NULL;
            if (entry && entry->directory) {
                skip[entry - ws->entries] = true;
                found = true;
            }
        }
        offset += size + 1;
    }
    free(output);
    /* Entries are in depth-first order, with parents before their children. */
    if (found) for (int i = 0; i < ws->count; i++) {
        int parent = ws->entries[i].parent;
        if (parent >= 0 && skip[parent]) skip[i] = true;
    }
    return found;
}

static void mark_ignored(CedWorkspace *ws) {
    CedEntry **entries = sorted_entries(ws);
    if (!entries) return;

    char *output = NULL;
    size_t length = 0;
    int status = check_ignored(ws, NULL, &output, &length);
    /* Git aborts the entire batch if a path lies inside a registered submodule. */
    if (status == 128) {
        bool *skip = calloc((size_t)ws->count, sizeof *skip);
        if (skip && skip_submodule_paths(ws, entries, skip)) {
            free(output);
            output = NULL;
            length = 0;
            status = check_ignored(ws, skip, &output, &length);
        }
        free(skip);
    }
    for (size_t offset = 0; !status && offset < length;) {
        char *path = output + offset, *end = memchr(path, 0, length - offset);
        if (!end) break;
        if (end - path > 2 && path[0] == '.' && path[1] == '/') {
            CedEntry *entry = find_entry(entries, (size_t)ws->count, path + 2);
            if (entry) entry->ignored = true;
        }
        offset = (size_t)(end - output) + 1;
    }
    free(output);
    free(entries);
}

/* Git reports ignored untracked trees as directory paths without listing
 * their contents. Confirm each directory itself is ignored: --directory can
 * also report a parent whose children match a pattern such as build/*. */
static void ignored_directories(CedWorkspace *dirs) {
    const char *args[] = {"git", "-C", dirs->root, "ls-files", "--others", "--ignored",
                         "--exclude-standard", "--directory", "-z", NULL};
    char *output = NULL;
    size_t length = 0;
    if (command_run(args, &output, &length, -1)) { free(output); return; }
    size_t capacity = 0;
    for (size_t offset = 0; offset < length && dirs->count < CED_MAX_ENTRIES;) {
        char *path = output + offset, *end = memchr(path, 0, length - offset);
        if (!end) break;
        offset = (size_t)(end - output) + 1;
        if (end <= path + 1 || end[-1] != '/') continue;
        if ((size_t)dirs->count == capacity) {
            size_t size = capacity ? capacity * 2 : 32;
            CedEntry *next = realloc(dirs->entries, size * sizeof *next);
            if (!next) break;
            dirs->entries = next;
            capacity = size;
        }
        end[-1] = 0;
        char *copy = strdup(path);
        if (!copy) break;
        dirs->entries[dirs->count++] = (CedEntry){.path = copy, .parent = -1, .directory = true};
    }
    free(output);
    mark_ignored(dirs);
}

bool ced_workspace_read_children(CedWorkspace *children, const char *root, const char *relative, char *error) {
    char path[CED_PATH], canonical[CED_PATH];
    struct stat st;
    size_t length = strlen(root);
    int written = snprintf(path, sizeof path, "%s%s%s", root, length && root[length - 1] == '/' ? "" : "/", relative);
    if (written < 0 || (size_t)written >= sizeof path || lstat(path, &st) || !S_ISDIR(st.st_mode) ||
        !realpath(path, canonical) || strcmp(path, canonical))
        return fail(error, "Could not open folder: %s", relative);
    int depth = 1;
    for (const char *p = relative; *p; p++) if (*p == '/') depth++;
    snprintf(children->root, sizeof children->root, "%s", root);
    size_t capacity = 0;
    if (!scan(children, relative, -1, depth, &capacity, &(TreeScan){.children_only = true}, true)) {
        ced_workspace_free(children);
        return fail(error, "Could not read folder: access denied or out of memory.");
    }
    return true;
}

bool ced_workspace_insert_children(CedWorkspace *ws, int index, CedWorkspace *children, char *error) {
    int first = index + 1;
    int added = children->count;
    if (added > CED_MAX_ENTRIES - ws->count) added = CED_MAX_ENTRIES - ws->count;
    if (added) {
        CedEntry *entries = realloc(ws->entries, (size_t)(ws->count + added) * sizeof *entries);
        if (!entries) return fail(error, "Could not expand folder: out of memory.");
        ws->entries = entries;
        memmove(entries + first + added, entries + first, (size_t)(ws->count - first) * sizeof *entries);
        ws->count += added;
        for (int i = 0; i < first; i++) if (entries[i].subtree_end > index) entries[i].subtree_end += added;
        for (int i = first; i < first + added; i++) {
            entries[i] = children->entries[i - first];
            entries[i].parent = index;
            entries[i].subtree_end = i + 1;
            children->entries[i - first].path = NULL;
        }
        for (int i = first + added; i < ws->count; i++) {
            entries[i].subtree_end += added;
            if (entries[i].parent > index) entries[i].parent += added;
        }
    }
    ws->entries[index].deferred = false;
    ws->truncated |= children->truncated || added < children->count;
    ced_workspace_free(children);
    return true;
}

bool ced_workspace_resolve(const char *path, char root[CED_PATH], char *error) {
    struct stat st;
    if (!realpath(path, root) || stat(root, &st) || !S_ISDIR(st.st_mode))
        return fail(error, "Could not open folder: %s", path);
    return true;
}

bool ced_workspace_open(CedWorkspace *ws, const char *path, char *error) {
    CedWorkspace next = {0};
    if (!ced_workspace_resolve(path, next.root, error)) return false;
    CedWorkspace dirs = {0};
    snprintf(dirs.root, sizeof dirs.root, "%s", next.root);
    ignored_directories(&dirs);
    CedEntry **ignored = sorted_entries(&dirs);
    CedEntry **previous = !strcmp(ws->root, next.root) ? sorted_entries(ws) : NULL;
    TreeScan options = {
        .ignored = ignored, .ignored_count = ignored ? (size_t)dirs.count : 0,
        .previous = previous, .previous_count = previous ? (size_t)ws->count : 0
    };
    size_t capacity = 0;
    bool ok = scan(&next, "", -1, 0, &capacity, &options, false);
    free(previous);
    free(ignored);
    ced_workspace_free(&dirs);
    if (!ok) {
        ced_workspace_free(&next);
        return fail(error, "Could not read folder: access denied or out of memory.");
    }
    mark_ignored(&next);
    ced_workspace_free(ws);
    *ws = next;
    return true;
}

void ced_workspace_free(CedWorkspace *ws) {
    for (int i = 0; i < ws->count; i++) free(ws->entries[i].path);
    free(ws->entries);
    memset(ws, 0, sizeof *ws);
}

static bool read_text(const char *path, char **out, bool *crlf, char *error) {
    FILE *f = fopen(path, "rb");
    if (!f) return fail(error, "%s: %s", ced_basename(path), strerror(errno));
    struct stat st;
    size_t capacity = 4096;
    if (!fstat(fileno(f), &st) && st.st_size >= 0) {
        if (st.st_size >= CED_TEXT_CAP) { fclose(f); return fail(error, "Binary, unreadable, or larger than 2 MiB: %s", ced_basename(path)); }
        capacity = (size_t)st.st_size + 1;
    }
    char *text = malloc(capacity);
    if (!text) { fclose(f); return fail(error, "Out of memory."); }
    size_t n = 0;
    while (true) {
        n += fread(text + n, 1, capacity - n, f);
        if (ferror(f) || feof(f) || n < capacity || capacity == CED_TEXT_CAP) break;
        size_t cap = capacity > CED_TEXT_CAP / 2 ? CED_TEXT_CAP : capacity * 2;
        char *next = realloc(text, cap);
        if (!next) { free(text); fclose(f); return fail(error, "Out of memory."); }
        text = next;
        capacity = cap;
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad || n == CED_TEXT_CAP || memchr(text, 0, n)) {
        free(text);
        return fail(error, "Binary, unreadable, or larger than 2 MiB: %s", ced_basename(path));
    }
    bool windows = false;
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        if (text[r] == '\r' && r + 1 < n && text[r + 1] == '\n') { windows = true; continue; }
        text[w++] = text[r];
    }
    text[w] = 0;
    if (crlf) *crlf = windows;
    *out = text;
    return true;
}

bool ced_document_open(CedDocument *doc, const char *path, char *error) {
    CedDocument next = {0};
    struct stat st;
    if (!realpath(path, next.path) || stat(next.path, &st) || !S_ISREG(st.st_mode))
        return fail(error, "Invalid file: %s", path);
    if (!read_text(next.path, &next.text, &next.crlf, error)) return false;
    /* CUI edits in place and requires the full writable capacity. Readers
     * used by search, conflict checks and diffs keep only the file-sized buffer. */
    char *editable = realloc(next.text, CED_TEXT_CAP);
    if (!editable) { free(next.text); return fail(error, "Out of memory."); }
    next.text = editable;
    next.saved = strdup(next.text);
    next.length = strlen(next.text);
    if (!next.saved) { free(next.text); return fail(error, "Out of memory."); }
    ced_document_free(doc);
    *doc = next;
    return true;
}

bool ced_document_dirty(const CedDocument *doc) {
    return doc->text && (!doc->saved || strcmp(doc->text, doc->saved));
}

bool ced_document_save(CedDocument *doc, const char *path, bool force, char *error) {
    if (strlen(path) >= sizeof doc->path) return fail(error, "Path too long.");
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) return fail(error, "Destination is not a regular file.");
    if (!force && exists) {
        char *disk = NULL;
        if (!doc->path[0] || strcmp(path, doc->path)) return fail(error, "Destination already exists.");
        if (!read_text(path, &disk, NULL, error)) return false;
        bool conflict = !doc->saved || strcmp(disk, doc->saved);
        free(disk);
        if (conflict) return fail(error, "File changed on disk. Use Save As to keep both versions.");
    }
    /* Keep original intact on write errors; replace atomically in the same directory. */
    char temp[CED_PATH];
    int n = snprintf(temp, sizeof temp, "%s.ced-XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof temp) return fail(error, "Path too long.");
    char *saved = strdup(doc->text);
    if (!saved) return fail(error, "Out of memory.");
    int fd = mkstemp(temp);
    if (fd < 0) { free(saved); return fail(error, "Could not save: %s", strerror(errno)); }
    bool ok = !exists || fchmod(fd, st.st_mode & 0777) == 0;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); ok = false; }
    else {
        if (doc->crlf) {
            for (const char *p = doc->text; *p; p++) {
                if (*p == '\n' && fputc('\r', f) == EOF) ok = false;
                if (fputc(*p, f) == EOF) ok = false;
            }
        } else {
            size_t len = strlen(doc->text);
            if (fwrite(doc->text, 1, len, f) != len) ok = false;
        }
        if (fflush(f) || fsync(fd)) ok = false;
        if (fclose(f)) ok = false;
    }
    if (ok && rename(temp, path)) ok = false;
    if (!ok) {
        unlink(temp);
        free(saved);
        return fail(error, "Could not write file: %s", strerror(errno));
    }
    free(doc->saved);
    doc->saved = saved;
#ifdef _WIN32
    if (!realpath(path,doc->path)) snprintf(doc->path,sizeof doc->path,"%s",path);
#else
    snprintf(doc->path, sizeof doc->path, "%s", path);
#endif
    doc->length = strlen(doc->text);
    return true;
}

void ced_document_free(CedDocument *doc) {
    free(doc->text);
    free(doc->saved);
    memset(doc, 0, sizeof *doc);
}

static void *search_worker(void *user) {
    CedSearch *s = user;
    bool incomplete = false;
    if (s->scan_all) {
        size_t capacity = 0;
        incomplete = !scan(&s->snapshot, "", -1, 0, &capacity, &(TreeScan){.search = s}, false) || s->snapshot.truncated;
    }
    size_t query_length = strlen(s->query);
    for (int i = 0; i < s->snapshot.count; i++) {
        if (search_cancelled(s)) break;
        CedEntry *entry = &s->snapshot.entries[i];
        if (entry->directory) continue;
        char path[CED_PATH], error[CED_ERROR], *text = NULL;
        if (!ced_join(path, sizeof path, s->snapshot.root, entry->path) || !read_text(path, &text, NULL, error)) continue;
        const char *line_start = text;
        int line = 1;
        for (const char *match = text; (match = strstr(match, s->query)); match += query_length) {
            while (line_start < match) {
                const char *nl = strchr(line_start, '\n');
                if (!nl || nl >= match) break;
                line_start = nl + 1;
                line++;
            }
            if (s->count == CED_MAX_HITS) { s->truncated = true; break; }
            CedHit *hit = &s->hits[s->count++];
            snprintf(hit->path, sizeof hit->path, "%s", entry->path);
            hit->line = line;
            int column = 0;
            for (const unsigned char *p = (const unsigned char *)line_start; p < (const unsigned char *)match; p++) {
                if (*p == '\t') column = (column / 4 + 1) * 4;
                else if ((*p & 0xc0) != 0x80) column++;
            }
            hit->column = column + 1;
            size_t len = strcspn(line_start, "\r\n");
            if (len >= sizeof hit->preview) len = sizeof hit->preview - 1;
            memcpy(hit->preview, line_start, len);
            hit->preview[len] = 0;
        }
        free(text);
        if (s->truncated) break;
    }
    s->truncated |= incomplete;
    pthread_mutex_lock(&s->mutex);
    s->done = true;
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

bool ced_search_start(CedSearch *s, const CedWorkspace *ws, const char *query, bool include_ignored) {
    ced_search_cancel(s);
    s->count = 0;
    s->truncated = false;
    s->done = false;
    s->cancelled = false;
    if (!*query) return true;
    snprintf(s->query, sizeof s->query, "%s", query);
    snprintf(s->snapshot.root, sizeof s->snapshot.root, "%s", ws->root);
    s->scan_all = false;
    if (include_ignored) for (int i = 0; i < ws->count; i++) {
        if (ws->entries[i].deferred) { s->scan_all = true; break; }
    }
    if (!s->scan_all) {
        s->snapshot.entries = calloc((size_t)ws->count + 1, sizeof(CedEntry));
        if (!s->snapshot.entries) return false;
        for (int i = 0; i < ws->count; i++) {
            const CedEntry *entry = &ws->entries[i];
            if (entry->directory || (!include_ignored && entry->ignored)) continue;
            char *path = strdup(entry->path);
            if (!path) { ced_workspace_free(&s->snapshot); return false; }
            s->snapshot.entries[s->snapshot.count++] = (CedEntry){.path = path};
        }
    }
    if (pthread_create(&s->thread, NULL, search_worker, s)) {
        ced_workspace_free(&s->snapshot);
        return false;
    }
    s->running = true;
    return true;
}

bool ced_search_done(CedSearch *s) {
    pthread_mutex_lock(&s->mutex);
    bool done = s->done;
    pthread_mutex_unlock(&s->mutex);
    return done;
}

void ced_search_finish(CedSearch *s) {
    if (!s->running) return;
    pthread_join(s->thread, NULL);
    s->running = false;
    ced_workspace_free(&s->snapshot);
}

void ced_search_cancel(CedSearch *s) {
    if (!s->running) return;
    pthread_mutex_lock(&s->mutex);
    s->cancelled = true;
    pthread_mutex_unlock(&s->mutex);
    ced_search_finish(s);
}

static int command_run(const char *const argv[], char **output, size_t *length, int input_fd) {
#ifdef _WIN32
    return ced_windows_command(argv, output, length, input_fd);
#else
    int pipes[2];
    *output = NULL;
    if (length) *length = 0;
    if (pipe(pipes)) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (input_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, input_fd, STDIN_FILENO);
        if (input_fd != STDIN_FILENO) posix_spawn_file_actions_addclose(&actions, input_fd);
    } else posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    pid_t pid;
    int spawned = posix_spawnp(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (spawned) { close(pipes[0]); return -1; }
    const size_t cap = 8 * 1024 * 1024;
    size_t capacity = 8192;
    char *buf = malloc(capacity);
    size_t used = 0;
    char chunk[8192];
    ssize_t n;
    bool truncated = false;
    while ((n = read(pipes[0], chunk, sizeof chunk)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (truncated) continue; /* Drain the pipe even after a limit/OOM. */
        size_t needed = used + (size_t)n + 1;
        if (!buf || needed > cap + 1) { truncated = true; continue; }
        if (needed > capacity) {
            size_t next_capacity = capacity * 2;
            if (next_capacity < needed) next_capacity = needed;
            if (next_capacity > cap + 1) next_capacity = cap + 1;
            char *next = realloc(buf, next_capacity);
            if (!next) { truncated = true; continue; }
            buf = next;
            capacity = next_capacity;
        }
        memcpy(buf + used, chunk, (size_t)n);
        used += (size_t)n;
    }
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) { status = -1; break; }
    if (buf) {
        buf[used] = 0;
        char *compact = realloc(buf, used + 1);
        *output = compact ? compact : buf;
    }
    if (length) *length = used;
    if (!buf || truncated || status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

int ced_command(const char *const argv[], char **output, size_t *length) {
    return command_run(argv, output, length, -1);
}

static bool git_run(const char *const argv[], char **out, char *error, bool show_output) {
    int status=ced_command(argv,out,NULL);
    if (show_output && *out && **out) ced_output_text("Git",*out);
    if (!status) return true;
    fail(error, "%s", *out && **out ? *out : "Could not run Git.");
    free(*out);
    *out = NULL;
    return false;
}

void ced_git_free(CedGit *git) {
    free(git->changes);
    memset(git, 0, sizeof *git);
}

bool ced_git_refresh(CedGit *git, const char *root, char *error) {
    CedGit next = {0};
    char *out = NULL;
    const char *root_args[] = {"git", "-C", root, "rev-parse", "--show-toplevel", NULL};
    if (!git_run(root_args,&out,error,false)) { ced_git_free(git); return false; }
    out[strcspn(out, "\r\n")] = 0;
    snprintf(next.root, sizeof next.root, "%s", out);
    free(out);
    const char *branch_args[] = {"git", "-C", next.root, "symbolic-ref", "--short", "HEAD", NULL};
    if (ced_command(branch_args, &out, NULL)) {
        free(out);
        const char *detached[] = {"git", "-C", next.root, "rev-parse", "--short", "HEAD", NULL};
        if (ced_command(detached, &out, NULL)) { free(out); out = strdup("HEAD"); }
    }
    snprintf(next.branch, sizeof next.branch, "%.*s", (int)strcspn(out ? out : "", "\r\n"), out ? out : "");
    free(out);
    const char *status_args[] = {"git", "--no-optional-locks", "-C", next.root,
                               "status", "--porcelain=v1", "-z", "--untracked-files=all", NULL};
    size_t length = 0;
    if (ced_command(status_args, &out, &length)) {
        fail(error, "%s", out ? out : "Could not read Git status.");
        free(out);
        return false;
    }
    size_t capacity = 0;
    for (size_t pos = 0; pos < length;) {
        char *record = out + pos;
        size_t len = strlen(record);
        pos += len + 1;
        if (len < 4) continue;
        if (next.count == CED_MAX_CHANGES) { next.truncated = true; break; }
        if ((size_t)next.count == capacity) {
            size_t cap = capacity ? capacity * 2 : 16;
            if (cap > CED_MAX_CHANGES) cap = CED_MAX_CHANGES;
            CedChange *changes = realloc(next.changes, cap * sizeof *changes);
            if (!changes) { free(out); ced_git_free(&next); return fail(error, "Out of memory."); }
            next.changes = changes;
            capacity = cap;
        }
        CedChange *change = &next.changes[next.count++];
        memset(change, 0, sizeof *change);
        change->index = record[0];
        change->worktree = record[1];
        snprintf(change->path, sizeof change->path, "%s", record + 3);
        if ((record[0] == 'R' || record[0] == 'C' || record[1] == 'R' || record[1] == 'C') && pos < length) {
            snprintf(change->original, sizeof change->original, "%s", out + pos);
            pos += strlen(out + pos) + 1;
        }
    }
    free(out);
    next.available = true;
    ced_git_free(git);
    *git = next;
    return true;
}

bool ced_git_stage(CedGit *git, const char *path, bool unstage, char *error) {
    const char *original = NULL;
    for (int i = 0; i < git->count; i++)
        if (!strcmp(git->changes[i].path, path) && git->changes[i].original[0]) original = git->changes[i].original;
    const char *args[] = {"git", "--literal-pathspecs", "-C", git->root,
                          unstage ? "restore" : "add", unstage ? "--staged" : "--all", "--", path, original, NULL};
    char *out = NULL;
    bool ok = git_run(args,&out,error,true);
    free(out);
    if (!ok && unstage) {
        /* An unborn branch has no HEAD to restore from. */
        const char *head[] = {"git", "-C", git->root, "rev-parse", "--verify", "HEAD", NULL};
        int result = ced_command(head, &out, NULL);
        free(out);
        if (result) {
            const char *remove[] = {"git", "--literal-pathspecs", "-C", git->root,
                                    "rm", "--cached", "-r", "--", path, NULL};
            ok = git_run(remove,&out,error,true);
            free(out);
        }
    }
    return ok;
}

bool ced_git_stage_all(CedGit *git, bool unstage, char *error) {
    char *out = NULL;
    if (unstage) {
        const char *head[] = {"git", "-C", git->root, "rev-parse", "--verify", "HEAD", NULL};
        int result = ced_command(head, &out, NULL);
        free(out);
        if (result) {
            /* Clear an unborn branch's index without touching working files. */
            const char *empty[] = {"git", "-C", git->root, "read-tree", "--empty", NULL};
            bool ok = git_run(empty,&out,error,true);
            free(out);
            return ok;
        }
    }
    const char *args[] = {"git", "-C", git->root, unstage ? "restore" : "add",
                         unstage ? "--staged" : "--all", "--", ".", NULL};
    bool ok = git_run(args,&out,error,true);
    free(out);
    return ok;
}

bool ced_git_discard(CedGit *git, const char *path, char *error) {
    if (!git->available || !path || !*path || *path == '/' || strlen(path) >= CED_PATH)
        return fail(error, "Invalid Git path.");
    /* Only one repository-relative literal file may be discarded. */
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1 && *part == '.') || (length == 2 && !memcmp(part,"..",2)) ||
            (length == 4 && !memcmp(part,".git",4))) return fail(error, "Invalid Git path.");
        if (!end) break;
        part = end + 1;
        if (!*part) return fail(error, "Discarding directories is not supported.");
    }
    CedGit current = {0};
    if (!ced_git_refresh(&current, git->root, error)) return false;
    const CedChange *change = NULL;
    for (int n = 0; n < current.count; n++)
        if (!strcmp(current.changes[n].path,path) && current.changes[n].worktree != ' ') { change = &current.changes[n]; break; }
    bool ok = false;
    if (!change || change->worktree == ' ') { fail(error, "No unstaged changes for this file."); goto done; }
    if (change->index == 'U' || change->worktree == 'U' ||
        (change->index == 'A' && change->worktree == 'A') || (change->index == 'D' && change->worktree == 'D')) {
        fail(error, "Resolve this file's merge conflict before discarding changes."); goto done;
    }
    char absolute[CED_PATH]; struct stat st;
    if (!ced_join(absolute,sizeof absolute,current.root,path)) { fail(error,"Path too long."); goto done; }
    int exists = lstat(absolute,&st);
    if (!exists && S_ISDIR(st.st_mode)) { fail(error,"Discarding directories or submodules is not supported."); goto done; }
    if (exists && errno != ENOENT) { fail(error,"Could not inspect this file: %s",strerror(errno)); goto done; }
    char *out = NULL;
    if (change->index == '?' && change->worktree == '?') {
        const char *args[] = {"git","--literal-pathspecs","-C",current.root,"clean","-f","--",path,NULL};
        ok = git_run(args,&out,error,true);
        if (ok && (!lstat(absolute,&st) || errno != ENOENT)) ok = fail(error,"The untracked file could not be removed.");
    } else {
        /* The default source is the index, not HEAD: staged content survives. */
        const char *args[] = {"git","--literal-pathspecs","-C",current.root,"restore","--worktree","--",path,NULL};
        ok = git_run(args,&out,error,true);
    }
    free(out);
done:
    ced_git_free(&current); return ok;
}

bool ced_git_sync(CedGit *git, bool pull, char *error) {
    const char *args[] = {"env", "GIT_TERMINAL_PROMPT=0", "git", "-C", git->root,
                         pull ? "pull" : "push", pull ? "--ff-only" : NULL, NULL};
    char *out = NULL;
    bool ok = git_run(args,&out,error,true);
    free(out);
    return ok;
}

bool ced_git_commit(CedGit *git, const char *message, char *error) {
    if (!*message) return fail(error, "Enter a commit message.");
    const char *args[] = {"git", "-C", git->root, "-c", "core.editor=true", "commit", "-m", message, NULL};
    char *out = NULL;
    bool ok = git_run(args,&out,error,true);
    free(out);
    return ok;
}

static bool git_blob(CedGit *git, const char *prefix, const char *path, char **out, char *error) {
    char spec[CED_PATH + 16];
    snprintf(spec, sizeof spec, "%s%s", prefix, path);
    const char *args[] = {"git", "-C", git->root, "show", spec, NULL};
    size_t length = 0;
    if (ced_command(args, out, &length)) {
        fail(error, "%s", *out ? *out : "Could not read Git version.");
        free(*out); *out = NULL; return false;
    }
    if (length >= CED_TEXT_CAP || memchr(*out, 0, length)) {
        free(*out); *out = NULL;
        return fail(error, "Diff unavailable for binary files or files larger than 2 MiB.");
    }
    return true;
}

bool ced_git_versions(CedGit *git, const CedChange *change, bool staged,
                      char **before, char **after, char *error) {
    *before = *after = NULL;
    bool untracked = change->index == '?';
    bool added = staged && change->index == 'A';
    bool deleted = staged ? change->index == 'D' : change->worktree == 'D';
    const char *old_path = staged && change->original[0] ? change->original : change->path;
    if (untracked || added) *before = strdup("");
    else if (!git_blob(git, staged ? "HEAD:" : ":", old_path, before, error)) return false;
    bool ok = true;
    if (deleted) *after = strdup("");
    else if (staged) ok = git_blob(git, ":", change->path, after, error);
    else {
        char path[CED_PATH];
        ok = ced_join(path, sizeof path, git->root, change->path) && read_text(path, after, NULL, error);
    }
    if (!ok || !*before || !*after) { free(*before); free(*after); *before = *after = NULL; return false; }
    return true;
}
