#ifndef CED_RPC_TRANSPORT_H
#define CED_RPC_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#define CED_RPC_HEADER_LIMIT 1024
#define CED_RPC_BODY_LIMIT (8 * 1024 * 1024)
#define CED_RPC_OUTPUT_LIMIT (16 * 1024 * 1024)

typedef struct { char *bytes; size_t length, capacity; bool failed; } CedRpcReader;
bool ced_rpc_feed(CedRpcReader *reader, const void *bytes, size_t length);
/* 1 = owned NUL-terminated body (free + release length+1), 0 = incomplete,
 * -1 = fatal framing error. */
int ced_rpc_next(CedRpcReader *reader, char **body, size_t *length);
void ced_rpc_reader_free(CedRpcReader *reader);
char *ced_rpc_frame(const char *body, size_t length, size_t *frame_length);
void ced_rpc_frame_free(char *frame, size_t length);
/* Shared across extension instances. Reservations precede allocation. */
bool ced_rpc_reserve(size_t bytes);
void ced_rpc_release(size_t bytes);
size_t ced_rpc_memory_used(void);

#endif
