#include "json.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>

double ced_ext_now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static bool valid_tree(const cJSON *value) {
    if (cJSON_IsNumber(value) && !isfinite(value->valuedouble)) return false;
    if (cJSON_IsObject(value)) {
        for (const cJSON *a = value->child; a; a = a->next)
            for (const cJSON *b = a->next; b; b = b->next)
                if (!strcmp(a->string, b->string)) return false;
    }
    for (const cJSON *child = value->child; child; child = child->next) if (!valid_tree(child)) return false;
    return true;
}

cJSON *ced_ext_json_parse(const char *text, size_t length, unsigned max_depth, char *error) {
    if (!length || (length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) || !ced_utf8_valid(text, length)) goto invalid;
    unsigned depth = 0, tokens = 0;
    bool quoted = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (quoted) {
            if (c < 32) goto invalid;
            if (c == '\\') {
                if (++i == length) goto invalid;
                if (text[i] == 'u' && i + 4 < length && !memcmp(text + i + 1, "0000", 4)) goto invalid;
            } else if (c == '"') quoted = false;
        } else if (c < 32 && c != '\n' && c != '\r' && c != '\t') goto invalid;
        else if (c == '"') { quoted = true; tokens++; }
        else if (c == '{' || c == '[') { if (++depth > max_depth) goto invalid; tokens++; }
        else if (c == '}' || c == ']') { if (!depth) goto invalid; depth--; }
        else if (c == ',' || c == ':') tokens++;
        else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t n = i;
            if (text[n] == '-' && ++n == length) goto invalid;
            if (text[n] == '0') n++;
            else {
                if (text[n] < '1' || text[n] > '9') goto invalid;
                while (n < length && text[n] >= '0' && text[n] <= '9') n++;
            }
            if (n < length && text[n] == '.') {
                size_t first = ++n;
                while (n < length && text[n] >= '0' && text[n] <= '9') n++;
                if (n == first) goto invalid;
            }
            if (n < length && (text[n] == 'e' || text[n] == 'E')) {
                n++; if (n < length && (text[n] == '+' || text[n] == '-')) n++;
                size_t first = n;
                while (n < length && text[n] >= '0' && text[n] <= '9') n++;
                if (n == first) goto invalid;
            }
            if (n < length && !strchr(" \t\r\n,]}", text[n])) goto invalid;
            i = n - 1; tokens++;
        }
        if (tokens > CED_EXTENSION_JSON_TOKEN_LIMIT) goto invalid;
    }
    if (quoted || depth) goto invalid;
    const char *end = NULL;
    cJSON *value = cJSON_ParseWithLengthOpts(text, length, &end, false);
    while (end && end < text + length && isspace((unsigned char)*end)) end++;
    if (!value || end != text + length || !valid_tree(value)) { cJSON_Delete(value); goto invalid; }
    return value;
invalid:
    snprintf(error, CED_ERROR, "Invalid JSON, UTF-8, nonfinite number, duplicate keys, or JSON resource limit");
    return NULL;
}

bool ced_ext_json_fields(const cJSON *object, const char *const *allowed, char *error) {
    if (!cJSON_IsObject(object)) { snprintf(error, CED_ERROR, "Expected an object"); return false; }
    for (const cJSON *field = object->child; field; field = field->next) {
        bool found = false;
        for (size_t i = 0; allowed[i]; i++) if (!strcmp(field->string, allowed[i])) found = true;
        if (!found) { snprintf(error, CED_ERROR, "Unsupported field: %.128s", field->string); return false; }
    }
    return true;
}

const char *ced_ext_json_string(const cJSON *object, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

bool ced_ext_json_uint(const cJSON *value, uint64_t *number) {
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > (double)CED_REVISION_MAX || floor(value->valuedouble) != value->valuedouble) return false;
    *number = (uint64_t)value->valuedouble; return true;
}

bool ced_ext_json_add(cJSON *object, const char *key, cJSON *value) {
    if (value && cJSON_AddItemToObject(object, key, value)) return true;
    cJSON_Delete(value); return false;
}

bool ced_ext_json_append(cJSON *array, cJSON *value) {
    if (value && cJSON_AddItemToArray(array, value)) return true;
    cJSON_Delete(value); return false;
}

bool ced_ext_rpc_id(const char *id, const char *prefix, uint64_t *counter) {
    size_t n = strlen(prefix);
    if (!id || strlen(id) > 64 || strncmp(id, prefix, n) || id[n] < '1' || id[n] > '9') return false;
    for (const char *p = id + n; *p; p++) if (*p < '0' || *p > '9') return false;
    char *end; errno = 0;
    unsigned long long value = strtoull(id + n, &end, 10);
    if (errno || *end || value > UINT64_MAX) return false;
    if (counter) *counter = (uint64_t)value;
    return true;
}

bool ced_ext_rpc_failure_valid(const cJSON *error) {
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    return cJSON_IsObject(error) && cJSON_IsNumber(code) && isfinite(code->valuedouble) &&
        floor(code->valuedouble) == code->valuedouble && ced_ext_json_string(error, "message");
}

cJSON *ced_ext_rpc_result(const char *id, cJSON *result) {
    cJSON *message = cJSON_CreateObject();
    if (!result) result = cJSON_CreateNull();
    if (!message || !result || !cJSON_AddStringToObject(message, "jsonrpc", "2.0") ||
        !ced_ext_json_add(message, "id", id ? cJSON_CreateString(id) : cJSON_CreateNull())) {
        cJSON_Delete(message); cJSON_Delete(result); return NULL;
    }
    if (!ced_ext_json_add(message, "result", result)) { cJSON_Delete(message); return NULL; }
    return message;
}

cJSON *ced_ext_rpc_error(const char *id, int code, const char *reason, const char *text) {
    cJSON *message = cJSON_CreateObject();
    if (!message) return NULL;
    cJSON *error = cJSON_AddObjectToObject(message, "error");
    if (!error || !cJSON_AddStringToObject(message, "jsonrpc", "2.0") ||
        !ced_ext_json_add(message, "id", id ? cJSON_CreateString(id) : cJSON_CreateNull()) ||
        !cJSON_AddNumberToObject(error, "code", code) || !cJSON_AddStringToObject(error, "message", text ? text : reason)) goto failed;
    cJSON *data = cJSON_AddObjectToObject(error, "data");
    if (!data || !cJSON_AddStringToObject(data, "reason", reason)) goto failed;
    return message;
failed:
    cJSON_Delete(message); return NULL;
}
