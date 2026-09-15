#ifndef MINIMALOS_X86_64_USER_PKG_H
#define MINIMALOS_X86_64_USER_PKG_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

// Extracts the .mpkg archive at `archive_path` into `target_dir`.
// *out_installed/*out_failed (either may be NULL) receive per-entry
// counts on return.
static inline long mos_pkg_unzip(const char* archive_path, const char* target_dir,
                                 uint32_t* out_installed, uint32_t* out_failed) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_UNZIP;
    request.path = archive_path;
    request.extra = target_dir;
    request.out_count = out_installed;
    request.out_failed = out_failed;
    return (long)mos_syscall1(SYS_PKG, (uint64_t)(uintptr_t)&request);
}

// Archives `source_path` (a file or directory) as
// "<source_path>.mpkg". `algorithm` is "lzss" (pass NULL for the
// default) or "store". If `out_path` is non-NULL, the produced
// archive's path is copied into it (up to out_path_size bytes).
static inline long mos_pkg_zip(const char* source_path, const char* algorithm,
                               char* out_path, uint32_t out_path_size) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_ZIP;
    request.path = source_path;
    request.extra = algorithm;
    request.out_path = out_path;
    request.out_path_size = out_path_size;
    return (long)mos_syscall1(SYS_PKG, (uint64_t)(uintptr_t)&request);
}

// Reports the entry count of the .mpkg archive at `archive_path`.
static inline long mos_pkg_info(const char* archive_path, uint32_t* out_entry_count) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_INFO;
    request.path = archive_path;
    request.out_count = out_entry_count;
    return (long)mos_syscall1(SYS_PKG, (uint64_t)(uintptr_t)&request);
}

#endif