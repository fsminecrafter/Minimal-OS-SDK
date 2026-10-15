#ifndef MINIMALOS_SDK_H
#define MINIMALOS_SDK_H

#include <stdint.h>
#include <stddef.h>
#include "syscall.h"
// mos_syscall, mos_write, mos_exit, files, packages and framebuffer calls
// shared with the x86_64/user_*.h headers:
#include "x86_64/user_common.h"

static inline long mos_open(const char* path, long flags) {
    return mos_syscall(SYS_OPEN, (long)(uintptr_t)path, flags, 0);
}

static inline long mos_read(long fd, void* buf, size_t len) {
    return mos_syscall(SYS_READ, fd, (long)(uintptr_t)buf, (long)len);
}

static inline long mos_close(long fd) {
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

static inline long mos_seek(long fd, long offset) {
    return mos_syscall(SYS_SEEK, fd, offset, 0);
}

static inline long mos_size(long fd) {
    return mos_syscall(SYS_SIZE, fd, 0, 0);
}

static inline long mos_mkdir(const char* path) {
    return mos_syscall(SYS_MKDIR, (long)(uintptr_t)path, 0, 0);
}

static inline long mos_getcwd(char* path, size_t path_size) {
    return mos_syscall(SYS_GETCWD, (long)(uintptr_t)path, (long)path_size, 0);
}

// Creates an empty file. Open it afterwards with SYS_O_RDWR to write.
// filetype/fileformat may be NULL for "binary"/"bin".
static inline long mos_create(const char* path, const char* filetype,
                              const char* fileformat) {
    return mos_syscall(SYS_CREATE, (long)(uintptr_t)path,
                       (long)(uintptr_t)filetype, (long)(uintptr_t)fileformat);
}

// Launches another .run bundle as a new process and returns its pid.
// `argv`/`argc` are the extra arguments the program sees as argv[1..]
// (argv[0] is always the path itself). Pass NULL/0 for none.
static inline long mos_exec(const char* path, const char** argv, long argc) {
    return mos_syscall(SYS_EXEC, (long)(uintptr_t)path, (long)(uintptr_t)argv, argc);
}

static inline long mos_mouse_call(uint64_t operation, uint64_t arg1,
                                  uint64_t arg2) {
    return mos_syscall(SYS_MOUSE, (long)operation, (long)arg1, (long)arg2);
}

// --- Process management ---

// Lists up to `max_entries` currently known processes into `entries`.
// Returns the number of entries written, or a negative SYS_ERR_* value.
static inline long mos_pslist(syscall_process_info_t* entries,
                              size_t max_entries) {
    return mos_syscall(
        SYS_PSLIST,
        (long)(uintptr_t)entries,
        (long)max_entries,
        0
    );
}

// Convenience: write a NUL-terminated string to stdout.
static inline long mos_puts(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return mos_write(1, str, len);
}

#endif // MINIMALOS_SDK_H