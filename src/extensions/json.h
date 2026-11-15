#ifndef CED_EXTENSION_JSON_H
#define CED_EXTENSION_JSON_H

#include "cJSON.h"
#include "documents.h"
#define CED_EXTENSION_JSON_TOKEN_LIMIT 32768

/* Bounds are checked before cJSON allocates a DOM. Nodes and strings have a
 * conservative combined bound; no per-request changes to global cJSON hooks. */
cJSON *ced_ext_json_parse(const char *text, size_t length, unsigned depth, char *error);
bool ced_ext_json_fields(const cJSON *object, const char *const *allowed, char *error);
const char *ced_ext_json_string(const cJSON *object, const char *key);
bool ced_ext_json_uint(const cJSON *value, uint64_t *number);
/* Consume value on success and failure. */
bool ced_ext_json_add(cJSON *object, const char *key, cJSON *value);
bool ced_ext_json_append(cJSON *array, cJSON *value);
bool ced_ext_rpc_id(const char *id, const char *prefix, uint64_t *counter);
bool ced_ext_rpc_failure_valid(const cJSON *error);
/* Result is consumed, including when constructing the envelope fails. */
cJSON *ced_ext_rpc_result(const char *id, cJSON *result);
cJSON *ced_ext_rpc_error(const char *id, int code, const char *reason, const char *message);
double ced_ext_now(void);

#endif
