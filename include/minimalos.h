#ifndef MINIMALOS_SDK_H
#define MINIMALOS_SDK_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"

static inline long mos_syscall(long num, long a1, long a2, long a3) {
    long ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline long mos_write(int fd, const void* buf, size_t len) {
    return mos_syscall(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)len);
}

static inline long mos_getpid(void) {
    return mos_syscall(SYS_GETPID, 0, 0, 0);
}

static inline long mos_uptime(void) {
    return mos_syscall(SYS_UPTIME, 0, 0, 0);
}

static inline long mos_sleep(long ticks) {
    return mos_syscall(SYS_SLEEP, ticks, 0, 0);
}

static inline long mos_open(const char* path, long flags) {
    return mos_syscall(SYS_OPEN, (long)(uintptr_t)path, flags, 0);
}

static inline long mos_read(int fd, void* buf, size_t len) {
    return mos_syscall(SYS_READ, fd, (long)(uintptr_t)buf, (long)len);
}

static inline long mos_close(int fd) {
    return mos_syscall(SYS_CLOSE, fd, 0, 0);
}

static inline long mos_exists(const char* path) {
    return mos_syscall(SYS_EXISTS, (long)(uintptr_t)path, 0, 0);
}

static inline long mos_is_dir(const char* path) {
    return mos_syscall(SYS_IS_DIR, (long)(uintptr_t)path, 0, 0);
}

static inline long mos_gettime(void) {
    return mos_syscall(SYS_GETTIME, 0, 0, 0);
}

static inline long mos_seek(int fd, long offset) {
    return mos_syscall(SYS_SEEK, fd, offset, 0);
}

static inline long mos_size(int fd) {
    return mos_syscall(SYS_SIZE, fd, 0, 0);
}

static inline long mos_mkdir(const char* path) {
    return mos_syscall(SYS_MKDIR, (long)(uintptr_t)path, 0, 0);
}

static inline void mos_exit(int code) {
    mos_syscall(SYS_EXIT, code, 0, 0);
    for (;;) { } // never reached - SYS_EXIT never returns
}

// Convenience: write a NUL-terminated string to stdout.
static inline long mos_puts(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return mos_write(1, str, len);
}

#endif // MINIMALOS_SDK_H