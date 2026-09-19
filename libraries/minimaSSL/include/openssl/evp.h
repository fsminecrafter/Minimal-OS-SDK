#ifndef MINIMASSL_EVP_H
#define MINIMASSL_EVP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evp_cipher_st     EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

#define EVP_CTRL_AEAD_SET_IVLEN 0x9
#define EVP_CTRL_AEAD_GET_TAG   0x10
#define EVP_CTRL_AEAD_SET_TAG   0x11
#define EVP_CTRL_GCM_SET_IVLEN  EVP_CTRL_AEAD_SET_IVLEN
#define EVP_CTRL_GCM_GET_TAG    EVP_CTRL_AEAD_GET_TAG
#define EVP_CTRL_GCM_SET_TAG    EVP_CTRL_AEAD_SET_TAG

const EVP_CIPHER* EVP_aes_256_gcm(void);

EVP_CIPHER_CTX* EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX* ctx);      /* NULL-safe, zeroizes */
int  EVP_CIPHER_CTX_reset(EVP_CIPHER_CTX* ctx);
int  EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX* ctx, int pad);  /* no-op for GCM */
int  EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX* ctx, int type, int arg, void* ptr);

/* Key and IV may be supplied in separate calls (cipher first, then key/iv). */
int EVP_EncryptInit_ex(EVP_CIPHER_CTX* ctx, const EVP_CIPHER* cipher, void* impl,
                       const unsigned char* key, const unsigned char* iv);
int EVP_DecryptInit_ex(EVP_CIPHER_CTX* ctx, const EVP_CIPHER* cipher, void* impl,
                       const unsigned char* key, const unsigned char* iv);

/* out == NULL means "this input is AAD". */
int EVP_EncryptUpdate(EVP_CIPHER_CTX* ctx, unsigned char* out, int* outl,
                      const unsigned char* in, int inl);
int EVP_DecryptUpdate(EVP_CIPHER_CTX* ctx, unsigned char* out, int* outl,
                      const unsigned char* in, int inl);

int EVP_EncryptFinal_ex(EVP_CIPHER_CTX* ctx, unsigned char* out, int* outl);
/* Returns 0 if the tag does not verify - the plaintext must then be discarded. */
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX* ctx, unsigned char* out, int* outl);

#ifdef __cplusplus
}
#endif
#endif