#ifndef CED_WORKSPACE_H
#define CED_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include "windows_compat.h"

#define CED_PATH 4096
#define CED_TEXT_CAP (2 * 1024 * 1024 + 1)
#define CED_MAX_ENTRIES 20000
#define CED_MAX_HITS 500
#define CED_MAX_CHANGES 4096
#define CED_ERROR 512

typedef struct {
    char *path;                    /* Relative to the workspace; sorted depth first. */
    int parent;
    bool directory, expanded, ignored;
} CedEntry;

typedef struct {
    char root[CED_PATH];
    CedEntry *entries;
    int count;
    bool truncated;
} CedWorkspace;

typedef struct {
    char path[CED_PATH];
    char *text;
    char *saved;
    size_t length;
    bool crlf;
    uint64_t id, version;
} CedDocument;

typedef struct {
    char path[CED_PATH];
    char preview[240];
    int line, column;
} CedHit;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool running, done, cancelled, truncated;
    char query[256];
    CedWorkspace snapshot;
    CedHit hits[CED_MAX_HITS];
    int count;
} CedSearch;

typedef struct {
    char path[CED_PATH], original[CED_PATH];
    char index, worktree;
} CedChange;

typedef struct {
    char root[CED_PATH], branch[256];
    CedChange *changes;
    int count;
    bool available, truncated;
} CedGit;

bool ced_join(char *out, size_t cap, const char *root, const char *relative);
const char *ced_basename(const char *path);
const char *ced_language(const char *path);
bool ced_markdown_file(const char *path);
bool ced_workspace_open(CedWorkspace *ws, const char *path, char *error);
void ced_workspace_free(CedWorkspace *ws);
bool ced_document_open(CedDocument *doc, const char *path, char *error);
bool ced_document_save(CedDocument *doc, const char *path, bool force, char *error);
bool ced_document_dirty(const CedDocument *doc);
void ced_document_free(CedDocument *doc);
bool ced_search_start(CedSearch *search, const CedWorkspace *ws, const char *query, bool include_ignored);
bool ced_search_done(CedSearch *search);
void ced_search_finish(CedSearch *search);
void ced_search_cancel(CedSearch *search);
bool ced_git_refresh(CedGit *git, const char *root, char *error);
void ced_git_free(CedGit *git);
bool ced_git_stage(CedGit *git, const char *path, bool unstage, char *error);
bool ced_git_stage_all(CedGit *git, bool unstage, char *error);
bool ced_git_discard(CedGit *git, const char *path, char *error);
bool ced_git_sync(CedGit *git, bool pull, char *error);
bool ced_git_commit(CedGit *git, const char *message, char *error);
bool ced_git_versions(CedGit *git, const CedChange *change, bool staged,
                      char **before, char **after, char *error);
/* argv is passed directly to posix_spawnp; no shell interpretation. */
int ced_command(const char *const argv[], char **output, size_t *length);

#endif
