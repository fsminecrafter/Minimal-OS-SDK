#ifndef MSSL_CORE_H
#define MSSL_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MSSL_AES256_RK_WORDS 60   /* 4 * (14 rounds + 1) */

typedef struct { uint32_t rk[MSSL_AES256_RK_WORDS]; } aes256_key_t;
typedef struct { uint8_t table[16][16]; } ghash_key_t;

void aes256_expand_key(const uint8_t key[32], aes256_key_t* out);
void aes256_encrypt_block(const aes256_key_t* key, const uint8_t in[16], uint8_t out[16]);
void ghash_init(ghash_key_t* gk, const uint8_t h[16]);
void ghash_mul(const ghash_key_t* gk, uint8_t x[16]);   /* x = x * H */

/* Zeroization the optimizer is not allowed to elide. */
void mssl_cleanse(void* p, size_t n);

#endif