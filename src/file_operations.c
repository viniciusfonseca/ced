#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "file_operations.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>

static bool fail(char *error, const char *message) {
    snprintf(error, CED_ERROR, "%s", message);
    return false;
}

static bool system_error(char *error, const char *path) {
    snprintf(error, CED_ERROR, "%s: %s", ced_basename(path), strerror(errno));
    return false;
}

bool ced_path_within(const char *path, const char *directory) {
    size_t length = strlen(directory);
    if (!length || !*path) return false;
    if (!strcmp(directory, "/")) return path[0] == '/';
    return !strncmp(path, directory, length) && (!path[length] || path[length] == '/');
}

bool ced_parent_path(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (!slash) return false;
    size_t length = slash == path ? 1 : (size_t)(slash - path);
    if (length >= cap) return false;
    memcpy(out, path, length); out[length] = 0;
    return true;
}

static bool valid_name(const char *name, char *error) {
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || strchr(name, '\n') || strchr(name, '\r'))
        return fail(error, "Enter a file name without path separators.");
    return true;
}

/* Resolve directory components from the workspace descriptor. Never follow
 * symlinks, including links substituted after the tree was scanned. */
static int open_directory(const char *root, const char *path, char *error) {
    if (!ced_path_within(path, root)) { fail(error, "Path is outside the workspace."); return -1; }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { system_error(error, root); return -1; }
    const char *relative = path + strlen(root);
    if (*relative == '/') relative++;
    if (!strcmp(root, "/")) relative = path + 1;
    char parts[CED_PATH];
    if (strlen(relative) >= sizeof parts) { close(fd); fail(error, "Path too long."); return -1; }
    snprintf(parts, sizeof parts, "%s", relative);
    char *state, *part = strtok_r(parts, "/", &state);
    while (part) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); fail(error, "Invalid directory path."); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0) { system_error(error, part); return -1; }
        fd = next;
        part = strtok_r(NULL, "/", &state);
    }
    return fd;
}

static int open_parent(const char *root, const char *path, char *error) {
    char parent[CED_PATH];
    if (!strcmp(root, path) || !ced_parent_path(path, parent, sizeof parent) || !valid_name(ced_basename(path), error)) {
        fail(error, "The workspace root cannot be renamed or deleted."); return -1;
    }
    return open_directory(root, parent, error);
}

bool ced_file_create(const char *root, const char *directory, const char *name, char *created, char *error) {
    if (!valid_name(name, error)) return false;
    if (!ced_join(created, CED_PATH, directory, name)) return fail(error, "Path too long.");
    int parent = open_directory(root, directory, error);
    if (parent < 0) return false;
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) { close(parent); return system_error(error, name); }
    bool ok = close(fd) == 0;
    close(parent);
    return ok || system_error(error, name);
}

bool ced_directory_create(const char *root, const char *directory, const char *name, char *created, char *error) {
    if (!valid_name(name, error)) return false;
    if (!ced_join(created, CED_PATH, directory, name)) return fail(error, "Path too long.");
    int parent = open_directory(root, directory, error);
    if (parent < 0) return false;
    int result = mkdirat(parent, name, 0777);
    int saved_errno = errno;
    close(parent); errno = saved_errno;
    return !result || system_error(error, name);
}

bool ced_file_rename(const char *root, const char *path, const char *name, char *renamed, char *error) {
    if (!valid_name(name, error)) return false;
    char parent_path[CED_PATH];
    if (!ced_parent_path(path, parent_path, sizeof parent_path) || !ced_join(renamed, CED_PATH, parent_path, name))
        return fail(error, "Path too long.");
    int parent = open_parent(root, path, error);
    if (parent < 0) return false;
    const char *old = ced_basename(path);
    struct stat st;
    if (fstatat(parent, old, &st, AT_SYMLINK_NOFOLLOW)) { close(parent); return system_error(error, old); }
    if (!strcmp(old, name)) { close(parent); return true; }
    int result;
#ifdef __APPLE__
    result = renameatx_np(parent, old, parent, name, RENAME_EXCL);
#elif defined(__linux__)
    result = (int)syscall(SYS_renameat2, parent, old, parent, name, 1 /* RENAME_NOREPLACE */);
#else
    errno = ENOTSUP; result = -1;
#endif
    int saved_errno = errno;
    close(parent); errno = saved_errno;
    return !result || system_error(error, name);
}

static bool remove_entry(int parent, const char *name, int depth, char *error) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return system_error(error, name);
    if (!S_ISDIR(st.st_mode)) return !unlinkat(parent, name, 0) || system_error(error, name);
    if (depth >= 128) return fail(error, "Directory nesting limit exceeded.");
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return system_error(error, name);
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return system_error(error, name); }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) ok = system_error(error, name); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!remove_entry(fd, entry->d_name, depth + 1, error)) { ok = false; break; }
    }
    closedir(directory);
    if (ok && unlinkat(parent, name, AT_REMOVEDIR)) ok = system_error(error, name);
    return ok;
}

bool ced_file_delete(const char *root, const char *path, char *error) {
    int parent = open_parent(root, path, error);
    if (parent < 0) return false;
    bool ok = remove_entry(parent, ced_basename(path), 0, error);
    close(parent);
    return ok;
}
