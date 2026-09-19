#include <openssl/sha.h>      /* NOT <openssl/sha.h> - that would clash with ours */
#include "crypto.h"

/* Zeroing that the optimizer cannot remove as a dead store. If
 * mssl_core.h already provides mssl_cleanse(), you can use that instead. */
static void sha_cleanse(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

int SHA256_Init(SHA256_CTX* c) {
    if (!c) return 0;
    dlr_sha256_init((dlr_sha256_ctx*)c);
    return 1;
}

int SHA256_Update(SHA256_CTX* c, const void* data, size_t len) {
    static const unsigned char empty = 0;
    if (!c) return 0;
    if (!data) {
        if (len) return 0;      /* non-zero length with NULL data is an error */
        data = &empty;          /* avoid memcpy(NULL, ..., 0) inside update */
    }
    dlr_sha256_update((dlr_sha256_ctx*)c, data, len);
    return 1;
}

int SHA256_Final(unsigned char* md, SHA256_CTX* c) {
    if (!md || !c) return 0;
    dlr_sha256_final((dlr_sha256_ctx*)c, md);
    sha_cleanse(c, sizeof *c);
    return 1;
}

unsigned char* SHA256(const unsigned char* d, size_t n, unsigned char* md) {
    static unsigned char static_md[SHA256_DIGEST_LENGTH];
    SHA256_CTX c;
    if (!md) md = static_md;
    if (!SHA256_Init(&c) ||
        !SHA256_Update(&c, d, n) ||
        !SHA256_Final(md, &c)) {
        sha_cleanse(&c, sizeof c);
        return NULL;
    }
    return md;
}