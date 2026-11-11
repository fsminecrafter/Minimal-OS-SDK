/* Host test only: the SDK's string.h functions. */
#ifndef MINIMALOS_STRING_H
#define MINIMALOS_STRING_H
#include <stddef.h>
static inline size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
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
static inline char* strcpy(char* d, const char* s) { char* o = d; while ((*d++ = *s++)) { } return o; }
static inline char* strchr(const char* s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    return c == 0 ? (char*)s : NULL;
}
static inline char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (; *s; s++) if (*s == (char)c) last = s;
    return c == 0 ? (char*)s : (char*)last;
}
static inline char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char* a = h; const char* b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return NULL;
}
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);
int memcmp(const void* a, const void* b, size_t n);
#endif
