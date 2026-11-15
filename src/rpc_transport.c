#include "rpc_transport.h"
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t budget_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t budget_used;

bool ced_rpc_reserve(size_t bytes) {
    pthread_mutex_lock(&budget_mutex);
    bool allowed = bytes <= 64 * 1024 * 1024 - budget_used;
    if (allowed) budget_used += bytes;
    pthread_mutex_unlock(&budget_mutex);
    return allowed;
}

void ced_rpc_release(size_t bytes) {
    pthread_mutex_lock(&budget_mutex);
    budget_used -= bytes;
    pthread_mutex_unlock(&budget_mutex);
}

size_t ced_rpc_memory_used(void) {
    pthread_mutex_lock(&budget_mutex);
    size_t bytes = budget_used;
    pthread_mutex_unlock(&budget_mutex);
    return bytes;
}

bool ced_rpc_feed(CedRpcReader *reader, const void *bytes, size_t length) {
    const size_t limit = CED_RPC_BODY_LIMIT + CED_RPC_HEADER_LIMIT;
    if (reader->failed || length > limit - reader->length) { reader->failed = true; return false; }
    size_t needed = reader->length + length + 1;
    if (needed > reader->capacity) {
        size_t capacity = reader->capacity ? reader->capacity : 4096;
        while (capacity < needed) capacity *= 2;
        if (capacity > limit + 1) capacity = limit + 1;
        size_t added = capacity - reader->capacity;
        if (!ced_rpc_reserve(added)) { reader->failed = true; return false; }
        char *data = realloc(reader->bytes, capacity);
        if (!data) { ced_rpc_release(added); reader->failed = true; return false; }
        reader->bytes = data; reader->capacity = capacity;
    }
    memcpy(reader->bytes + reader->length, bytes, length);
    reader->length += length; reader->bytes[reader->length] = 0;
    return true;
}

int ced_rpc_next(CedRpcReader *reader, char **body, size_t *length) {
    *body = NULL; *length = 0;
    if (reader->failed) return -1;
    if (!reader->length) return 0;
    size_t end = 0;
    while (end + 3 < reader->length && memcmp(reader->bytes + end, "\r\n\r\n", 4)) end++;
    if (end + 3 >= reader->length) {
        if (reader->length >= CED_RPC_HEADER_LIMIT) goto invalid;
        return 0;
    }
    size_t header = end + 4;
    if (header > CED_RPC_HEADER_LIMIT) goto invalid;
    bool found = false;
    size_t size = 0;
    for (size_t from = 0; from < end + 2;) {
        size_t to = from;
        while (to < end + 2 && reader->bytes[to] != '\r') {
            unsigned char c = (unsigned char)reader->bytes[to++];
            if (c < 32 || c > 126) goto invalid;
        }
        if (to + 1 >= header || reader->bytes[to + 1] != '\n') goto invalid;
        const char prefix[] = "Content-Length: ";
        if (to - from < sizeof prefix || memcmp(reader->bytes + from, prefix, sizeof prefix - 1) || found) goto invalid;
        found = true;
        for (size_t i = from + sizeof prefix - 1; i < to; i++) {
            unsigned char c = (unsigned char)reader->bytes[i];
            if (c < '0' || c > '9' || size > (CED_RPC_BODY_LIMIT - (size_t)(c - '0')) / 10) goto invalid;
            size = size * 10 + (size_t)(c - '0');
        }
        from = to + 2;
    }
    if (!found || !size || size > CED_RPC_BODY_LIMIT) goto invalid;
    if (reader->length - header < size) return 0;
    if (!ced_rpc_reserve(size + 1)) goto invalid;
    char *copy = malloc(size + 1);
    if (!copy) { ced_rpc_release(size + 1); goto invalid; }
    memcpy(copy, reader->bytes + header, size); copy[size] = 0;
    reader->length -= header + size;
    memmove(reader->bytes, reader->bytes + header + size, reader->length);
    reader->bytes[reader->length] = 0;
    *body = copy; *length = size;
    return 1;
invalid:
    reader->failed = true; return -1;
}

void ced_rpc_reader_free(CedRpcReader *reader) {
    free(reader->bytes); ced_rpc_release(reader->capacity); memset(reader, 0, sizeof *reader);
}

char *ced_rpc_frame(const char *body, size_t length, size_t *frame_length) {
    if (!length || length > CED_RPC_OUTPUT_LIMIT) return NULL;
    char header[64];
    int size = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", length);
    if (!ced_rpc_reserve((size_t)size + length)) return NULL;
    char *frame = malloc((size_t)size + length);
    if (!frame) { ced_rpc_release((size_t)size + length); return NULL; }
    memcpy(frame, header, (size_t)size); memcpy(frame + size, body, length);
    *frame_length = (size_t)size + length;
    return frame;
}

void ced_rpc_frame_free(char *frame, size_t length) { free(frame); ced_rpc_release(length); }
