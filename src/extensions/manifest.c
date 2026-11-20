#include "manifest.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *ced_extension_platform(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin-x86_64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#else
    return "unsupported";
#endif
}

bool ced_extension_id_valid(const char *id) {
    if (!id || !*id || strlen(id) > 128 || !strncmp(id, "ced.", 4)) return false;
    unsigned dots = 0, part = 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        if (*p == '.') { if (!part || ++dots > 1) return false; part = 0; }
        else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-') part++;
        else return false;
    }
    return dots == 1 && part > 0;
}

static bool version_parts(const char *version, unsigned parts[3]) {
    if (!version || !*version) return false;
    const char *p = version;
    for (unsigned i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return false;
        unsigned number = 0;
        while (*p >= '0' && *p <= '9') {
            if (number > 99999) return false;
            number = number * 10 + (unsigned)(*p++ - '0');
        }
        parts[i] = number;
        if (i != 2) { if (*p++ != '.') return false; }
    }
    return !*p;
}

bool ced_extension_version_valid(const char *version) { unsigned parts[3]; return version_parts(version, parts); }

bool ced_extension_relative_path(const char *path) {
    if (!path || !*path || strlen(path) >= CED_PATH || *path == '/' || strchr(path, '\\')) return false;
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        if (!length || (length == 1 && *p == '.') || (length == 2 && !memcmp(p, "..", 2))) return false;
        if (!end) return true;
        p = end + 1;
    }
    return false;
}

static bool own_name(const char *id, const char *name) {
    size_t n = strlen(id);
    if (!name || strncmp(id, name, n) || name[n] != '.' || !name[n + 1] || strlen(name) > 256) return false;
    bool part = false;
    for (const unsigned char *p = (const unsigned char *)name + n + 1; *p; p++) {
        if (*p == '.') { if (!part) return false; part = false; continue; }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) && *p != '-' && *p != '_') return false;
        part = true;
    }
    return part;
}

static bool presentation(const cJSON *object, const char *key, char *out, size_t capacity, bool required) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value && !required) { *out = 0; return true; }
    if (!cJSON_IsString(value) || (required && !*value->valuestring) || strlen(value->valuestring) >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)value->valuestring; *p; p++) if (*p < 32 || *p == 127) return false;
    snprintf(out, capacity, "%s", value->valuestring); return true;
}

static bool parse_tabs(const cJSON *list, CedExtensionManifest *manifest, char *error) {
    if (!list) return true;
    static const char *const fields[] = {"id", "title", "panel", "command", NULL};
    if (!manifest->executable || !cJSON_IsArray(list) ||
        cJSON_GetArraySize(list) > CED_EXTENSION_TAB_LIMIT) return false;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, list) {
        if (!ced_ext_json_fields(entry, fields, error)) return false;
        CedExtensionTab *tab = &manifest->tabs[manifest->tab_count];
        const char *panel = ced_ext_json_string(entry, "panel");
        const char *command = ced_ext_json_string(entry, "command");
        if (!panel || (strcmp(panel, "sidebar") && strcmp(panel, "terminal")) || !command ||
            !presentation(entry, "id", tab->id, sizeof tab->id, true) || !own_name(manifest->id, tab->id) ||
            !presentation(entry, "title", tab->title, sizeof tab->title, true)) return false;
        while (tab->command < manifest->command_count &&
               strcmp(command, manifest->commands[tab->command].id)) tab->command++;
        if (tab->command == manifest->command_count) return false;
        for (size_t i = 0; i < manifest->tab_count; i++)
            if (!strcmp(manifest->tabs[i].id, tab->id) || manifest->tabs[i].command == tab->command)
                return false;
        tab->terminal = !strcmp(panel, "terminal");
        manifest->tab_count++;
    }
    return true;
}

static bool condition(const cJSON *object, CedCommandCondition *when, char *error) {
    if (!object) return true;
    static const char *const fields[] = {"hasDocument", "editorTextFocus", "hasSelection", "workspaceTrusted", "languageId", NULL};
    if (!ced_ext_json_fields(object, fields, error)) return false;
    unsigned masks[] = {CED_WHEN_DOCUMENT, CED_WHEN_FOCUS, CED_WHEN_SELECTION, CED_WHEN_TRUSTED};
    for (unsigned i = 0; i < 4; i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, fields[i]);
        if (!value) continue;
        if (!cJSON_IsBool(value)) return false;
        when->mask |= masks[i]; if (cJSON_IsTrue(value)) when->values |= masks[i];
    }
    return presentation(object, "languageId", when->language, sizeof when->language, false);
}

static bool parse_commands(const cJSON *array, CedExtensionManifest *manifest, char *error) {
    if (!array) return true;
    int count = cJSON_GetArraySize(array);
    if (!cJSON_IsArray(array) || count > CED_EXTENSION_COMMAND_LIMIT) return false;
    manifest->commands = calloc((size_t)count + 1, sizeof *manifest->commands);
    if (!manifest->commands) return false;
    static const char *const fields[] = {"id", "title", "category", "enableWhen", NULL};
    for (int i = 0; i < count; i++) {
        const cJSON *value = cJSON_GetArrayItem(array, i);
        CedExtensionCommand *command = &manifest->commands[i];
        if (!ced_ext_json_fields(value, fields, error) ||
            !presentation(value, "id", command->id, sizeof command->id, true) || !own_name(manifest->id, command->id) ||
            !presentation(value, "title", command->title, sizeof command->title, true) ||
            !presentation(value, "category", command->category, sizeof command->category, false) ||
            !condition(cJSON_GetObjectItemCaseSensitive(value, "enableWhen"), &command->when, error)) return false;
        for (int j = 0; j < i; j++) if (!strcmp(command->id, manifest->commands[j].id)) return false;
        manifest->command_count++;
    }
    return true;
}

static bool configuration(const cJSON *array, CedExtensionManifest *manifest, char *error) {
    if (!array) return true;
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 100) return false;
    static const char *const fields[] = {"key", "type", "default", "scope", "description", NULL};
    for (const cJSON *value = array->child; value; value = value->next) {
        char description[513];
        if (!ced_ext_json_fields(value, fields, error)) return false;
        if (!presentation(value, "description", description, sizeof description, false)) return false;
        const char *key = ced_ext_json_string(value, "key"), *type = ced_ext_json_string(value, "type");
        const char *scope = ced_ext_json_string(value, "scope");
        const cJSON *initial = cJSON_GetObjectItemCaseSensitive(value, "default");
        if (!own_name(manifest->id, key) || !type || !scope || (strcmp(scope, "user") && strcmp(scope, "workspace")) ||
            !((!strcmp(type, "string") && cJSON_IsString(initial) && strlen(initial->valuestring) <= 512) ||
              (!strcmp(type, "boolean") && cJSON_IsBool(initial)) ||
              (!strcmp(type, "number") && cJSON_IsNumber(initial) && isfinite(initial->valuedouble)))) return false;
        for (const cJSON *other = array->child; other != value; other = other->next)
            if (!strcmp(key, ced_ext_json_string(other, "key"))) return false;
    }
    manifest->configuration = cJSON_Duplicate(array, true);
    return manifest->configuration != NULL;
}

bool ced_extension_manifest_parse(const char *bytes, size_t length, CedExtensionManifest *out, char *error) {
    *error = 0; memset(out, 0, sizeof *out);
    if (length > CED_EXTENSION_MANIFEST_LIMIT) goto invalid;
    cJSON *root = ced_ext_json_parse(bytes, length, 32, error);
    if (!root) return false;
    static const char *const fields[] = {"manifestVersion", "id", "name", "version", "description", "license",
        "ced", "kind", "entrypoints", "activationEvents", "capabilities", "untrustedWorkspaces", "contributes", "requirements", "metadata", NULL};
    static const char *const ced_fields[] = {"minVersion", "apiMajor", NULL};
    static const char *const contributes_fields[] = {"commands", "configuration", "tabs", NULL};
    const cJSON *ced = cJSON_GetObjectItemCaseSensitive(root, "ced"), *contributes = cJSON_GetObjectItemCaseSensitive(root, "contributes");
    uint64_t number;
    const char *kind = ced_ext_json_string(root, "kind");
    unsigned minimum[3], current[3];
    bool valid = ced_ext_json_fields(root, fields, error) &&
        ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(root, "manifestVersion"), &number) && number == 1 &&
        presentation(root, "id", out->id, sizeof out->id, true) && ced_extension_id_valid(out->id) &&
        presentation(root, "name", out->name, sizeof out->name, true) &&
        presentation(root, "version", out->version, sizeof out->version, true) && ced_extension_version_valid(out->version) &&
        presentation(root, "description", out->description, sizeof out->description, false) &&
        ced_ext_json_fields(ced, ced_fields, error) && version_parts(ced_ext_json_string(ced, "minVersion"), minimum) &&
        version_parts(CED_EXTENSION_APP_VERSION, current) &&
        ced_ext_json_uint(cJSON_GetObjectItemCaseSensitive(ced, "apiMajor"), &number) && number == 1 &&
        kind && (!strcmp(kind, "executable") || !strcmp(kind, "declarative"));
    if (!valid) goto parsed_invalid;
    char license[513];
    if (!presentation(root, "license", license, sizeof license, false)) goto parsed_invalid;
    for (unsigned i = 0; i < 3; i++) { if (minimum[i] > current[i]) goto parsed_invalid; if (minimum[i] < current[i]) break; }
    out->executable = !strcmp(kind, "executable");
    const cJSON *metadata = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    if (metadata && !cJSON_IsObject(metadata)) goto parsed_invalid;
    const cJSON *requirements = cJSON_GetObjectItemCaseSensitive(root, "requirements");
    if (requirements) {
        if (!cJSON_IsArray(requirements) || cJSON_GetArraySize(requirements) > 20) goto parsed_invalid;
        static const char *const required_fields[] = {"name", "minVersion", NULL};
        for (const cJSON *r = requirements->child; r; r = r->next) {
            char name[513], version[513];
            if (!ced_ext_json_fields(r, required_fields, error) || !presentation(r, "name", name, sizeof name, true) ||
                !presentation(r, "minVersion", version, sizeof version, true)) goto parsed_invalid;
        }
        out->requirements = cJSON_Duplicate(requirements, true);
        if (!out->requirements) goto parsed_invalid;
    }
    if (contributes && !ced_ext_json_fields(contributes, contributes_fields, error)) goto parsed_invalid;
    if (!parse_commands(cJSON_GetObjectItemCaseSensitive(contributes, "commands"), out, error) ||
        !parse_tabs(cJSON_GetObjectItemCaseSensitive(contributes, "tabs"), out, error) ||
        !configuration(cJSON_GetObjectItemCaseSensitive(contributes, "configuration"), out, error)) goto parsed_invalid;
    const cJSON *entrypoints = cJSON_GetObjectItemCaseSensitive(root, "entrypoints");
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(root, "activationEvents");
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (!out->executable) {
        if (entrypoints || events || capabilities || out->command_count || cJSON_GetObjectItemCaseSensitive(root, "untrustedWorkspaces")) goto parsed_invalid;
    } else {
        if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "untrustedWorkspaces")) ||
            !cJSON_IsObject(entrypoints) || !cJSON_IsArray(events) || !cJSON_GetArraySize(events) ||
            cJSON_GetArraySize(events) > 100 || !cJSON_IsArray(capabilities) || cJSON_GetArraySize(capabilities) > 2) goto parsed_invalid;
        const char *entrypoint = ced_ext_json_string(entrypoints, ced_extension_platform());
        if (!ced_extension_relative_path(entrypoint)) { snprintf(error, CED_ERROR, "No valid entrypoint for %s", ced_extension_platform()); goto parsed_invalid; }
        for (const cJSON *v = entrypoints->child; v; v = v->next) {
            if ((strcmp(v->string, "darwin-arm64") && strcmp(v->string, "darwin-x86_64") && strcmp(v->string, "linux-x86_64") && strcmp(v->string, "linux-arm64")) ||
                !cJSON_IsString(v) || !ced_extension_relative_path(v->valuestring)) goto parsed_invalid;
        }
        snprintf(out->entrypoint, sizeof out->entrypoint, "%s", entrypoint);
        for (const cJSON *v = capabilities->child; v; v = v->next) {
            if (!cJSON_IsString(v)) goto parsed_invalid;
            unsigned flag = !strcmp(v->valuestring, "documents.read") ? CED_EXT_READ : !strcmp(v->valuestring, "documents.edit") ? CED_EXT_EDIT : 0;
            if (!flag || (out->capabilities & flag)) goto parsed_invalid;
            out->capabilities |= flag;
        }
        if ((out->capabilities & CED_EXT_EDIT) && !(out->capabilities & CED_EXT_READ)) goto parsed_invalid;
        for (const cJSON *v = events->child; v; v = v->next) {
            if (!cJSON_IsString(v) || strncmp(v->valuestring, "onCommand:", 10)) goto parsed_invalid;
            bool found = false;
            for (size_t i = 0; i < out->command_count; i++) if (!strcmp(v->valuestring + 10, out->commands[i].id)) found = true;
            if (!found) goto parsed_invalid;
            for (const cJSON *other = events->child; other != v; other = other->next)
                if (!strcmp(other->valuestring, v->valuestring)) goto parsed_invalid;
        }
        for (size_t i = 0; i < out->command_count; i++) {
            bool found = false;
            for (const cJSON *v = events->child; v; v = v->next) if (!strcmp(v->valuestring + 10, out->commands[i].id)) found = true;
            if (!found) goto parsed_invalid;
        }
    }
    cJSON_Delete(root); return true;
parsed_invalid:
    cJSON_Delete(root); ced_extension_manifest_free(out);
invalid:
    if (!*error) snprintf(error, CED_ERROR, "Invalid or unsupported extension manifest");
    return false;
}

bool ced_extension_manifest_load(const char *package, CedExtensionManifest *manifest, char *error) {
    char path[CED_PATH];
    if (!ced_join(path, sizeof path, package, "ced-extension.json")) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > CED_EXTENSION_MANIFEST_LIMIT) {
        if (fd >= 0) close(fd);
        snprintf(error, CED_ERROR, "Missing, linked, or oversized ced-extension.json"); return false;
    }
    size_t length = (size_t)st.st_size, used = 0;
    char *bytes = malloc(length + 1);
    if (!bytes) { close(fd); return false; }
    while (used < length) {
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd); bytes[used] = 0;
    bool ok = used == length && ced_extension_manifest_parse(bytes, length, manifest, error);
    free(bytes); return ok;
}

void ced_extension_manifest_free(CedExtensionManifest *manifest) {
    free(manifest->commands); cJSON_Delete(manifest->configuration); cJSON_Delete(manifest->requirements);
    memset(manifest, 0, sizeof *manifest);
}
