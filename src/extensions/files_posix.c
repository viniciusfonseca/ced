#include "files.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

bool ced_ext_directory(CedExtDirectory *dir, const CedExtDirectory *parent, const char *name, bool create) {
    int at = parent ? parent->fd : AT_FDCWD;
    if (create && mkdirat(at, name, 0700) && errno != EEXIST) return false;
    dir->fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return dir->fd >= 0;
}

void ced_ext_directory_close(CedExtDirectory *dir) { close(dir->fd); }

bool ced_ext_directory_names(const CedExtDirectory *dir, char **names, size_t *count, size_t limit) {
    int fd = openat(dir->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    DIR *stream = fdopendir(fd);
    if (!stream) { close(fd); return false; }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(stream);
        if (!entry) { ok = !errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (*count == limit || !(names[*count] = strdup(entry->d_name))) { ok = false; break; }
        ++*count;
    }
    closedir(stream);
    return ok;
}

int ced_ext_file(const CedExtDirectory *dir, const char *name, int flags) {
    int at = dir ? dir->fd : AT_FDCWD;
    struct stat expected;
    bool existing = !fstatat(at, name, &expected, AT_SYMLINK_NOFOLLOW);
    if (existing && !S_ISREG(expected.st_mode)) { errno = EACCES; return -1; }
    if (!existing && (errno != ENOENT || !(flags & O_CREAT))) return -1;
    int fd = openat(at, name, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    struct stat opened;
    if (fd >= 0 && (fstat(fd, &opened) || !S_ISREG(opened.st_mode) ||
        (existing && (opened.st_dev != expected.st_dev || opened.st_ino != expected.st_ino)))) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

bool ced_ext_file_info(int fd, const char *name, bool private_file, CedExtFileInfo *info) {
    (void)name;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (private_file && (st.st_uid != geteuid() || (st.st_mode & 0022)))) return false;
    *info = (CedExtFileInfo){(uint64_t)st.st_size, (st.st_mode & 0111) != 0};
    return true;
}

bool ced_ext_finish_copy(int fd, bool executable) {
    return !fchmod(fd, executable ? 0555 : 0444) && !fsync(fd);
}

bool ced_ext_directory_private(const CedExtDirectory *dir) {
    struct stat st;
    return !fstat(dir->fd, &st) && st.st_uid == geteuid() && !(st.st_mode & 0022);
}

bool ced_ext_lock(int fd, bool exclusive) { return !flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB); }
bool ced_ext_mkdtemp(char *path) { return mkdtemp(path) != NULL; }
int ced_ext_rmdir(const char *path) { return rmdir(path); }
bool ced_ext_path_equal(const char *a, const char *b) { return !strcmp(a, b); }
