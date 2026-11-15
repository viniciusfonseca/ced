#include "catalog.h"
#include "sha256.h"
#include "file_operations.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static bool failure(char *error, const char *message) { snprintf(error, CED_ERROR, "%s", message); return false; }
static bool join(char *out, const char *root, const char *name, char *error) {
    return ced_join(out, CED_PATH, root, name) || failure(error, "Extension path is too long");
}

static int directory(const char *path, bool create, char *error) {
    if (create && mkdir(path, 0700) && errno != EEXIST) { failure(error, "Could not create extension directory"); return -1; }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) failure(error, "Extension directory is missing or is a symlink");
    return fd;
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

typedef struct { size_t bytes, files; CedSha256 hash; char *manifest; size_t manifest_length; } PackageScan;

/* Directory descriptors remain open throughout traversal. Files are copied
 * from those descriptors, never reopened through an untrusted absolute path. */
static bool package_walk(int source, int destination, const char *relative, unsigned depth, PackageScan *scan, char *error) {
    if (depth > 32) return failure(error, "Package directory depth exceeded");
    DIR *dir = fdopendir(dup(source));
    if (!dir) return failure(error, "Could not read extension package");
    char **names = calloc(CED_EXTENSION_FILE_LIMIT + 1, sizeof *names);
    size_t count = 0;
    bool ok = names != NULL;
    struct dirent *entry;
    while (ok && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++scan->files > CED_EXTENSION_FILE_LIMIT || count == CED_EXTENSION_FILE_LIMIT) { ok = false; break; }
        names[count] = strdup(entry->d_name);
        if (!names[count]) { ok = false; break; }
        count++;
    }
    closedir(dir);
    if (ok) qsort(names, count, sizeof *names, name_order);
    for (size_t i = 0; ok && i < count; i++) {
        char path[CED_PATH];
        int n = snprintf(path, sizeof path, "%s%s%s", relative, *relative ? "/" : "", names[i]);
        struct stat st;
        if (n < 0 || (size_t)n >= sizeof path || fstatat(source, names[i], &st, AT_SYMLINK_NOFOLLOW) ||
            (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) { ok = false; break; }
        unsigned char kind = S_ISDIR(st.st_mode) ? 'd' : (st.st_mode & 0111) ? 'x' : 'f';
        ced_sha256_update(&scan->hash, &kind, 1); ced_sha256_update(&scan->hash, path, strlen(path) + 1);
        if (S_ISDIR(st.st_mode)) {
            int child = openat(source, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), copy = -1;
            if (destination >= 0 && (mkdirat(destination, names[i], 0700) ||
                (copy = openat(destination, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)) ok = false;
            if (child < 0) ok = false;
            if (ok) ok = package_walk(child, copy, path, depth + 1, scan, error);
            if (child >= 0) close(child);
            if (copy >= 0) close(copy);
        } else {
            if (st.st_size < 0 || (uint64_t)st.st_size > CED_EXTENSION_PACKAGE_LIMIT - scan->bytes) { ok = false; break; }
            unsigned char file_length[8];
            for (unsigned b = 0; b < 8; b++) file_length[b] = (unsigned char)((uint64_t)st.st_size >> (56 - b * 8));
            ced_sha256_update(&scan->hash, file_length, sizeof file_length);
            int file = openat(source, names[i], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), copy = -1;
            struct stat opened;
            if (file < 0 || fstat(file, &opened) || !S_ISREG(opened.st_mode) || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) ok = false;
            if (ok && destination >= 0 && (copy = openat(destination, names[i], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)) < 0) ok = false;
            size_t read_bytes = 0;
            bool manifest = !strcmp(path, "ced-extension.json");
            if (ok && manifest) {
                if (st.st_size <= 0 || st.st_size > CED_EXTENSION_MANIFEST_LIMIT) ok = false;
                else { scan->manifest = malloc((size_t)st.st_size + 1); if (!scan->manifest) ok = false; }
            }
            unsigned char bytes[16384];
            while (ok) {
                ssize_t size = read(file, bytes, sizeof bytes);
                if (size < 0 && errno == EINTR) continue;
                if (size < 0) { ok = false; break; }
                if (!size) break;
                if ((size_t)size > CED_EXTENSION_PACKAGE_LIMIT - scan->bytes) { ok = false; break; }
                if (manifest) {
                    if ((size_t)size > (size_t)st.st_size - read_bytes) { ok = false; break; }
                    memcpy(scan->manifest + read_bytes, bytes, (size_t)size);
                }
                scan->bytes += (size_t)size; read_bytes += (size_t)size;
                ced_sha256_update(&scan->hash, bytes, (size_t)size);
                if (copy >= 0 && !write_all(copy, bytes, (size_t)size)) ok = false;
            }
            if (ok && read_bytes != (size_t)st.st_size) ok = false;
            if (ok && manifest) { scan->manifest_length = read_bytes; scan->manifest[read_bytes] = 0; }
            if (file >= 0) close(file);
            if (copy >= 0) {
                if (ok && (fchmod(copy, kind == 'x' ? 0555 : 0444) || fsync(copy))) ok = false;
                if (close(copy)) ok = false;
            }
        }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    if (!ok && !*error) failure(error, "Package changed, contains links/special files, exceeds limits, or cannot be copied");
    return ok;
}

static bool package_scan(const char *source, const char *destination, char hash[65], CedExtensionManifest *manifest, char *error) {
    int from = directory(source, false, error), to = -1;
    if (from < 0) return false;
    if (destination && (to = directory(destination, false, error)) < 0) { close(from); return false; }
    PackageScan scan = {0}; ced_sha256_init(&scan.hash);
    bool ok = package_walk(from, to, "", 0, &scan, error);
    if (ok && manifest) {
        if (!scan.manifest) ok = failure(error, "Missing ced-extension.json");
        else ok = ced_extension_manifest_parse(scan.manifest, scan.manifest_length, manifest, error);
    }
    free(scan.manifest);
    close(from); if (to >= 0) close(to);
    if (ok) ced_sha256_finish(&scan.hash, hash);
    return ok;
}

static bool entrypoint_valid(const char *package, const char *entrypoint, char *error) {
    int fd = directory(package, false, error);
    if (fd < 0) return false;
    char path[CED_PATH]; snprintf(path, sizeof path, "%s", entrypoint);
    char *state, *part = strtok_r(path, "/", &state);
    while (part) {
        char *next = strtok_r(NULL, "/", &state);
        int child = openat(fd, part, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (next ? O_DIRECTORY : 0));
        close(fd); fd = child;
        if (fd < 0) return failure(error, "Entrypoint is missing or linked");
        if (!next) {
            struct stat st;
            bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0111);
            close(fd);
            return ok || failure(error, "Entrypoint must be a regular executable file");
        }
        part = next;
    }
    close(fd); return false;
}

bool ced_extension_inspect(const char *source, CedExtensionRecord *record, char *error) {
    memset(record, 0, sizeof *record); *error = 0;
    struct stat st;
    if (lstat(source, &st) || !S_ISDIR(st.st_mode) || !realpath(source, record->source)) return failure(error, "Select a regular package directory, not a symlink");
    if (!package_scan(record->source, NULL, record->hash, &record->manifest, error)) return false;
    if (record->manifest.executable && !entrypoint_valid(record->source, record->manifest.entrypoint, error)) {
        ced_extension_manifest_free(&record->manifest); return false;
    }
    record->valid = true; record->granted = record->manifest.capabilities;
    return true;
}

static cJSON *load_profile(const char *profile, char *error) {
    char path[CED_PATH];
    if (!join(path, profile, "extensions.json", error)) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "formatVersion", 1);
        cJSON_AddArrayToObject(root, "extensions"); cJSON_AddArrayToObject(root, "trustedWorkspaces");
        return root;
    }
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) || st.st_size < 0 || st.st_size > CED_EXTENSION_MANIFEST_LIMIT) {
        if (fd >= 0) close(fd); failure(error, "Invalid extensions.json"); return NULL;
    }
    size_t used = 0, size = (size_t)st.st_size;
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
    int dir = directory(profile, true, error);
    if (dir < 0) return -1;
    struct stat st;
    if (fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(dir); failure(error, "The extension profile must be owned by the current user and not writable by others"); return -1;
    }
    int lock = openat(dir, ".extensions.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0) close(lock); failure(error, "Conflict: extension profile is busy"); return -1;
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
    struct stat profile_stat;
    if (!lstat(profile, &profile_stat) && (!S_ISDIR(profile_stat.st_mode) || profile_stat.st_uid != geteuid() || (profile_stat.st_mode & 0022)))
        return failure(error, "Unsafe extension profile directory");
    cJSON *root = load_profile(profile, error);
    if (!root) return false;
    char canonical[CED_PATH];
    if (!realpath(workspace, canonical)) { cJSON_Delete(root); return failure(error, "Workspace is unavailable"); }
    cJSON *trusted = cJSON_GetObjectItemCaseSensitive(root, "trustedWorkspaces");
    for (const cJSON *path = trusted->child; path; path = path->next)
        if (cJSON_IsString(path) && !strcmp(path->valuestring, canonical)) catalog->workspace_trusted = true;
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
    int fd = directory(packages, true, error); if (fd < 0) goto done; close(fd);
    if (!ced_extension_id_valid(approved->manifest.id) || !ced_extension_version_valid(approved->manifest.version) ||
        !join(publisher, packages, approved->manifest.id, error)) goto done;
    fd = directory(publisher, true, error); if (fd < 0) goto done; close(fd);
    if (!join(destination, publisher, approved->manifest.version, error) || !join(temporary, packages, ".install-XXXXXX", error)) goto done;
    if (!mkdtemp(temporary)) { *temporary = 0; failure(error, "Could not stage extension installation"); goto done; }
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
            if (cJSON_IsString(path) && !strcmp(path->valuestring, canonical)) cJSON_DeleteItemFromArray(paths, i);
        }
        if (trusted) cJSON_AddItemToArray(paths, cJSON_CreateString(canonical));
        ok = save_profile(profile, root, error);
    }
    cJSON_Delete(root); close(lock); return ok;
}

int ced_extension_lease(const char *profile, const char *id, const char *version, bool exclusive, char *error) {
    if (!ced_extension_id_valid(id) || !ced_extension_version_valid(version)) return -1;
    char locks[CED_PATH], path[CED_PATH], name[256];
    if (!join(locks, profile, "extension-locks", error)) return -1;
    int dir = directory(locks, true, error); if (dir < 0) return -1; close(dir);
    snprintf(name, sizeof name, "%s-%s.lock", id, version);
    if (!join(path, locks, name, error)) return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        if (fd >= 0) close(fd); failure(error, "Conflict: package is in use by another window"); return -1;
    }
    return fd;
}

bool ced_extension_uninstall(const char *profile, const char *id, char *error) {
    int lock = profile_lock(profile, error); if (lock < 0) return false;
    cJSON *root = load_profile(profile, error), *entry = root ? profile_entry(root, id, false) : NULL;
    bool ok = false; int leases[128]; size_t lease_count = 0;
    DIR *versions = NULL;
    char relative[256], package[CED_PATH], staging[CED_PATH] = {0};
    if (!entry || !ced_extension_id_valid(id)) { failure(error, "Extension is not installed"); goto done; }
    snprintf(relative, sizeof relative, "extensions/%s", id);
    if (!join(package, profile, relative, error)) goto done;
    int fd = directory(package, false, error);
    if (fd < 0) goto done;
    versions = fdopendir(fd);
    if (!versions) { close(fd); goto done; }
    struct dirent *version;
    while ((version = readdir(versions))) {
        if (!strcmp(version->d_name, ".") || !strcmp(version->d_name, "..")) continue;
        if (lease_count == 128 || !ced_extension_version_valid(version->d_name)) { failure(error, "Invalid package version directory"); goto done; }
        int lease = ced_extension_lease(profile, id, version->d_name, true, error);
        if (lease < 0) goto done;
        leases[lease_count++] = lease;
    }
    closedir(versions); versions = NULL;
    /* Remove the active registry entry atomically before deleting files. A
     * failed profile write restores the untouched package directory. */
    if (!join(staging, profile, "extensions/.remove-XXXXXX", error) || !mkdtemp(staging)) goto done;
    if (rmdir(staging) || rename(package, staging)) goto done;
    cJSON_Delete(cJSON_DetachItemViaPointer(cJSON_GetObjectItemCaseSensitive(root, "extensions"), entry));
    if (!save_profile(profile, root, error)) { rename(staging, package); goto done; }
    ok = ced_file_delete(profile, staging, error);
done:
    if (versions) closedir(versions);
    for (size_t n = 0; n < lease_count; n++) close(leases[n]);
    cJSON_Delete(root); close(lock); return ok;
}

void ced_extension_record_free(CedExtensionRecord *record) { ced_extension_manifest_free(&record->manifest); memset(record, 0, sizeof *record); }
void ced_extension_catalog_free(CedExtensionCatalog *catalog) {
    for (size_t i = 0; i < catalog->count; i++) ced_extension_record_free(&catalog->entries[i]);
    memset(catalog, 0, sizeof *catalog);
}
