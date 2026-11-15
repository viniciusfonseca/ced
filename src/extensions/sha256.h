#ifndef CED_EXTENSION_SHA256_H
#define CED_EXTENSION_SHA256_H

#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t state[8]; uint64_t length; unsigned char block[64]; size_t used; } CedSha256;
void ced_sha256_init(CedSha256 *hash);
void ced_sha256_update(CedSha256 *hash, const void *bytes, size_t length);
void ced_sha256_finish(CedSha256 *hash, char output[65]);

#endif
