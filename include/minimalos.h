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

// --- Clocks, stat, rename, working directory, spawn/wait (OS-0 calls) ---
// process.h builds a friendlier API on these. All of them take a plain
// path ("0:/dir/file", "/dir/file", or relative to the working directory)
// and return SYS_SUCCESS (0) or a negative SYS_ERR_* value, except where
// noted.

// Nanoseconds since boot; never goes backwards.
static inline uint64_t mos_clock_monotonic_ns(void) {
    return (uint64_t)mos_syscall(SYS_CLOCK, SYS_CLOCK_MONOTONIC, 0, 0);
}

// Nanoseconds since 1970-01-01 UTC by the system clock (millisecond steps;
// jumps when the clock is set).
static inline uint64_t mos_clock_realtime_ns(void) {
    return (uint64_t)mos_syscall(SYS_CLOCK, SYS_CLOCK_REALTIME, 0, 0);
}

// RDTSC ticks per second when the TSC is a usable clock in user code
// (invariant TSC), else 0.
static inline uint64_t mos_clock_tsc_hz(void) {
    return (uint64_t)mos_syscall(SYS_CLOCK, SYS_CLOCK_TSC_HZ, 0, 0);
}

// Size, type and times of a file or directory. SYS_ERR_NOTFOUND when
// nothing is at `path`.
static inline long mos_stat(const char* path, syscall_stat_t* out) {
    return mos_syscall(SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)out, 0);
}

// Moves/renames a file or directory on one drive. Replaces an existing
// file at `to`; a directory there is an error. SYS_ERR_NOTFOUND when
// `from` does not exist, SYS_ERR_GENERIC for any other refusal.
static inline long mos_rename(const char* from, const char* to) {
    return mos_syscall(SYS_RENAME, (long)(uintptr_t)from, (long)(uintptr_t)to, 0);
}

// Sets the program's working directory (shared by its threads, inherited
// by programs it starts). SYS_ERR_NOTFOUND if `path` is not a directory.
static inline long mos_chdir(const char* path) {
    return mos_syscall(SYS_CHDIR, (long)(uintptr_t)path, 0, 0);
}

// Starts a program as a child of the caller. `request->size` must be
// sizeof(syscall_spawn_t) (process.h's mos_spawn_ex() fills it in).
// Returns the child's pid (> 0) or a negative SYS_ERR_* value.
static inline long mos_spawn_raw(const syscall_spawn_t* request) {
    return mos_syscall(SYS_SPAWN, (long)(uintptr_t)request, 0, 0);
}

// Waits for a child (pid, or SYS_WAIT_ANY) to end; returns its pid.
// `flags` is 0 or SYS_WAIT_NOHANG. `out` may be NULL.
static inline long mos_wait_raw(long pid, long flags, syscall_wait_t* out) {
    return mos_syscall(SYS_WAIT, pid, flags, (long)(uintptr_t)out);
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

// The kernel log: serial and, on a netbooted machine, 'make netboot' on
// the build PC. Not shown on screen. One call is one line.
#define MOS_FD_LOG 3
static inline long mos_log(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return mos_write(MOS_FD_LOG, str, len);
}

#endif // MINIMALOS_SDK_H