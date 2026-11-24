#include "files.h"
#include "file_operations.h"
#include <aclapi.h>
#include <bcrypt.h>
#include <errno.h>
#include <wchar.h>
#include <string.h>

static bool file_error(DWORD status) {
    errno = status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return false;
}

static bool component(const char *name) {
    size_t n = strlen(name);
    if (!n || name[n - 1] == '.' || name[n - 1] == ' ' || strpbrk(name, "/\\:<>\"|?*")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 32) return false;
    size_t base = strcspn(name, ".");
    while (base && name[base - 1] == ' ') base--;
    if (base == 3 && (!_strnicmp(name,"CON",3) || !_strnicmp(name,"PRN",3) ||
                      !_strnicmp(name,"AUX",3) || !_strnicmp(name,"NUL",3))) return false;
    if (base > 3 && (!_strnicmp(name,"COM",3) || !_strnicmp(name,"LPT",3))) {
        const char *suffix = name + 3;
        if ((base == 4 && *suffix >= '0' && *suffix <= '9') ||
            (base == 5 && (!memcmp(suffix,"\xc2\xb9",2) || !memcmp(suffix,"\xc2\xb2",2) || !memcmp(suffix,"\xc2\xb3",2)))) return false;
    }
    return true;
}

static bool plain(HANDLE handle, bool directory) {
    FILE_ATTRIBUTE_TAG_INFO info;
    return GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof info) &&
        !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

static bool directory_handle(CedExtDirectory *dir, const char *path) {
    wchar_t *wide = ced_windows_wide(path);
    if (!wide) return file_error(ERROR_INVALID_NAME);
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(wide);
        return file_error(ERROR_ACCESS_DENIED);
    }
    HANDLE handle = CreateFileW(wide, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD status = GetLastError();
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return file_error(status);
    if (!plain(handle, true)) { CloseHandle(handle); return file_error(ERROR_ACCESS_DENIED); }
    dir->handle = handle;
    strcpy(dir->path, path);
    return true;
}

/* Keep all opened parents stable while traversing children. For an absolute
 * starting directory also reject junctions or symlinks in its ancestry. */
bool ced_ext_directory(CedExtDirectory *dir, const CedExtDirectory *parent, const char *name, bool create) {
    char path[CED_PATH];
    if (parent) {
        if (!component(name) || !ced_join(path, sizeof path, parent->path, name)) return file_error(ERROR_INVALID_NAME);
    } else {
        wchar_t *wide = ced_windows_wide(name), absolute[CED_PATH];
        DWORD length = wide ? GetFullPathNameW(wide, CED_PATH, absolute, NULL) : 0;
        free(wide);
        if (!length || length >= CED_PATH || !wcsncmp(absolute, L"\\\\?\\", 4) || !wcsncmp(absolute, L"\\\\.\\", 4))
            return file_error(ERROR_INVALID_NAME);
        char *utf8 = ced_windows_utf8(absolute);
        if (!utf8 || strlen(utf8) >= sizeof path) { free(utf8); return file_error(ERROR_INVALID_NAME); }
        strcpy(path, utf8); free(utf8);
        for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    }
    if (create && mkdir(path, 0700) && errno != EEXIST) return false;
    if (!directory_handle(dir, path)) return false;
    if (!parent) {
        char canonical[CED_PATH];
        if (!realpath(path, canonical) || !ced_ext_path_equal(path, canonical)) {
            ced_ext_directory_close(dir);
            return file_error(ERROR_ACCESS_DENIED);
        }
    }
    return true;
}

void ced_ext_directory_close(CedExtDirectory *dir) { CloseHandle(dir->handle); }

bool ced_ext_directory_names(const CedExtDirectory *dir, char **names, size_t *count, size_t limit) {
    BYTE *buffer = malloc(16384);
    if (!buffer) return false;
    bool first = true, ok = true;
    while (ok) {
        if (!GetFileInformationByHandleEx(dir->handle, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buffer, 16384)) {
            ok = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
        first = false;
        FILE_ID_BOTH_DIR_INFO *entry = (FILE_ID_BOTH_DIR_INFO *)buffer;
        for (;;) {
            wchar_t name[256];
            size_t length = entry->FileNameLength / sizeof(wchar_t);
            if (length >= sizeof name / sizeof *name) { ok = false; break; }
            memcpy(name, entry->FileName, entry->FileNameLength); name[length] = 0;
            if (wcscmp(name,L".") && wcscmp(name,L"..")) {
                if (*count == limit || (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ok = false; break; }
                char *utf8 = ced_windows_utf8(name);
                if (!utf8 || !component(utf8)) { free(utf8); ok = false; break; }
                names[(*count)++] = utf8;
            }
            if (!entry->NextEntryOffset) break;
            entry = (FILE_ID_BOTH_DIR_INFO *)((BYTE *)entry + entry->NextEntryOffset);
        }
    }
    free(buffer);
    return ok;
}

int ced_ext_file(const CedExtDirectory *dir, const char *name, int flags) {
    char path[CED_PATH];
    if (!dir || !component(name) || !ced_join(path, sizeof path, dir->path, name)) { errno = EINVAL; return -1; }
    wchar_t *wide = ced_windows_wide(path);
    if (!wide) { errno = EINVAL; return -1; }
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(wide);
        errno = EACCES;
        return -1;
    }
    bool writable = (flags & (O_WRONLY | O_RDWR)) != 0;
    DWORD disposition = flags & O_EXCL ? CREATE_NEW : flags & O_TRUNC ? CREATE_ALWAYS :
        flags & O_CREAT ? OPEN_ALWAYS : OPEN_EXISTING;
    /* Shared writable handles are only used for the lock files. */
    DWORD sharing = FILE_SHARE_READ | ((flags & O_RDWR) ? FILE_SHARE_WRITE : 0);
    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
    HANDLE handle = CreateFileW(wide, access, sharing, NULL, disposition,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD status = GetLastError();
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) { file_error(status); return -1; }
    if (!plain(handle, false)) { CloseHandle(handle); errno = EACCES; return -1; }
    int fd = _open_osfhandle((intptr_t)handle, flags | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) CloseHandle(handle);
    return fd;
}

static bool private_handle(HANDLE handle) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = malloc(size);
    bool ok = user && GetTokenInformation(token, TokenUser, user, size, &size);
    CloseHandle(token);
    PSID owner = NULL;
    PACL acl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    ok = ok && GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                               &owner, NULL, &acl, NULL, &descriptor) == ERROR_SUCCESS &&
        owner && EqualSid(owner, user->User.Sid) && acl;
    const DWORD changes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
    for (DWORD i = 0; ok && i < acl->AceCount; i++) {
        void *entry;
        if (!GetAce(acl, i, &entry)) { ok = false; break; }
        ACE_HEADER *header = entry;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        ACCESS_ALLOWED_ACE *ace = entry;
        PSID sid = &ace->SidStart;
        if ((ace->Mask & changes) && !EqualSid(sid, user->User.Sid) &&
            !((header->AceFlags & INHERIT_ONLY_ACE) && IsWellKnownSid(sid, WinCreatorOwnerSid)) &&
            !IsWellKnownSid(sid, WinLocalSystemSid) && !IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) ok = false;
    }
    if (descriptor) LocalFree(descriptor);
    free(user);
    return ok;
}

bool ced_ext_file_info(int fd, const char *name, bool private_file, CedExtFileInfo *info) {
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || (private_file && !private_handle(handle))) return false;
    const char *suffix = strrchr(name, '.');
    *info = (CedExtFileInfo){(uint64_t)size.QuadPart, suffix && !_stricmp(suffix, ".exe")};
    return true;
}

/* The profile ACL protects the copy. Keeping DOS read-only off lets the
 * regular Windows file removal path delete an uninstalled package. */
bool ced_ext_finish_copy(int fd, bool executable) { (void)executable; return !fsync(fd); }

bool ced_ext_directory_private(const CedExtDirectory *dir) { return private_handle(dir->handle); }

bool ced_ext_lock(int fd, bool exclusive) {
    OVERLAPPED offset = {0};
    return LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_FAIL_IMMEDIATELY |
        (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &offset) != 0;
}

bool ced_ext_mkdtemp(char *path) {
    size_t length = strlen(path);
    if (length < 6 || strcmp(path + length - 6, "XXXXXX")) return false;
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        unsigned char bytes[6];
        if (BCryptGenRandom(NULL, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return false;
        for (size_t i = 0; i < sizeof bytes; i++) path[length - 6 + i] = alphabet[bytes[i] % (sizeof alphabet - 1)];
        if (!mkdir(path, 0700)) return true;
        if (errno != EEXIST) break;
    }
    return false;
}

int ced_ext_rmdir(const char *path) {
    wchar_t *wide = ced_windows_wide(path);
    int result = wide ? _wrmdir(wide) : -1;
    free(wide);
    return result;
}

bool ced_ext_path_equal(const char *a, const char *b) {
    wchar_t *left = ced_windows_wide(a), *right = ced_windows_wide(b);
    bool equal = left && right && CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
    free(left); free(right);
    return equal;
}
