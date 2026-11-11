/* Doom for Minimal-OS: <stdlib.h> beyond the SDK's malloc/free/atoi. */
#ifndef DOOM_LIBC_STDLIB_H
#define DOOM_LIBC_STDLIB_H
#include_next <stdlib.h>
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFF

__attribute__((noreturn)) void exit(int code);
__attribute__((noreturn)) void abort(void);
int atexit(void (*fn)(void));
char* getenv(const char* name);
int system(const char* command);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
double atof(const char* s);
double strtod(const char* s, char** end);
int rand(void);
void srand(unsigned seed);
void qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*));
static inline int abs(int v) { return v < 0 ? -v : v; }
static inline long labs(long v) { return v < 0 ? -v : v; }
#endif
