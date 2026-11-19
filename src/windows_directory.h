#ifndef CED_WINDOWS_DIRECTORY_H
#define CED_WINDOWS_DIRECTORY_H
#include "windows_compat.h"
#include <errno.h>
#include <string.h>

struct dirent { char d_name[4096]; };
typedef struct { HANDLE search; WIN32_FIND_DATAW data; bool first; struct dirent entry; } DIR;

static DIR *opendir(const char *path) {
    char pattern[CED_PATH];
    if (!ced_join(pattern, sizeof pattern, path, "*")) { errno = ENAMETOOLONG; return NULL; }
    wchar_t *wide = ced_windows_wide(pattern);
    DIR *dir = calloc(1, sizeof *dir);
    if (!wide || !dir) { free(wide); free(dir); return NULL; }
    dir->search = FindFirstFileW(wide, &dir->data); free(wide);
    if (dir->search == INVALID_HANDLE_VALUE) { free(dir); errno = ENOENT; return NULL; }
    dir->first = true; return dir;
}

static struct dirent *readdir(DIR *dir) {
    if (!dir->first && !FindNextFileW(dir->search, &dir->data)) {
        errno = GetLastError() == ERROR_NO_MORE_FILES ? 0 : EIO; return NULL;
    }
    dir->first = false;
    char *name = ced_windows_utf8(dir->data.cFileName);
    if (!name) { errno = EILSEQ; return NULL; }
    snprintf(dir->entry.d_name, sizeof dir->entry.d_name, "%s", name); free(name);
    return &dir->entry;
}

static int closedir(DIR *dir) { FindClose(dir->search); free(dir); return 0; }
#endif
