#ifndef MINIMASSL_SHA_H
#define MINIMASSL_SHA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LENGTH 32

/*
 * Public SHA-256 context layout. Keep this ABI-compatible with the
 * internal dlr_sha256_ctx without making consumers depend on src/crypto.h.
 */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buffered;
} SHA256_CTX;

/* All return 1 on success, 0 on failure (OpenSSL convention). */
int SHA256_Init(SHA256_CTX* c);
int SHA256_Update(SHA256_CTX* c, const void* data, size_t len);

/* Writes 32 bytes to md and zeroizes *c afterwards, like OpenSSL. */
int SHA256_Final(unsigned char* md, SHA256_CTX* c);

/* One-shot. If md is NULL a static buffer is used (NOT thread-safe,
 * same as OpenSSL). Returns md, or NULL on failure. */
unsigned char* SHA256(const unsigned char* d, size_t n, unsigned char* md);

#ifdef __cplusplus
}
#endif
#endif /* MINIMASSL_SHA_H */