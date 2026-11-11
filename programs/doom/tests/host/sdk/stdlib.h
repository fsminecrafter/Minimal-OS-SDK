/* Host test only: stands in for the SDK's stdlib.h (SYS_HEAP there). */
#ifndef MINIMALOS_STDLIB_H
#define MINIMALOS_STDLIB_H
#include <stddef.h>
void* malloc(size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);
static inline int atoi(const char* s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}
#endif
