#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void tohex(const unsigned char* b, int n, char* o) {
    static const char* d = "0123456789abcdef";
    for (int i = 0; i < n; i++) { o[2*i] = d[b[i] >> 4]; o[2*i+1] = d[b[i] & 15]; }
    o[2*n] = 0;
}

static int seal(const unsigned char* key, const unsigned char* iv,
                const unsigned char* aad, int aadl,
                const unsigned char* pt, int ptl, int chunk,
                unsigned char* ct, unsigned char* tag) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int ok = 0, l = 0, tot = 0;
    if (!c) return 0;
    if (!EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL)) goto out;
    if (!EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL)) goto out;
    if (!EVP_EncryptInit_ex(c, NULL, NULL, key, iv)) goto out;
    if (aadl && !EVP_EncryptUpdate(c, NULL, &l, aad, aadl)) goto out;
    for (int o = 0; o < ptl; o += chunk) {
        int n = (ptl - o < chunk) ? ptl - o : chunk;
        if (!EVP_EncryptUpdate(c, ct + tot, &l, pt + o, n)) goto out;
        tot += l;
    }
    if (!EVP_EncryptFinal_ex(c, ct + tot, &l)) goto out;
    ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
out:
    EVP_CIPHER_CTX_free(c);
    return ok;
}

static int unseal(const unsigned char* key, const unsigned char* iv,
                  const unsigned char* aad, int aadl,
                  const unsigned char* ct, int ctl, int chunk,
                  const unsigned char* tag, unsigned char* pt) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    unsigned char t[16];
    int ok = 0, l = 0, tot = 0;
    if (!c) return 0;
    memcpy(t, tag, 16);
    if (!EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, iv)) goto out;
    if (aadl && !EVP_DecryptUpdate(c, NULL, &l, aad, aadl)) goto out;
    for (int o = 0; o < ctl; o += chunk) {
        int n = (ctl - o < chunk) ? ctl - o : chunk;
        if (!EVP_DecryptUpdate(c, pt + tot, &l, ct + o, n)) goto out;
        tot += l;
    }
    if (!EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, t)) goto out;
    ok = EVP_DecryptFinal_ex(c, pt + tot, &l);
out:
    EVP_CIPHER_CTX_free(c);
    return ok;
}

int main(void) {
    char hex[130];
    unsigned char md[32], key[32], iv[12], ct[256], tag[16], pt[256];

    /* SHA-256("abc") */
    SHA256((const unsigned char*)"abc", 3, md);
    tohex(md, 32, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    /* GCM spec test cases 13 and 14 (AES-256, zero key/IV) */
    memset(key, 0, 32); memset(iv, 0, 12);
    memset(pt, 0, 16);
    CHECK(seal(key, iv, NULL, 0, NULL, 0, 1, ct, tag));
    tohex(tag, 16, hex);
    CHECK(strcmp(hex, "530f8afbc74536b9a963b4f1c4cb738b") == 0);

    CHECK(seal(key, iv, NULL, 0, pt, 16, 16, ct, tag));
    tohex(ct, 16, hex);
    CHECK(strcmp(hex, "cea7403d4d606b6e074ec5d3baf39d18") == 0);
    tohex(tag, 16, hex);
    CHECK(strcmp(hex, "d0d1c8a799996bf0265b98b5d48ab919") == 0);

    /* Chunking must not change the result; tampering must be rejected. */
    unsigned char msg[100], aad[20], ct1[100], ct2[100], t1[16], t2[16], back[100];
    CHECK(RAND_bytes(key, 32) && RAND_bytes(iv, 12));
    for (int i = 0; i < 100; i++) msg[i] = (unsigned char)(i * 7 + 3);
    for (int i = 0; i < 20; i++)  aad[i] = (unsigned char)(i + 1);

    CHECK(seal(key, iv, aad, 20, msg, 100, 100, ct1, t1));
    CHECK(seal(key, iv, aad, 20, msg, 100, 7,   ct2, t2));
    CHECK(memcmp(ct1, ct2, 100) == 0 && memcmp(t1, t2, 16) == 0);

    CHECK(unseal(key, iv, aad, 20, ct1, 100, 13, t1, back));
    CHECK(memcmp(back, msg, 100) == 0);

    ct1[50] ^= 1;   CHECK(!unseal(key, iv, aad, 20, ct1, 100, 13, t1, back)); ct1[50] ^= 1;
    aad[3] ^= 1;    CHECK(!unseal(key, iv, aad, 20, ct1, 100, 13, t1, back)); aad[3] ^= 1;
    t1[15] ^= 1;    CHECK(!unseal(key, iv, aad, 20, ct1, 100, 13, t1, back));

    printf(fails ? "%d FAILURE(S)\n" : "all ok\n", fails);
    return fails != 0;
}