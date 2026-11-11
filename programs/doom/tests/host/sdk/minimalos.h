/* Host test only: the Minimal-OS calls the Doom port uses, on raw Linux
 * system calls, so the port's C library (libc/) runs unchanged. */
#ifndef MINIMALOS_SDK_H
#define MINIMALOS_SDK_H
#include <stddef.h>
#include <stdint.h>

static inline long lx_call(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

#define SYS_O_RDONLY 0
#define SYS_O_RDWR   1

static inline long mos_open(const char* path, long flags) {
    long fd = lx_call(2, (long)path, flags ? 2 : 0, 0);     /* open: O_RDWR / O_RDONLY */
    return fd < 0 ? -2 : fd;
}
static inline long mos_read(long fd, void* buf, size_t len) { return lx_call(0, fd, (long)buf, (long)len); }
static inline long mos_close(long fd) { return lx_call(3, fd, 0, 0); }
static inline long mos_fwrite(long fd, const void* buf, size_t len) { return lx_call(1, fd, (long)buf, (long)len); }
static inline long mos_seek(long fd, long offset) { return lx_call(8, fd, offset, 0) < 0 ? -1 : 0; }
static inline long mos_size(long fd) {
    long cur = lx_call(8, fd, 0, 1), end = lx_call(8, fd, 0, 2);
    lx_call(8, fd, cur, 0);
    return end;
}
static inline long mos_exists(const char* path) { return lx_call(21, (long)path, 0, 0) == 0 ? 1 : 0; }
static inline long mos_is_dir(const char* path) {
    long fd = lx_call(2, (long)path, 0200000 /* O_DIRECTORY */, 0);
    if (fd < 0) return 0;
    lx_call(3, fd, 0, 0);
    return 1;
}
static inline long mos_mkdir(const char* path) { return lx_call(83, (long)path, 0755, 0); }
static inline long mos_delete(const char* path) { return lx_call(87, (long)path, 0, 0); }
static inline long mos_create(const char* path, const char* t, const char* f) {
    (void)t; (void)f;
    long fd = lx_call(2, (long)path, 0101 /* O_WRONLY|O_CREAT */, 0644);
    if (fd < 0) return -1;
    lx_call(3, fd, 0, 0);
    return 0;
}
#define mos_write(fd, buf, len) lx_call(1, (fd) == 2 ? 2 : 1, (long)(buf), (long)(len))
__attribute__((noreturn)) static inline void mos_exit_with(int code) {
    lx_call(231, code, 0, 0);
    for (;;) { }
}
long test_uptime_ms(void);
static inline long mos_uptime(void) { return test_uptime_ms(); }
static inline long mos_sleep(long ms) { (void)ms; return 0; }
static inline long mos_log(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    lx_call(1, 2, (long)"[log] ", 6);
    lx_call(1, 2, (long)s, (long)n);
    lx_call(1, 2, (long)"\n", 1);
    return 0;
}
#endif
