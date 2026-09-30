#ifndef FS_SHA256_H
#define FS_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t st[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   blen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

// HMAC-SHA256 over the concatenation a||b (b may be empty). 
void hmac_sha256_2(const uint8_t *key, size_t klen,
                   const void *a, size_t alen,
                   const void *b, size_t blen,
                   uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen,
                 const void *msg, size_t mlen, uint8_t out[32]);

// Constant-time comparison. Returns 1 if equal. 
int ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

#endif
