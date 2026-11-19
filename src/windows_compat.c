#define CED_WINDOWS_IMPLEMENTATION
#include "workspace.h"
#include <bcrypt.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>

wchar_t *ced_windows_wide(const char *text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = n ? malloc((size_t)n * sizeof *wide) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n)) { free(wide); wide = NULL; }
    return wide;
}

char *ced_windows_utf8(const wchar_t *text) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    char *utf8 = n ? malloc((size_t)n) : NULL;
    if (utf8 && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8, n, NULL, NULL)) { free(utf8); utf8 = NULL; }
    return utf8;
}

void ced_windows_init(void) {
    _set_fmode(_O_BINARY);
    if (_fileno(stdin)>=0) _setmode(_fileno(stdin),_O_BINARY);
    if (_fileno(stdout)>=0) _setmode(_fileno(stdout),_O_BINARY);
    if (_fileno(stderr)>=0) _setmode(_fileno(stderr),_O_BINARY);
    const char *names[]={"HOME","USERPROFILE","SHELL","CED_CLANGD","COMSPEC"};
    for (size_t i=0;i<sizeof names/sizeof *names;i++) {
        wchar_t *key=ced_windows_wide(names[i]), value[32768];
        DWORD n=GetEnvironmentVariableW(key,value,32768);
        if (!n && !i) n=GetEnvironmentVariableW(L"USERPROFILE",value,32768);
        if (n && n<32768) {
            char *utf8=ced_windows_utf8(value);
            if (utf8) {
                /* Keep CED's narrow environment in UTF-8 and the inherited OS
                 * environment in UTF-16, independent of the system ANSI page. */
                _putenv_s(names[i],utf8); SetEnvironmentVariableW(key,value); free(utf8);
            }
        }
        free(key);
    }
}

char *ced_windows_realpath(const char *path, char *resolved) {
    wchar_t *wide = ced_windows_wide(path), final[32768];
    HANDLE file = wide ? CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL) : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) { errno = ENOENT; return NULL; }
    DWORD n = GetFinalPathNameByHandleW(file, final, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (!n || n >= 32768) { errno = ENAMETOOLONG; return NULL; }
    wchar_t *start = final;
    if (!wcsncmp(final, L"\\\\?\\UNC\\", 8)) { start = final + 6; start[0] = L'\\'; }
    else if (!wcsncmp(final, L"\\\\?\\", 4)) start += 4;
    char *utf8 = ced_windows_utf8(start);
    if (!utf8 || strlen(utf8) >= CED_PATH) { free(utf8); errno = ENAMETOOLONG; return NULL; }
    for (char *p = utf8; *p; p++) if (*p == '\\') *p = '/';
    if (!resolved) return utf8;
    strcpy(resolved, utf8); free(utf8); return resolved;
}

int ced_windows_stat(const char *path, struct stat *st) {
    wchar_t *wide = ced_windows_wide(path);
    if (!wide) { errno = EINVAL; return -1; }
    struct _stat64 native;
    int result = _wstat64(wide, &native); free(wide);
    if (!result) {
        memset(st, 0, sizeof *st);
        st->st_mode=native.st_mode; st->st_size=native.st_size;
        st->st_dev=native.st_dev; st->st_ino=native.st_ino; st->st_nlink=native.st_nlink;
        st->st_atime=native.st_atime; st->st_mtime=native.st_mtime; st->st_ctime=native.st_ctime;
    }
    return result;
}

int ced_windows_lstat(const char *path, struct stat *st) {
    wchar_t *wide = ced_windows_wide(path);
    DWORD attrs = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) { errno = ENOENT; return -1; }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) { memset(st, 0, sizeof *st); st->st_mode = S_IFLNK; return 0; }
    return ced_windows_stat(path, st);
}

int ced_windows_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    wchar_t *wide = ced_windows_wide(path);
    if (!wide) { errno = EINVAL; return -1; }
    int fd = _wopen(wide, flags | _O_BINARY, mode); free(wide); return fd;
}

FILE *ced_windows_fopen(const char *path, const char *mode) {
    wchar_t *wide = ced_windows_wide(path), *format = ced_windows_wide(mode);
    FILE *file = wide && format ? _wfopen(wide, format) : NULL;
    free(wide); free(format); return file;
}

FILE *ced_windows_tmpfile(void) {
    wchar_t directory[MAX_PATH], path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, directory);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(directory, L"ced", 0, path)) return NULL;
    int fd = _wopen(path, _O_BINARY | _O_RDWR | _O_TEMPORARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
    if (fd < 0) { DeleteFileW(path); return NULL; }
    FILE *file = _fdopen(fd, "w+b");
    if (!file) _close(fd);
    return file;
}

int ced_windows_mkdir(const char *path) {
    wchar_t *wide = ced_windows_wide(path);
    int result = wide ? _wmkdir(wide) : -1; free(wide); return result;
}

int ced_windows_unlink(const char *path) {
    wchar_t *wide = ced_windows_wide(path);
    int result = wide ? _wunlink(wide) : -1; free(wide); return result;
}

int ced_windows_rename(const char *from, const char *to) {
    wchar_t *a = ced_windows_wide(from), *b = ced_windows_wide(to);
    bool ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a); free(b); if (!ok) errno = EACCES; return ok ? 0 : -1;
}

int ced_windows_mkstemp(char *path) {
    size_t length = strlen(path);
    if (length < 6 || strcmp(path + length - 6, "XXXXXX")) { errno = EINVAL; return -1; }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 64; attempt++) {
        unsigned char random[6];
        if (BCryptGenRandom(NULL, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) break;
        for (size_t i = 0; i < 6; i++) path[length - 6 + i] = chars[random[i] % (sizeof chars - 1)];
        int fd = ced_windows_open(path, O_CREAT | O_EXCL | O_RDWR | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST; return -1;
}

int ced_windows_fchmod(int fd, int mode) {
    HANDLE handle = (HANDLE)_get_osfhandle(fd); FILE_BASIC_INFO info;
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof info)) return -1;
    if (mode & _S_IWRITE) info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    else info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    return SetFileInformationByHandle(handle, FileBasicInfo, &info, sizeof info) ? 0 : -1;
}
