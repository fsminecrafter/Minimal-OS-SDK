#ifndef MINIMALOS_STDLIB_H
#define MINIMALOS_STDLIB_H

#include <stddef.h>
#include <stdint.h>
#include "minimalos.h"
#include "syscall.h"

/*
 * Dynamic memory for .run programs, over SYS_HEAP.
 *
 * Before this existed a program had only .bss and its stack, so
 * anything with a runtime-sized working set had to guess a maximum at
 * compile time. The allocation itself comes from the kernel heap
 * (every process shares the kernel PML4 today), so these pointers are
 * ordinary kernel-heap addresses.
 *
 * Errors come back as SYS_ERR_* values, which as addresses are up in
 * the top of the address space - mos_heap_failed() is the check, and
 * the malloc() wrappers below fold them to NULL for you.
 */

static inline int mos_heap_failed(long value) {
    return value < 0 && value >= (long)-16;
}

static inline void* malloc(size_t size) {
    long r = mos_syscall(SYS_HEAP, SYS_HEAP_ALLOC, 0, (long)size);
    if (mos_heap_failed(r)) return NULL;
    return (void*)(uintptr_t)r;
}

static inline void* calloc(size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size) return NULL;   // overflow
    long r = mos_syscall(SYS_HEAP, SYS_HEAP_ALLOC_ZEROED, 0, (long)total);
    if (mos_heap_failed(r)) return NULL;
    return (void*)(uintptr_t)r;
}

// On failure the original pointer is still valid and unchanged.
static inline void* realloc(void* ptr, size_t size) {
    long r = mos_syscall(SYS_HEAP, SYS_HEAP_RESIZE, (long)(uintptr_t)ptr, (long)size);
    if (mos_heap_failed(r)) return NULL;
    return (void*)(uintptr_t)r;
}

static inline void free(void* ptr) {
    mos_syscall(SYS_HEAP, SYS_HEAP_FREE, (long)(uintptr_t)ptr, 0);
}

// Cryptographic-quality where the CPU has RDRAND; TSC/RTC-derived
// otherwise. Returns the number of bytes written.
static inline long mos_random_bytes(void* buf, size_t len) {
    return mos_syscall(SYS_RANDOM, (long)(uintptr_t)buf, (long)len, 0);
}

static inline uint64_t mos_random_u64(void) {
    uint64_t v = 0;
    mos_random_bytes(&v, sizeof(v));
    return v;
}

static inline int atoi(const char* s) {
    int sign = 1, value = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') value = value * 10 + (*s++ - '0');
    return value * sign;
}

#endif // MINIMALOS_STDLIB_H
