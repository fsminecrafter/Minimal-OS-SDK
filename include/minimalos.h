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

// --- MinimaFS extensions ---

// Write to a file opened with mos_open(). Distinct from mos_write(),
// which is fixed to fd 1/2 (stdout/stderr -> terminal).
static inline long mos_fwrite(int fd, const void* buf, size_t len) {
    return mos_syscall(SYS_FWRITE, fd, (long)(uintptr_t)buf, (long)len);
}

// Lists up to `max_entries` entries of the directory at `path` into
// `entries` (an array of syscall_dirent_t - see syscall.h). Returns
// the number of entries written, or a negative SYS_ERR_* value.
static inline long mos_listdir(const char* path, syscall_dirent_t* entries,
                               size_t max_entries) {
    return mos_syscall(SYS_LISTDIR, (long)(uintptr_t)path,
                       (long)(uintptr_t)entries, (long)max_entries);
}

static inline long mos_delete(const char* path) {
    return mos_syscall(SYS_DELETE, (long)(uintptr_t)path, 0, 0);
}

static inline long mos_rmdir(const char* path) {
    return mos_syscall(SYS_RMDIR, (long)(uintptr_t)path, 0, 0);
}

static inline long mos_get_metadata(const char* path, syscall_file_metadata_t* out) {
    return mos_syscall(SYS_GET_METADATA, (long)(uintptr_t)path, (long)(uintptr_t)out, 0);
}

static inline long mos_tell(int fd) {
    return mos_syscall(SYS_TELL, fd, 0, 0);
}

static inline long mos_feof(int fd) {
    return mos_syscall(SYS_EOF, fd, 0, 0);
}

// --- MPKG (.mpkg archive) ---

static inline long mos_pkg_unzip(const char* archive_path, const char* target_dir,
                                 uint32_t* out_installed, uint32_t* out_failed) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_UNZIP;
    request.path = archive_path;
    request.extra = target_dir;
    request.out_count = out_installed;
    request.out_failed = out_failed;
    return mos_syscall(SYS_PKG, (long)(uintptr_t)&request, 0, 0);
}

static inline long mos_pkg_zip(const char* source_path, const char* algorithm,
                               char* out_path, uint32_t out_path_size) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_ZIP;
    request.path = source_path;
    request.extra = algorithm;
    request.out_path = out_path;
    request.out_path_size = out_path_size;
    return mos_syscall(SYS_PKG, (long)(uintptr_t)&request, 0, 0);
}

static inline long mos_pkg_info(const char* archive_path, uint32_t* out_entry_count) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_INFO;
    request.path = archive_path;
    request.out_count = out_entry_count;
    return mos_syscall(SYS_PKG, (long)(uintptr_t)&request, 0, 0);
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