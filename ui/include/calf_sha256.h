#ifndef CALF_SHA256_H
#define CALF_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t byte_count;
    unsigned char block[64];
    size_t block_used;
} calf_sha256_t;

void calf_sha256_init(calf_sha256_t *context);
void calf_sha256_update(calf_sha256_t *context, const void *data,
                        size_t length);
void calf_sha256_final(calf_sha256_t *context, unsigned char digest[32]);

#endif
