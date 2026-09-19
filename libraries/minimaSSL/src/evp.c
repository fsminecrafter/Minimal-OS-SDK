#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include "mssl_core.h"

#define GCM_IV_LEN   12
#define GCM_TAG_LEN  16
/* SP 800-38D limits: 2^36-32 bytes of ciphertext, AAD < 2^61 bytes. */
#define GCM_MAX_MSG_BYTES (((uint64_t)1 << 36) - 32)
#define GCM_MAX_AAD_BYTES ((uint64_t)1 << 61)

enum { PH_NONE = 0, PH_AAD, PH_DATA, PH_DONE };

struct evp_cipher_st { const char* name; };
static const struct evp_cipher_st g_aes_256_gcm = { "AES-256-GCM" };

struct evp_cipher_ctx_st {
    int cipher_set, key_set, iv_set;
    int encrypt;
    int phase;
    int tag_set;     /* decrypt: caller supplied an expected tag */
    int tag_valid;   /* encrypt: tag has been computed */

    aes256_key_t aes;
    ghash_key_t  gk;

    uint8_t iv[GCM_IV_LEN];
    uint8_t j0[16];
    uint8_t ctr[16];
    uint8_t y[16];          /* GHASH accumulator */
    uint8_t pend[16];       /* partial GHASH block */
    size_t  pend_len;
    uint8_t ks[16];         /* current keystream block */
    size_t  ks_used;        /* 16 == exhausted */
    uint8_t tag[GCM_TAG_LEN];     /* computed tag (encrypt) */
    uint8_t tag_in[GCM_TAG_LEN];  /* expected tag (decrypt) */

    uint64_t aad_len, ct_len;
};

void mssl_cleanse(void* p, size_t n) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    while (n--) *v++ = 0;
}

const EVP_CIPHER* EVP_aes_256_gcm(void) { return &g_aes_256_gcm; }

EVP_CIPHER_CTX* EVP_CIPHER_CTX_new(void) {
    EVP_CIPHER_CTX* c = (EVP_CIPHER_CTX*)malloc(sizeof *c);
    if (c) memset(c, 0, sizeof *c);
    return c;
}

void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX* c) {
    if (!c) return;
    mssl_cleanse(c, sizeof *c);
    free(c);
}

int EVP_CIPHER_CTX_reset(EVP_CIPHER_CTX* c) {
    if (!c) return 0;
    mssl_cleanse(c, sizeof *c);
    return 1;
}

int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX* c, int pad) {
    (void)pad;
    return c != NULL;
}

/* ---- GHASH helpers ---- */

static void ghash_block(EVP_CIPHER_CTX* c, const uint8_t blk[16]) {
    for (int i = 0; i < 16; i++) c->y[i] ^= blk[i];
    ghash_mul(&c->gk, c->y);
}

static void ghash_absorb(EVP_CIPHER_CTX* c, const uint8_t* d, size_t n) {
    while (n > 0) {
        size_t take = 16 - c->pend_len;
        if (take > n) take = n;
        memcpy(c->pend + c->pend_len, d, take);
        c->pend_len += take;
        d += take;
        n -= take;
        if (c->pend_len == 16) {
            ghash_block(c, c->pend);
            c->pend_len = 0;
        }
    }
}

/* AAD and ciphertext are each zero-padded to a block boundary. */
static void ghash_flush_partial(EVP_CIPHER_CTX* c) {
    if (c->pend_len == 0) return;
    memset(c->pend + c->pend_len, 0, 16 - c->pend_len);
    ghash_block(c, c->pend);
    c->pend_len = 0;
}

static void gcm_inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i]) break;
    }
}

/* Begins a fresh message. Requires key and IV. */
static void start_message(EVP_CIPHER_CTX* c) {
    uint8_t zero[16], h[16];
    memset(zero, 0, sizeof zero);
    aes256_encrypt_block(&c->aes, zero, h);
    ghash_init(&c->gk, h);
    mssl_cleanse(h, sizeof h);

    memcpy(c->j0, c->iv, GCM_IV_LEN);
    c->j0[12] = 0; c->j0[13] = 0; c->j0[14] = 0; c->j0[15] = 1;
    memcpy(c->ctr, c->j0, 16);

    memset(c->y, 0, sizeof c->y);
    c->pend_len  = 0;
    c->ks_used   = 16;
    c->aad_len   = 0;
    c->ct_len    = 0;
    c->tag_valid = 0;
    c->phase     = PH_AAD;
}

static int init_ex(EVP_CIPHER_CTX* c, const EVP_CIPHER* cipher,
                   const unsigned char* key, const unsigned char* iv, int enc) {
    if (!c) return 0;

    if (cipher) {
        if (cipher != &g_aes_256_gcm) return 0;
        mssl_cleanse(c, sizeof *c);          /* new cipher == fresh context */
        c->cipher_set = 1;
    } else if (!c->cipher_set) {
        return 0;
    }

    /* Flipping direction in the middle of a message would be nonsense. */
    if (!cipher && !key && !iv && c->encrypt != enc &&
        (c->phase == PH_AAD || c->phase == PH_DATA)) {
        return 0;
    }
    c->encrypt = enc;

    if (key) {
        aes256_expand_key(key, &c->aes);
        c->key_set = 1;
    }
    if (iv) {
        memcpy(c->iv, iv, GCM_IV_LEN);
        c->iv_set = 1;
    }
    if ((key || iv) && c->key_set && c->iv_set) start_message(c);
    return 1;
}

int EVP_EncryptInit_ex(EVP_CIPHER_CTX* c, const EVP_CIPHER* cipher, void* impl,
                       const unsigned char* key, const unsigned char* iv) {
    (void)impl;
    return init_ex(c, cipher, key, iv, 1);
}

int EVP_DecryptInit_ex(EVP_CIPHER_CTX* c, const EVP_CIPHER* cipher, void* impl,
                       const unsigned char* key, const unsigned char* iv) {
    (void)impl;
    return init_ex(c, cipher, key, iv, 0);
}

/* True if [out,out+n) and [in,in+n) overlap without being identical. */
static int partial_overlap(const uint8_t* out, const uint8_t* in, size_t n) {
    if (out == in || n == 0) return 0;
    uintptr_t o = (uintptr_t)out, i = (uintptr_t)in;
    return (o < i) ? ((i - o) < n) : ((o - i) < n);
}

static int gcm_update(EVP_CIPHER_CTX* c, int want_enc, unsigned char* out,
                      int* outl, const unsigned char* in, int inl) {
    if (!c || !outl) return 0;
    *outl = 0;
    if (!c->cipher_set || c->encrypt != want_enc) return 0;
    if (c->phase != PH_AAD && c->phase != PH_DATA) return 0;
    if (inl < 0) return 0;
    if (inl == 0) return 1;
    if (!in) return 0;
    size_t n = (size_t)inl;

    if (!out) {                                   /* AAD */
        if (c->phase != PH_AAD) return 0;         /* AAD after data is illegal */
        if (c->aad_len > GCM_MAX_AAD_BYTES - n) return 0;
        ghash_absorb(c, in, n);
        c->aad_len += n;
        *outl = inl;
        return 1;
    }

    if (c->ct_len > GCM_MAX_MSG_BYTES - n) return 0;
    if (partial_overlap(out, in, n)) return 0;

    if (c->phase == PH_AAD) {
        ghash_flush_partial(c);
        c->phase = PH_DATA;
    }

    for (size_t i = 0; i < n; i++) {
        if (c->ks_used == 16) {
            gcm_inc32(c->ctr);
            aes256_encrypt_block(&c->aes, c->ctr, c->ks);
            c->ks_used = 0;
        }
        /* Read in[i] before writing out[i]: in-place (out == in) is allowed. */
        uint8_t b = in[i];
        uint8_t o = (uint8_t)(b ^ c->ks[c->ks_used++]);
        c->pend[c->pend_len++] = c->encrypt ? o : b;   /* GHASH covers ciphertext */
        if (c->pend_len == 16) {
            ghash_block(c, c->pend);
            c->pend_len = 0;
        }
        out[i] = o;
    }

    c->ct_len += n;
    *outl = inl;
    return 1;
}

int EVP_EncryptUpdate(EVP_CIPHER_CTX* c, unsigned char* out, int* outl,
                      const unsigned char* in, int inl) {
    return gcm_update(c, 1, out, outl, in, inl);
}

int EVP_DecryptUpdate(EVP_CIPHER_CTX* c, unsigned char* out, int* outl,
                      const unsigned char* in, int inl) {
    return gcm_update(c, 0, out, outl, in, inl);
}

static int gcm_final(EVP_CIPHER_CTX* c, int want_enc, int* outl) {
    if (!c || !outl) return 0;
    *outl = 0;
    if (!c->cipher_set || c->encrypt != want_enc) return 0;
    if (c->phase != PH_AAD && c->phase != PH_DATA) return 0;

    ghash_flush_partial(c);

    uint8_t lens[16];
    uint64_t abits = c->aad_len * 8u, cbits = c->ct_len * 8u;
    for (int i = 0; i < 8; i++) {
        lens[7 - i]  = (uint8_t)(abits >> (i * 8));
        lens[15 - i] = (uint8_t)(cbits >> (i * 8));
    }
    ghash_block(c, lens);

    uint8_t s[16], tag[GCM_TAG_LEN];
    aes256_encrypt_block(&c->aes, c->j0, s);
    for (int i = 0; i < GCM_TAG_LEN; i++) tag[i] = (uint8_t)(c->y[i] ^ s[i]);

    c->phase = PH_DONE;
    int ok;
    if (want_enc) {
        memcpy(c->tag, tag, GCM_TAG_LEN);
        c->tag_valid = 1;
        ok = 1;
    } else {
        uint8_t diff = 0;                         /* constant-time compare */
        for (int i = 0; i < GCM_TAG_LEN; i++) diff |= (uint8_t)(tag[i] ^ c->tag_in[i]);
        ok = (c->tag_set != 0) && (diff == 0);
        c->tag_set = 0;
        mssl_cleanse(c->tag_in, sizeof c->tag_in);
    }

    mssl_cleanse(s, sizeof s);
    mssl_cleanse(tag, sizeof tag);
    mssl_cleanse(c->y, sizeof c->y);
    mssl_cleanse(c->pend, sizeof c->pend);
    mssl_cleanse(c->ks, sizeof c->ks);
    return ok;
}

int EVP_EncryptFinal_ex(EVP_CIPHER_CTX* c, unsigned char* out, int* outl) {
    (void)out;
    return gcm_final(c, 1, outl);
}

int EVP_DecryptFinal_ex(EVP_CIPHER_CTX* c, unsigned char* out, int* outl) {
    (void)out;
    return gcm_final(c, 0, outl);
}

int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX* c, int type, int arg, void* ptr) {
    if (!c || !c->cipher_set) return 0;
    switch (type) {
        case EVP_CTRL_AEAD_SET_IVLEN:
            return arg == GCM_IV_LEN;
        case EVP_CTRL_AEAD_GET_TAG:
            if (!c->encrypt || c->phase != PH_DONE || !c->tag_valid) return 0;
            if (arg != GCM_TAG_LEN || !ptr) return 0;
            memcpy(ptr, c->tag, GCM_TAG_LEN);
            return 1;
        case EVP_CTRL_AEAD_SET_TAG:
            if (c->encrypt || arg != GCM_TAG_LEN || !ptr) return 0;
            memcpy(c->tag_in, ptr, GCM_TAG_LEN);
            c->tag_set = 1;
            return 1;
        default:
            return 0;
    }
}