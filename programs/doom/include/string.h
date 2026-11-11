/* Doom for Minimal-OS: <string.h> beyond the SDK's. */
#ifndef DOOM_LIBC_STRING_H
#define DOOM_LIBC_STRING_H
#include_next <string.h>
#include <stddef.h>

char* strncpy(char* dst, const char* src, size_t n);
char* strcat(char* dst, const char* src);
char* strncat(char* dst, const char* src, size_t n);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);
char* strerror(int err);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
void* memchr(const void* s, int c, size_t n);
#endif
