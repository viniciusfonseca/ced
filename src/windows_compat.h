#ifndef CED_WINDOWS_COMPAT_H
#define CED_WINDOWS_COMPAT_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#endif

void ced_windows_init(void);
wchar_t *ced_windows_wide(const char *text);
char *ced_windows_utf8(const wchar_t *text);
char *ced_windows_realpath(const char *path, char *resolved);
int ced_windows_stat(const char *path, struct stat *st);
int ced_windows_lstat(const char *path, struct stat *st);
int ced_windows_open(const char *path, int flags, ...);
FILE *ced_windows_fopen(const char *path, const char *mode);
FILE *ced_windows_tmpfile(void);
int ced_windows_mkdir(const char *path);
int ced_windows_unlink(const char *path);
int ced_windows_rename(const char *from, const char *to);
int ced_windows_mkstemp(char *path);
int ced_windows_fchmod(int fd, int mode);

#ifndef CED_WINDOWS_IMPLEMENTATION
#define realpath(path, resolved) ced_windows_realpath(path, resolved)
#define stat(path, st) ced_windows_stat(path, st)
#define lstat(path, st) ced_windows_lstat(path, st)
#define open(...) ced_windows_open(__VA_ARGS__)
#define fopen(path, mode) ced_windows_fopen(path, mode)
#define tmpfile() ced_windows_tmpfile()
#define mkdir(path, mode) ced_windows_mkdir(path)
#define unlink(path) ced_windows_unlink(path)
#define rename(from, to) ced_windows_rename(from, to)
#define mkstemp(path) ced_windows_mkstemp(path)
#define fsync(fd) _commit(fd)
#define fchmod(fd, mode) ced_windows_fchmod(fd, mode)
#endif
#endif
#endif
