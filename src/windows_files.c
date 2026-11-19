#include "file_operations.h"
#include <stddef.h>
#include <string.h>

static bool failure(char *error, const char *message) { snprintf(error, CED_ERROR, "%s", message); return false; }
static bool system_error(char *error) { snprintf(error, CED_ERROR, "File operation failed (Windows error %lu).", GetLastError()); return false; }

bool ced_path_within(const char *path, const char *directory) {
    size_t n = strlen(directory);
    return n && !_strnicmp(path, directory, n) && (!path[n] || path[n]=='/' || directory[n-1]=='/');
}

bool ced_parent_path(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/'); if (!slash) return false;
    size_t n = (size_t)(slash-path);
    if (!n || (n==2 && path[1]==':')) n++;
    if (n>=cap) return false;
    memcpy(out,path,n); out[n]=0; return true;
}

static bool valid_name(const char *name, char *error) {
    size_t n = strlen(name);
    if (!n || n>255 || strpbrk(name,"/\\:<>\"|?*\r\n") || name[n-1]=='.' || name[n-1]==' ')
        return failure(error,"Enter a valid Windows file name without path separators.");
    for (const unsigned char *p=(const unsigned char *)name; *p; p++) if (*p<32) return failure(error,"Invalid file name.");
    char base[256]; snprintf(base,sizeof base,"%s",name); base[strcspn(base,".")]=0;
    if (!_stricmp(base,"CON") || !_stricmp(base,"PRN") || !_stricmp(base,"AUX") || !_stricmp(base,"NUL") ||
        (strlen(base)==4 && (!_strnicmp(base,"COM",3) || !_strnicmp(base,"LPT",3)) && base[3]>='1' && base[3]<='9'))
        return failure(error,"This file name is reserved by Windows.");
    return true;
}

typedef struct { HANDLE handles[130]; int count; } DirectoryLock;
static void unlock(DirectoryLock *lock) { while (lock->count) CloseHandle(lock->handles[--lock->count]); }

/* Hold ancestors without FILE_SHARE_DELETE so paths cannot be renamed or
 * replaced with junctions while a mutation is in progress. Reject reparse points. */
static bool lock_directory(const char *root, const char *path, DirectoryLock *lock, char *error) {
    char canonical[CED_PATH];
    if (!ced_path_within(path,root) || !realpath(root,canonical) || _stricmp(canonical,root))
        return failure(error,"Path is outside the workspace.");
    char part[CED_PATH]; snprintf(part,sizeof part,"%s",root);
    size_t root_length=strlen(root), length=strlen(path);
    for (size_t end=root_length;;) {
        wchar_t *wide=ced_windows_wide(part);
        HANDLE handle=wide ? CreateFileW(wide,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL) : INVALID_HANDLE_VALUE;
        free(wide); FILE_ATTRIBUTE_TAG_INFO info;
        if (handle==INVALID_HANDLE_VALUE) { unlock(lock); return system_error(error); }
        bool valid=GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&info,sizeof info) &&
            (info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
        if (!valid || lock->count==130) { CloseHandle(handle); unlock(lock); return failure(error,"Unsafe directory or nesting limit exceeded."); }
        lock->handles[lock->count++]=handle;
        if (end==length) return true;
        size_t start=end; if (path[start]=='/') start++;
        end=start; while (end<length && path[end]!='/') end++;
        if (end==start || (end-start==1 && path[start]=='.') || (end-start==2 && !memcmp(path+start,"..",2))) {
            unlock(lock); return failure(error,"Invalid directory path.");
        }
        memcpy(part,path,end); part[end]=0;
    }
}

static bool destination(const char *root, const char *directory, const char *name, char *path, DirectoryLock *lock, char *error) {
    return valid_name(name,error) && (ced_join(path,CED_PATH,directory,name) || failure(error,"Path too long.")) &&
        lock_directory(root,directory,lock,error);
}

bool ced_file_create(const char *root, const char *directory, const char *name, char *created, char *error) {
    DirectoryLock lock={0}; if (!destination(root,directory,name,created,&lock,error)) return false;
    wchar_t *wide=ced_windows_wide(created);
    HANDLE file=wide ? CreateFileW(wide,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL) : INVALID_HANDLE_VALUE;
    free(wide); bool ok=file!=INVALID_HANDLE_VALUE;
    if (ok) CloseHandle(file); else system_error(error);
    unlock(&lock); return ok;
}

bool ced_directory_create(const char *root, const char *directory, const char *name, char *created, char *error) {
    DirectoryLock lock={0}; if (!destination(root,directory,name,created,&lock,error)) return false;
    wchar_t *wide=ced_windows_wide(created); bool ok=wide && CreateDirectoryW(wide,NULL); free(wide);
    if (!ok) system_error(error);
    unlock(&lock); return ok;
}

static bool lock_parent(const char *root, const char *path, DirectoryLock *lock, char *error) {
    char parent[CED_PATH];
    if (!_stricmp(root,path) || !ced_parent_path(path,parent,sizeof parent)) return failure(error,"The workspace root cannot be renamed or deleted.");
    return lock_directory(root,parent,lock,error);
}

bool ced_file_rename(const char *root, const char *path, const char *name, char *renamed, char *error) {
    DirectoryLock lock={0}; char parent[CED_PATH];
    if (!valid_name(name,error) || !ced_parent_path(path,parent,sizeof parent) || !ced_join(renamed,CED_PATH,parent,name) ||
        !lock_parent(root,path,&lock,error)) return false;
    if (!strcmp(path,renamed)) { unlock(&lock); return true; }
    wchar_t *from=ced_windows_wide(path), *to=ced_windows_wide(renamed);
    bool ok=from && to && MoveFileExW(from,to,MOVEFILE_WRITE_THROUGH);
    free(from); free(to); if (!ok) system_error(error); unlock(&lock); return ok;
}

static bool remove_entry(const char *path, int depth, char *error) {
    if (depth>=128) return failure(error,"Directory nesting limit exceeded.");
    wchar_t *wide=ced_windows_wide(path);
    HANDLE handle=wide ? CreateFileW(wide,DELETE|FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL) : INVALID_HANDLE_VALUE;
    free(wide); FILE_ATTRIBUTE_TAG_INFO info;
    if (handle==INVALID_HANDLE_VALUE) return system_error(error);
    bool ok=GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&info,sizeof info);
    if (ok && (info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        /* Enumerate through the same locked handle: FindFirstFile would open a
         * second directory handle whose sharing flags conflict with DELETE. */
        BYTE *buffer=malloc(16384); bool first=true;
        ok=buffer!=NULL;
        if (!ok) failure(error,"Out of memory.");
        while (ok) {
            FILE_INFO_BY_HANDLE_CLASS query=first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
            if (!GetFileInformationByHandleEx(handle,query,buffer,16384)) {
                if (GetLastError()!=ERROR_NO_MORE_FILES) ok=system_error(error);
                break;
            }
            first=false;
            FILE_ID_BOTH_DIR_INFO *entry=(FILE_ID_BOTH_DIR_INFO *)buffer;
            for (;;) {
                wchar_t name[256]; size_t length=entry->FileNameLength/sizeof(wchar_t);
                if (length>=256) { ok=failure(error,"File name is too long."); break; }
                memcpy(name,entry->FileName,entry->FileNameLength); name[length]=0;
                if (wcscmp(name,L".") && wcscmp(name,L"..")) {
                    char *utf8=ced_windows_utf8(name), child[CED_PATH];
                    ok=utf8 && ced_join(child,sizeof child,path,utf8) && remove_entry(child,depth+1,error); free(utf8);
                    if (!ok) break;
                }
                if (!entry->NextEntryOffset) break;
                entry=(FILE_ID_BOTH_DIR_INFO *)((BYTE *)entry+entry->NextEntryOffset);
            }
        }
        free(buffer);
    }
    if (ok) { FILE_DISPOSITION_INFO disposition={TRUE}; ok=SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof disposition); if (!ok) system_error(error); }
    else if (!*error) system_error(error);
    CloseHandle(handle); return ok;
}

bool ced_file_delete(const char *root, const char *path, char *error) {
    DirectoryLock lock={0}; *error=0;
    if (!lock_parent(root,path,&lock,error)) return false;
    bool ok=remove_entry(path,0,error); unlock(&lock); return ok;
}
