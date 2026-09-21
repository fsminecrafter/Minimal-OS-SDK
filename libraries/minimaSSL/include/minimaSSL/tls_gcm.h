#ifndef MINIMASSL_TLS_GCM_H
#define MINIMASSL_TLS_GCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mssl_aes128_gcm_encrypt_aad(const uint8_t key[16], const uint8_t iv[12],
                                 const uint8_t* aad, size_t aad_len,
                                 const uint8_t* pt, size_t len,
                                 uint8_t* ct, uint8_t tag[16]);

int mssl_aes128_gcm_decrypt_aad(const uint8_t key[16], const uint8_t iv[12],
                                const uint8_t* aad, size_t aad_len,
                                const uint8_t* ct, size_t len,
                                const uint8_t tag[16], uint8_t* pt);

#ifdef __cplusplus
}
#endif
#endif
