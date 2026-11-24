#ifndef CED_EXTENSION_FILES_H
#define CED_EXTENSION_FILES_H

#include "workspace.h"
#include <sys/stat.h>

typedef struct {
#ifdef _WIN32
    HANDLE handle;
    char path[CED_PATH];
#else
    int fd;
#endif
} CedExtDirectory;

bool ced_ext_directory(CedExtDirectory *dir, const CedExtDirectory *parent, const char *name, bool create);
void ced_ext_directory_close(CedExtDirectory *dir);
bool ced_ext_directory_names(const CedExtDirectory *dir, char **names, size_t *count, size_t limit);
int ced_ext_file(const CedExtDirectory *dir, const char *name, int flags);
typedef struct { uint64_t size; bool executable; } CedExtFileInfo;
bool ced_ext_file_info(int fd, const char *name, bool private_file, CedExtFileInfo *info);
bool ced_ext_finish_copy(int fd, bool executable);
bool ced_ext_directory_private(const CedExtDirectory *dir);
bool ced_ext_lock(int fd, bool exclusive);
bool ced_ext_mkdtemp(char *path);
int ced_ext_rmdir(const char *path);
bool ced_ext_path_equal(const char *a, const char *b);

#endif
