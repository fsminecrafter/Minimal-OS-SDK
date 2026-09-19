#ifndef DLR_CRYPTO_H
#define DLR_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buffered;
} dlr_sha256_ctx;

void dlr_sha256_init(dlr_sha256_ctx* ctx);
void dlr_sha256_update(dlr_sha256_ctx* ctx, const void* data, size_t len);
void dlr_sha256_final(dlr_sha256_ctx* ctx, uint8_t out[32]);
void dlr_sha256(const void* data, size_t len, uint8_t out[32]);

// Lowercase hex, NUL-terminated. `out` must be at least 65 bytes.
// Deliver compares these as strings over the wire.
void dlr_hex(const uint8_t* bytes, size_t len, char* out);

// ---------------------------------------------------------------------------
// AES-256-GCM
// ---------------------------------------------------------------------------

#define DLR_GCM_IV_LEN  12
#define DLR_GCM_TAG_LEN 16
#define DLR_KEY_LEN     32

// Encrypt/decrypt in place is allowed (ct may alias pt).
void dlr_aes256_gcm_encrypt(const uint8_t key[32], const uint8_t iv[12],
                            const uint8_t* pt, size_t len,
                            uint8_t* ct, uint8_t tag[16]);

// Returns 1 if the tag verifies, 0 otherwise. On failure the output
// buffer's contents are undefined and must not be used - a GCM
// plaintext is only meaningful once the tag has been checked.
int dlr_aes256_gcm_decrypt(const uint8_t key[32], const uint8_t iv[12],
                           const uint8_t* ct, size_t len,
                           const uint8_t tag[16], uint8_t* pt);

/*
 * Deliver's framing: an encrypted frame is
 *     nonce(12) || ciphertext || tag(16)
 * with no associated data. dlr_seal/dlr_open wrap the above in that
 * layout so the call sites never have to slice the buffer by hand.
 */

// out must have room for len + 28 bytes. Returns the total length.
size_t dlr_seal(const uint8_t key[32], const uint8_t* pt, size_t len, uint8_t* out);

// Returns the plaintext length, or -1 if the frame is too short or the
// tag does not verify. out must have room for len - 28 bytes.
long dlr_open(const uint8_t key[32], const uint8_t* frame, size_t len, uint8_t* out);

// ---------------------------------------------------------------------------
// Base64 (standard alphabet, '=' padded)
// ---------------------------------------------------------------------------

// Returns the number of characters written (NUL-terminated).
size_t dlr_b64_encode(const uint8_t* data, size_t len, char* out, size_t out_size);

// Returns the number of bytes decoded, or -1 if out_size is too small.
long dlr_b64_decode(const char* text, uint8_t* out, size_t out_size);


// ---------------------------------------------------------------------------
// Low-level primitives (exported for the EVP shim's incremental GCM)
// ---------------------------------------------------------------------------

#define AES256_ROUNDS   14
#define AES256_RK_WORDS (4 * (AES256_ROUNDS + 1))   /* 60 */

typedef struct {
    uint32_t rk[AES256_RK_WORDS];
} aes256_key_t;

typedef struct {
    uint8_t table[16][16];   /* table[i] = H * i in GCM bit ordering */
} ghash_key_t;

void aes256_expand_key(const uint8_t key[32], aes256_key_t* out);
void aes256_encrypt_block(const aes256_key_t* key,
                          const uint8_t in[16], uint8_t out[16]);
void ghash_init(ghash_key_t* gk, const uint8_t h[16]);
void ghash_mul(const ghash_key_t* gk, uint8_t x[16]);   /* x = x * H */


#endif // DLR_CRYPTO_H