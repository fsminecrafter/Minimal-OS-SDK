#ifndef MINIMALOS_X86_64_USER_MINIMAFS_H
#define MINIMALOS_X86_64_USER_MINIMAFS_H

#include <stdint.h>
#include "x86_64/user_syscalls.h"

// Write to a MinimaFS file handle previously returned by mos_open().
// Distinct from mos_write(), which is fixed to fd 1/2 (stdout/stderr,
// routed to the terminal) - this is the file-I/O counterpart to
// mos_read().
static inline long mos_fwrite(long fd, const void* buf, uint64_t len) {
    return (long)mos_syscall3(SYS_FWRITE, (uint64_t)fd,
                             (uint64_t)(uintptr_t)buf, len);
}

// Lists up to `max_entries` entries of the directory at `path` into
// `entries`. Returns the number of entries written, or a negative
// SYS_ERR_* value.
static inline long mos_listdir(const char* path, syscall_dirent_t* entries,
                               uint32_t max_entries) {
    return (long)mos_syscall3(SYS_LISTDIR, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)entries, max_entries);
}

static inline long mos_delete(const char* path) {
    return (long)mos_syscall1(SYS_DELETE, (uint64_t)(uintptr_t)path);
}

static inline long mos_rmdir(const char* path) {
    return (long)mos_syscall1(SYS_RMDIR, (uint64_t)(uintptr_t)path);
}

static inline long mos_get_metadata(const char* path, syscall_file_metadata_t* out) {
    return (long)mos_syscall3(SYS_GET_METADATA, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)out, 0);
}

static inline long mos_tell(long fd) {
    return (long)mos_syscall1(SYS_TELL, (uint64_t)fd);
}

static inline long mos_feof(long fd) {
    return (long)mos_syscall1(SYS_EOF, (uint64_t)fd);
}

#endif