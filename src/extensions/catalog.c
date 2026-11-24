#include "catalog.h"
#include "sha256.h"
#include "file_operations.h"
#include "files.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool failure(char *error, const char *message) { snprintf(error, CED_ERROR, "%s", message); return false; }
static bool join(char *out, const char *root, const char *name, char *error) {
    return ced_join(out, CED_PATH, root, name) || failure(error, "Extension path is too long");
}

static bool directory(CedExtDirectory *dir, const char *path, bool create, char *error) {
    return ced_ext_directory(dir, NULL, path, create) ||
        failure(error, "Could not open a regular extension directory");
}

static bool write_all(int fd, const void *data, size_t length) {
    const char *bytes = data;
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n; length -= (size_t)n;
    }
    return true;
}

static int name_order(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { size_t bytes, files; CedSha256 hash; char *manifest; size_t manifest_length; unsigned char buffer[16384]; } PackageScan;

/* Directory descriptors remain open throughout traversal. Files are copied
 * from those descriptors, never reopened through an untrusted absolute path. */
static bool package_walk(const CedExtDirectory *source, const CedExtDirectory *destination,
                         const char *relative, unsigned depth, PackageScan *scan, char *error) {
    if (depth > 32) return failure(error, "Package directory depth exceeded");
    size_t limit = CED_EXTENSION_FILE_LIMIT - scan->files;
    char **names = calloc(limit + 1, sizeof *names);
    size_t count = 0;
    bool ok = names && ced_ext_directory_names(source, names, &count, limit);
    scan->files += count;
    if (ok) qsort(names, count, sizeof *names, name_order);
    for (size_t i = 0; ok && i < count; i++) {
        char path[CED_PATH];
        const char *name = names[i];
        int n = snprintf(path, sizeof path, "%s%s%s", relative, *relative ? "/" : "", name);
        if (n < 0 || (size_t)n >= sizeof path) { ok = false; break; }
        CedExtDirectory child, copy_dir;
        if (ced_ext_directory(&child, source, name, false)) {
            unsigned char kind = 'd';
            ced_sha256_update(&scan->hash, &kind, 1);
            ced_sha256_update(&scan->hash, path, strlen(path) + 1);
            bool copying = destination && ced_ext_directory(&copy_dir, destination, name, true);
            ok = (!destination || copying) && package_walk(&child, copying ? &copy_dir : NULL, path, depth + 1, scan, error);
            if (copying) ced_ext_directory_close(&copy_dir);
            ced_ext_directory_close(&child);
            continue;
        }

        int file = ced_ext_file(source, name, O_RDONLY), copy = -1;
        CedExtFileInfo info;
        ok = file >= 0 && ced_ext_file_info(file, name, false, &info) &&
            info.size <= CED_EXTENSION_PACKAGE_LIMIT - scan->bytes;
        bool manifest = !strcmp(path, "ced-extension.json");
        if (ok) {
            unsigned char kind = info.executable ? 'x' : 'f';
            unsigned char file_length[8];
            for (unsigned b = 0; b < 8; b++) file_length[b] = (unsigned char)(info.size >> (56 - b * 8));
            ced_sha256_update(&scan->hash, &kind, 1);
            ced_sha256_update(&scan->hash, path, strlen(path) + 1);
            ced_sha256_update(&scan->hash, file_length, sizeof file_length);
            if (destination) {
                copy = ced_ext_file(destination, name, O_WRONLY | O_CREAT | O_EXCL);
                ok = copy >= 0;
            }
            if (ok && manifest) {
                if (!info.size || info.size > CED_EXTENSION_MANIFEST_LIMIT) ok = false;
                else { scan->manifest = malloc((size_t)info.size + 1); if (!scan->manifest) ok = false; }
            }
        }
        size_t read_bytes = 0;
        while (ok) {
            ssize_t size = read(file, scan->buffer, sizeof scan->buffer);
            if (size < 0 && errno == EINTR) continue;
            if (size < 0) { ok = false; break; }
            if (!size) break;
            if ((size_t)size > (size_t)info.size - read_bytes) { ok = false; break; }
            if (manifest) memcpy(scan->manifest + read_bytes, scan->buffer, (size_t)size);
            scan->bytes += (size_t)size; read_bytes += (size_t)size;
            ced_sha256_update(&scan->hash, scan->buffer, (size_t)size);
            if (copy >= 0 && !write_all(copy, scan->buffer, (size_t)size)) ok = false;
        }
        if (ok && read_bytes != (size_t)info.size) ok = false;
        if (ok && manifest) { scan->manifest_length = read_bytes; scan->manifest[read_bytes] = 0; }
        if (file >= 0) close(file);
        if (copy >= 0) {
            if (ok && !ced_ext_finish_copy(copy, info.executable)) ok = false;
            if (close(copy)) ok = false;
        }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    if (!ok && !*error) failure(error, "Package changed, contains links/special files, exceeds limits, or cannot be copied");
    return ok;
}

static bool package_scan(const char *source, const char *destination, char hash[65], CedExtensionManifest *manifest, char *error) {
    CedExtDirectory from, to;
    if (!directory(&from, source, false, error)) return false;
    if (destination && !directory(&to, destination, false, error)) { ced_ext_directory_close(&from); return false; }
    PackageScan scan = {0}; ced_sha256_init(&scan.hash);
    bool ok = package_walk(&from, destination ? &to : NULL, "", 0, &scan, error);
    if (ok && manifest) {
        if (!scan.manifest) ok = failure(error, "Missing ced-extension.json");
        else ok = ced_extension_manifest_parse(scan.manifest, scan.manifest_length, manifest, error);
    }
    free(scan.manifest);
    ced_ext_directory_close(&from);
    if (destination) ced_ext_directory_close(&to);
    if (ok) ced_sha256_finish(&scan.hash, hash);
    return ok;
}

static bool entrypoint_valid(const CedExtDirectory *dir, char *path) {
    char *tail = strchr(path, '/');
    if (!tail) {
        int fd = ced_ext_file(dir, path, O_RDONLY);
        if (fd < 0) return false;
        CedExtFileInfo info;
        bool ok = ced_ext_file_info(fd, path, false, &info) && info.executable;
        close(fd);
        return ok;
    }
    *tail++ = 0;
    CedExtDirectory child;
    if (!ced_ext_directory(&child, dir, path, false)) return false;
    bool ok = entrypoint_valid(&child, tail);
    ced_ext_directory_close(&child);
    return ok;
}

bool ced_extension_inspect(const char *source, CedExtensionRecord *record, char *error) {
    memset(record, 0, sizeof *record); *error = 0;
    CedExtDirectory dir;
    if (!directory(&dir, source, false, error)) return false;
    bool ok = realpath(source, record->source) && package_scan(record->source, NULL, record->hash, &record->manifest, error);
    if (ok && record->manifest.executable) {
        char path[CED_PATH];
        strcpy(path, record->manifest.entrypoint);
        ok = entrypoint_valid(&dir, path) || failure(error, "Entrypoint must be a regular executable file");
    }
    ced_ext_directory_close(&dir);
    if (!ok) { ced_extension_manifest_free(&record->manifest); return false; }
    record->valid = true; record->granted = record->manifest.capabilities;
    return true;
}

static cJSON *load_profile(const char *profile, char *error) {
    CedExtDirectory dir;
    bool opened = ced_ext_directory(&dir, NULL, profile, false);
    if (opened && !ced_ext_directory_private(&dir)) {
        ced_ext_directory_close(&dir); failure(error, "Unsafe extension profile directory"); return NULL;
    }
    int fd = opened ? ced_ext_file(&dir, "extensions.json", O_RDONLY) : -1;
    int open_error = errno;
    if (opened) ced_ext_directory_close(&dir);
    errno = open_error;
    if (fd < 0 && errno == ENOENT) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "formatVersion", 1);
        cJSON_AddArrayToObject(root, "extensions"); cJSON_AddArrayToObject(root, "trustedWorkspaces");
        return root;
    }
    CedExtFileInfo info;
    if (fd < 0 || !ced_ext_file_info(fd, "extensions.json", true, &info) || info.size > CED_EXTENSION_MANIFEST_LIMIT) {
        if (fd >= 0) close(fd); failure(error, "Invalid extensions.json"); return NULL;
    }
    size_t used = 0, size = (size_t)info.size;
    char *bytes = malloc(size + 1);
    if (!bytes) { close(fd); return NULL; }
    while (used < size) {
        ssize_t n = read(fd, bytes + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd); bytes[used] = 0;
    cJSON *root = used == size ? ced_ext_json_parse(bytes, size, 32, error) : NULL;
    free(bytes);
    static const char *const fields[] = {"formatVersion", "extensions", "trustedWorkspaces", NULL};
    uint64_t version;
    if (!root || !ced_ext_json_fields(root, fields, error) ||
        !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(root, "formatVersion"), &version) || version != 1 ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "extensions")) ||
        cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "extensions")) > CED_EXTENSION_LIMIT ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "trustedWorkspaces"))) {
        cJSON_Delete(root); failure(error, "Invalid extension profile; the original file was preserved"); return NULL;
    }
    return root;
}

static bool save_profile(const char *profile, const cJSON *root, char *error) {
    char path[CED_PATH], temp[CED_PATH];
    if (!join(path, profile, "extensions.json", error) || !join(temp, profile, ".extensions-XXXXXX", error)) return false;
    char *bytes = cJSON_PrintUnformatted(root);
    if (!bytes) return failure(error, "Out of memory");
    int fd = mkstemp(temp);
    bool ok = fd >= 0 && write_all(fd, bytes, strlen(bytes)) && !fsync(fd);
    cJSON_free(bytes);
    if (fd >= 0 && close(fd)) ok = false;
    if (ok && rename(temp, path)) ok = false;
    if (!ok) { unlink(temp); return failure(error, "Could not atomically write extensions.json"); }
    return true;
}

static int profile_lock(const char *profile, char *error) {
    CedExtDirectory dir;
    if (!directory(&dir, profile, true, error)) return -1;
    if (!ced_ext_directory_private(&dir)) {
        ced_ext_directory_close(&dir); failure(error, "The extension profile must be owned by the current user and not writable by others"); return -1;
    }
    int lock = ced_ext_file(&dir, ".extensions.lock", O_RDWR | O_CREAT);
    ced_ext_directory_close(&dir);
    CedExtFileInfo info;
    if (lock < 0 || !ced_ext_file_info(lock, ".extensions.lock", true, &info) || !ced_ext_lock(lock, true)) {
        if (lock >= 0) close(lock);
        failure(error, "Conflict: extension profile is busy or its lock is unsafe"); return -1;
    }
    return lock;
}

static cJSON *profile_entry(cJSON *root, const char *id, bool create) {
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "extensions");
    for (cJSON *entry = entries->child; entry; entry = entry->next) {
        const char *other = ced_ext_json_string(entry, "id");
        if (other && !strcmp(id, other)) return entry;
    }
    if (!create || cJSON_GetArraySize(entries) == CED_EXTENSION_LIMIT) return NULL;
    cJSON *entry = cJSON_CreateObject();
    if (!cJSON_AddItemToArray(entries, entry)) { cJSON_Delete(entry); return NULL; }
    cJSON_AddStringToObject(entry, "id", id);
    return entry;
}

bool ced_extension_catalog_load(const char *profile, const char *workspace, CedExtensionCatalog *catalog, char *error) {
    memset(catalog, 0, sizeof *catalog); *error = 0;
    cJSON *root = load_profile(profile, error);
    if (!root) return false;
    char canonical[CED_PATH];
    if (!realpath(workspace, canonical)) { cJSON_Delete(root); return failure(error, "Workspace is unavailable"); }
    cJSON *trusted = cJSON_GetObjectItemCaseSensitive(root, "trustedWorkspaces");
    for (const cJSON *path = trusted->child; path; path = path->next)
        if (cJSON_IsString(path) && ced_ext_path_equal(path->valuestring, canonical)) catalog->workspace_trusted = true;
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "extensions");
    static const char *const fields[] = {"id", "version", "hash", "source", "enabled", "granted", NULL};
    for (const cJSON *entry = entries->child; entry; entry = entry->next) {
        const char *id = ced_ext_json_string(entry, "id"), *version = ced_ext_json_string(entry, "version");
        const char *hash = ced_ext_json_string(entry, "hash"), *source = ced_ext_json_string(entry, "source");
        uint64_t granted;
        if (!ced_ext_json_fields(entry, fields, error) || !ced_extension_id_valid(id) || !ced_extension_version_valid(version) ||
            !hash || strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 || !source || strlen(source) >= CED_PATH ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(entry, "enabled")) ||
            !ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(entry, "granted"), &granted) || granted > 3 ||
            ((granted & CED_EXT_EDIT) && !(granted & CED_EXT_READ))) {
            failure(error, "Invalid extension profile entry"); goto invalid;
        }
        for (size_t i = 0; i < catalog->count; i++) if (!strcmp(catalog->entries[i].manifest.id, id)) {
            failure(error, "Duplicate extension profile entry"); goto invalid;
        }
        CedExtensionRecord *record = &catalog->entries[catalog->count++];
        char relative[256], package[CED_PATH];
        snprintf(relative, sizeof relative, "extensions/%s/%s", id, version);
        if (!join(package, profile, relative, error)) goto invalid;
        char diagnostic[CED_ERROR] = {0};
        bool ok = ced_extension_inspect(package, record, diagnostic);
        if (ok && (strcmp(id, record->manifest.id) || strcmp(version, record->manifest.version) || strcmp(hash, record->hash) ||
            (granted & ~record->manifest.capabilities))) { ok = false; snprintf(diagnostic, sizeof diagnostic, "Installed package no longer matches its approval"); }
        snprintf(record->package, sizeof record->package, "%s", package);
        snprintf(record->source, sizeof record->source, "%s", source);
        record->enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "enabled"));
        record->granted = (unsigned)granted;
        record->valid = ok;
        if (!ok) {
            ced_extension_manifest_free(&record->manifest);
            snprintf(record->manifest.id, sizeof record->manifest.id, "%s", id);
            snprintf(record->manifest.version, sizeof record->manifest.version, "%s", version);
            snprintf(record->manifest.name, sizeof record->manifest.name, "%s", id);
            snprintf(record->error, sizeof record->error, "%s", diagnostic);
        }
    }
    cJSON_Delete(root); return true;
invalid:
    cJSON_Delete(root); ced_extension_catalog_free(catalog); return false;
}

bool ced_extension_install(const char *profile, const CedExtensionRecord *approved, char *error) {
    *error = 0;
    if (!approved->valid || (approved->granted & ~approved->manifest.capabilities) ||
        ((approved->granted & CED_EXT_EDIT) && !(approved->granted & CED_EXT_READ))) return failure(error, "Invalid permission grant");
    int lock = profile_lock(profile, error);
    if (lock < 0) return false;
    bool ok = false;
    cJSON *root = load_profile(profile, error);
    char packages[CED_PATH], publisher[CED_PATH], destination[CED_PATH], temporary[CED_PATH] = {0};
    if (!root || !join(packages, profile, "extensions", error)) goto done;
    cJSON *registered = profile_entry(root, approved->manifest.id, false);
    if (registered) {
        const char *version = ced_ext_json_string(registered, "version"), *hash = ced_ext_json_string(registered, "hash");
        if (!version || !hash || (!strcmp(version, approved->manifest.version) && strcmp(hash, approved->hash))) {
            failure(error, "Conflict: the registered version has different contents"); goto done;
        }
    } else if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "extensions")) >= CED_EXTENSION_LIMIT) {
        failure(error, "ResourceLimit: at most eight installed extensions"); goto done;
    }
    CedExtDirectory dir;
    if (!directory(&dir, packages, true, error)) goto done;
    ced_ext_directory_close(&dir);
    if (!ced_extension_id_valid(approved->manifest.id) || !ced_extension_version_valid(approved->manifest.version) ||
        !join(publisher, packages, approved->manifest.id, error)) goto done;
    if (!directory(&dir, publisher, true, error)) goto done;
    ced_ext_directory_close(&dir);
    if (!join(destination, publisher, approved->manifest.version, error) || !join(temporary, packages, ".install-XXXXXX", error)) goto done;
    if (!ced_ext_mkdtemp(temporary)) { *temporary = 0; failure(error, "Could not stage extension installation"); goto done; }
    char copied_hash[65];
    if (!package_scan(approved->source, temporary, copied_hash, NULL, error) || strcmp(copied_hash, approved->hash)) {
        failure(error, "Conflict: package changed after review"); goto done;
    }
    CedExtensionRecord copy = {0};
    if (!ced_extension_inspect(temporary, &copy, error)) goto done;
    bool matches = !strcmp(copy.manifest.id, approved->manifest.id) && !strcmp(copy.manifest.version, approved->manifest.version) &&
        !strcmp(copy.hash, approved->hash) && copy.manifest.capabilities == approved->manifest.capabilities;
    ced_extension_record_free(&copy);
    if (!matches) { failure(error, "Conflict: package changed after review"); goto done; }
    struct stat st;
    if (!lstat(destination, &st)) {
        CedExtensionRecord existing = {0};
        matches = ced_extension_inspect(destination, &existing, error) && !strcmp(existing.hash, approved->hash);
        ced_extension_record_free(&existing);
        if (!matches) { failure(error, "Conflict: the same extension version has different contents"); goto done; }
    } else if (errno != ENOENT || rename(temporary, destination)) { failure(error, "Could not publish extension package"); goto done; }
    else *temporary = 0;
    cJSON *entry = profile_entry(root, approved->manifest.id, true);
    if (!entry) { failure(error, "ResourceLimit: at most eight installed extensions"); goto done; }
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "version"); cJSON_AddStringToObject(entry, "version", approved->manifest.version);
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "hash"); cJSON_AddStringToObject(entry, "hash", approved->hash);
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "source"); cJSON_AddStringToObject(entry, "source", approved->source);
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "enabled"); cJSON_AddBoolToObject(entry, "enabled", true);
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "granted"); cJSON_AddNumberToObject(entry, "granted", approved->granted);
    ok = save_profile(profile, root, error);
done:
    if (*temporary) { char ignored[CED_ERROR]; ced_file_delete(packages, temporary, ignored); }
    cJSON_Delete(root); close(lock); return ok;
}

bool ced_extension_set_enabled(const char *profile, const char *id, bool enabled, char *error) {
    int lock = profile_lock(profile, error); if (lock < 0) return false;
    cJSON *root = load_profile(profile, error), *entry = root ? profile_entry(root, id, false) : NULL;
    bool ok = false;
    if (entry) {
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "enabled"); cJSON_AddBoolToObject(entry, "enabled", enabled);
        ok = save_profile(profile, root, error);
    } else failure(error, "Extension is not installed");
    cJSON_Delete(root); close(lock); return ok;
}

bool ced_extension_set_trusted(const char *profile, const char *workspace, bool trusted, char *error) {
    char canonical[CED_PATH];
    if (!realpath(workspace, canonical)) return failure(error, "Workspace is unavailable");
    int lock = profile_lock(profile, error); if (lock < 0) return false;
    cJSON *root = load_profile(profile, error);
    bool ok = false;
    if (root) {
        cJSON *paths = cJSON_GetObjectItemCaseSensitive(root, "trustedWorkspaces");
        for (int i = cJSON_GetArraySize(paths) - 1; i >= 0; i--) {
            const cJSON *path = cJSON_GetArrayItem(paths, i);
            if (cJSON_IsString(path) && ced_ext_path_equal(path->valuestring, canonical)) cJSON_DeleteItemFromArray(paths, i);
        }
        if (trusted) cJSON_AddItemToArray(paths, cJSON_CreateString(canonical));
        ok = save_profile(profile, root, error);
    }
    cJSON_Delete(root); close(lock); return ok;
}

int ced_extension_lease(const char *profile, const char *id, const char *version, bool exclusive, char *error) {
    if (!ced_extension_id_valid(id) || !ced_extension_version_valid(version)) return -1;
    char name[256];
    CedExtDirectory base, locks;
    if (!directory(&base, profile, false, error)) return -1;
    bool opened = ced_ext_directory_private(&base) && ced_ext_directory(&locks, &base, "extension-locks", true);
    ced_ext_directory_close(&base);
    if (!opened) { failure(error, "Could not open the extension lock directory"); return -1; }
    snprintf(name, sizeof name, "%s-%s.lock", id, version);
    int fd = ced_ext_directory_private(&locks) ? ced_ext_file(&locks, name, O_RDWR | O_CREAT) : -1;
    ced_ext_directory_close(&locks);
    CedExtFileInfo info;
    if (fd < 0 || !ced_ext_file_info(fd, name, true, &info) || !ced_ext_lock(fd, exclusive)) {
        if (fd >= 0) close(fd);
        failure(error, "Conflict: package is in use by another window or its lock is unsafe"); return -1;
    }
    return fd;
}

bool ced_extension_uninstall(const char *profile, const char *id, char *error) {
    int lock = profile_lock(profile, error); if (lock < 0) return false;
    cJSON *root = load_profile(profile, error), *entry = root ? profile_entry(root, id, false) : NULL;
    bool ok = false; int leases[128]; size_t lease_count = 0;
    char *versions[128]; size_t version_count = 0;
    char relative[256], package[CED_PATH], staging[CED_PATH] = {0};
    if (!entry || !ced_extension_id_valid(id)) { failure(error, "Extension is not installed"); goto done; }
    snprintf(relative, sizeof relative, "extensions/%s", id);
    if (!join(package, profile, relative, error)) goto done;
    CedExtDirectory dir;
    if (!directory(&dir, package, false, error)) goto done;
    bool listed = ced_ext_directory_names(&dir, versions, &version_count, 128);
    ced_ext_directory_close(&dir);
    if (!listed) { failure(error, "Could not list package versions"); goto done; }
    for (size_t n = 0; n < version_count; n++) {
        if (!ced_extension_version_valid(versions[n])) { failure(error, "Invalid package version directory"); goto done; }
        int lease = ced_extension_lease(profile, id, versions[n], true, error);
        if (lease < 0) goto done;
        leases[lease_count++] = lease;
    }
    /* Remove the active registry entry atomically before deleting files. A
     * failed profile write restores the untouched package directory. */
    if (!join(staging, profile, "extensions/.remove-XXXXXX", error) || !ced_ext_mkdtemp(staging)) goto done;
    if (ced_ext_rmdir(staging) || rename(package, staging)) goto done;
    cJSON_Delete(cJSON_DetachItemViaPointer(cJSON_GetObjectItemCaseSensitive(root, "extensions"), entry));
    if (!save_profile(profile, root, error)) { rename(staging, package); goto done; }
    ok = ced_file_delete(profile, staging, error);
done:
    for (size_t n = 0; n < version_count; n++) free(versions[n]);
    for (size_t n = 0; n < lease_count; n++) close(leases[n]);
    cJSON_Delete(root); close(lock); return ok;
}

void ced_extension_record_free(CedExtensionRecord *record) { ced_extension_manifest_free(&record->manifest); memset(record, 0, sizeof *record); }
void ced_extension_catalog_free(CedExtensionCatalog *catalog) {
    for (size_t i = 0; i < catalog->count; i++) ced_extension_record_free(&catalog->entries[i]);
    memset(catalog, 0, sizeof *catalog);
}
