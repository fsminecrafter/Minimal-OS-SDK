#ifndef MINIMALOS_STRING_H
#define MINIMALOS_STRING_H

#include <stddef.h>
#include <stdint.h>

/*
 * Freestanding string/memory helpers.
 *
 * These are `static inline`, so including this header never emits a
 * duplicate symbol. GCC also synthesizes calls to memcpy/memset/
 * memmove/memcmp on its own - for struct assignment, array init and
 * so on - even under -fno-builtin, and those need real out-of-line
 * symbols to link against. crt0.c defines them, so every .run program
 * gets them whether or not it includes this header.
 */

static inline size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static inline char* strcpy(char* dst, const char* src) {
    char* out = dst;
    while ((*dst++ = *src++)) { }
    return out;
}

// Always NUL-terminates, unlike the C library's strncpy. Truncates if
// the source does not fit.
static inline char* strlcpy_safe(char* dst, const char* src, size_t size) {
    if (size == 0) return dst;
    size_t i = 0;
    for (; i + 1 < size && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

static inline char* strchr(const char* s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    return (c == 0) ? (char*)s : NULL;
}

static inline char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (; *s; s++) if (*s == (char)c) last = s;
    return (char*)last;
}

static inline char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

static inline int mos_memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

/*
 * Constant-time comparison. Use this and not memcmp for anything
 * secret - an auth tag, a password hash, a MAC. The early-exit in an
 * ordinary compare leaks how many leading bytes matched, which is
 * enough to forge a tag one byte at a time.
 */
static inline int mos_memeq_ct(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}

// Out-of-line versions supplied by crt0.c.
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

#endif // MINIMALOS_STRING_H
