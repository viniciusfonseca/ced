#ifndef CED_EXTENSION_PROCESS_H
#define CED_EXTENSION_PROCESS_H

#include "catalog.h"
#include "rpc_transport.h"

typedef struct CedExtensionProcess CedExtensionProcess;
typedef struct CedExtensionMessage { cJSON *json; size_t charge, body_length; struct CedExtensionMessage *next; } CedExtensionMessage;

CedExtensionProcess *ced_extension_process_start(const char *profile, const CedExtensionRecord *record,
                                                const char *session, char *error);
/* Send consumes json, including on failure. Parsing and serialization happen
 * on the process worker; receive transfers a parsed, charged message. */
bool ced_extension_process_send(CedExtensionProcess *process, cJSON *json);
/* Reserve the complete reply and an outgoing slot before a mutation. Prepare
 * consumes json. Submit/discard consume the reservation without allocating. */
CedExtensionMessage *ced_extension_process_prepare(CedExtensionProcess *process, cJSON *json);
bool ced_extension_process_submit(CedExtensionProcess *process, CedExtensionMessage *message);
void ced_extension_process_discard(CedExtensionProcess *process, CedExtensionMessage *message);
CedExtensionMessage *ced_extension_process_receive(CedExtensionProcess *process);
void ced_extension_message_free(CedExtensionMessage *message);
void ced_extension_process_log(CedExtensionProcess *process, const char *text);
void ced_extension_process_stop(CedExtensionProcess *process);
bool ced_extension_process_done(CedExtensionProcess *process, char *error);
size_t ced_extension_process_queued(CedExtensionProcess *process);
/* Called only after done, except bounded shutdown outside UI frames. */
void ced_extension_process_destroy(CedExtensionProcess *process);

#endif
