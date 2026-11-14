#ifndef CED_FILE_OPERATIONS_H
#define CED_FILE_OPERATIONS_H

#include "workspace.h"

bool ced_path_within(const char *path, const char *directory);
bool ced_parent_path(const char *path, char *out, size_t cap);
bool ced_file_create(const char *root, const char *directory, const char *name, char *created, char *error);
bool ced_directory_create(const char *root, const char *directory, const char *name, char *created, char *error);
bool ced_file_rename(const char *root, const char *path, const char *name, char *renamed, char *error);
bool ced_file_delete(const char *root, const char *path, char *error);

#endif
