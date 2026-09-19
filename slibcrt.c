/*
 * Freestanding runtime helpers for .slib libraries.
 *
 * crt0.c cannot be used for shared libraries because it contains
 * _start() and calls main(). GCC can still emit calls to these
 * memory functions under -ffreestanding -fno-builtin, so .slib
 * needs its own tiny runtime implementation.
 */

void* memcpy(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    for (unsigned long i = 0; i < n; ++i)
        d[i] = s[i];

    return dst;
}

void* memmove(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {
        for (unsigned long i = 0; i < n; ++i)
            d[i] = s[i];
    } else {
        for (unsigned long i = n; i > 0; --i)
            d[i - 1] = s[i - 1];
    }

    return dst;
}

void* memset(void* dst, int value, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;

    for (unsigned long i = 0; i < n; ++i)
        d[i] = (unsigned char)value;

    return dst;
}

int memcmp(const void* a, const void* b, unsigned long n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;

    for (unsigned long i = 0; i < n; ++i) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }

    return 0;
}

