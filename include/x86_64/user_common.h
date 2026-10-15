#ifndef MINIMALOS_X86_64_USER_COMMON_H
#define MINIMALOS_X86_64_USER_COMMON_H

/*
 * Wrappers shared by the two header families.
 *
 * minimalos.h (with stdio.h, stdlib.h, window.h, net.h) and the
 * x86_64/user_*.h headers behind graphics.h used to define these
 * functions separately, with slightly different signatures, so a
 * program could not include both (graphics.h together with stdio.h,
 * for example). Each wrapper now has exactly one definition, here, and
 * both families include this header.
 *
 * Two of them had genuinely different call forms, and both still work:
 *
 *   mos_write(fd, buf, len)   or  mos_write(text, len)
 *   mos_exit(code)            or  mos_exit()          (exit code 0)
 */

#include <stddef.h>
#include <stdint.h>
#include "syscall.h"

static inline long mos_syscall(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory", "cc"
    );
    return ret;
}

// --- Terminal output and process control ---

static inline long mos_write_fd(int fd, const void* buf, size_t len) {
    return mos_syscall(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)len);
}

static inline long mos_write_text(const char* text, uint64_t length) {
    return mos_write_fd(1, text, (size_t)length);
}

// mos_write(fd, buf, len) writes to fd 1/2; mos_write(text, len) writes to stdout.
#define MOS_WRITE_SELECT_(a, b, c, name, ...) name
#define mos_write(...) \
    MOS_WRITE_SELECT_(__VA_ARGS__, mos_write_fd, mos_write_text, mos_write_fd)(__VA_ARGS__)

__attribute__((noreturn)) static inline void mos_exit_with(int code) {
    mos_syscall(SYS_EXIT, code, 0, 0);
    for (;;) { } // never reached - SYS_EXIT never returns
}

// mos_exit(code), or mos_exit() for code 0.
#define mos_exit(...) mos_exit_with(__VA_ARGS__ + 0)

static inline long mos_getpid(void) {
    return mos_syscall(SYS_GETPID, 0, 0, 0);
}

static inline long mos_uptime(void) {
    return mos_syscall(SYS_UPTIME, 0, 0, 0);
}

// Sleeps for `ms` milliseconds.
static inline long mos_sleep(long ms) {
    return mos_syscall(SYS_SLEEP, ms, 0, 0);
}

static inline long mos_register_cleanup(void (*cleanup)(void)) {
    return mos_syscall(SYS_REGISTER_CLEANUP, (long)(uintptr_t)cleanup, 0, 0);
}

// --- MinimaFS extensions ---

// Write to a file opened with mos_open(). Distinct from mos_write(),
// which is fixed to fd 1/2 (stdout/stderr -> terminal).
static inline long mos_fwrite(long fd, const void* buf, size_t len) {
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

static inline long mos_tell(long fd) {
    return mos_syscall(SYS_TELL, fd, 0, 0);
}

static inline long mos_feof(long fd) {
    return mos_syscall(SYS_EOF, fd, 0, 0);
}

// --- MPKG (.mpkg archive) ---

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
    return mos_syscall(SYS_PKG, (long)(uintptr_t)&request, 0, 0);
}

// Archives `source_path` (a file or directory) as "<source_path>.mpkg".
// `algorithm` is "lzss" (pass NULL for the default) or "store".
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

// Reports the entry count of the .mpkg archive at `archive_path`.
static inline long mos_pkg_info(const char* archive_path, uint32_t* out_entry_count) {
    syscall_pkg_request_t request = {0};
    request.op = SYS_PKG_INFO;
    request.path = archive_path;
    request.out_count = out_entry_count;
    return mos_syscall(SYS_PKG, (long)(uintptr_t)&request, 0, 0);
}

// --- Framebuffer ownership and blitting (SYS_GRAPHICS) ---

static inline long mos_graphics_call(syscall_graphics_request_t* request) {
    return mos_syscall(SYS_GRAPHICS, (long)(uintptr_t)request, 0, 0);
}

static inline long mos_graphics_acquire(void) {
    syscall_graphics_request_t request = {0};
    request.op = SYS_GRAPHICS_ACQUIRE;
    return mos_graphics_call(&request);
}

static inline long mos_graphics_release(void) {
    syscall_graphics_request_t request = {0};
    request.op = SYS_GRAPHICS_RELEASE;
    return mos_graphics_call(&request);
}

static inline long mos_graphics_info(syscall_graphics_info_t* info) {
    syscall_graphics_request_t request = {0};
    request.op = SYS_GRAPHICS_INFO;
    request.info = info;
    if (info) {
        info->size = sizeof(*info);
        info->version = SYS_GRAPHICS_ABI_VERSION;
        info->reserved = 0;
    }
    return mos_graphics_call(&request);
}

static inline long mos_graphics_blit_surface(
        const uint32_t* src, uint32_t width, uint32_t height,
        uint32_t pitch, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
        int32_t dx, int32_t dy, uint32_t flags) {
    syscall_blit_request_t blit = {0};
    blit.size = sizeof(blit);
    blit.version = SYS_GRAPHICS_ABI_VERSION;
    blit.flags = flags;
    blit.src_w = width;
    blit.src_h = height;
    blit.src_pitch = pitch;
    blit.src = src;
    blit.sx = sx;
    blit.sy = sy;
    blit.sw = sw;
    blit.sh = sh;
    blit.dx = dx;
    blit.dy = dy;
    syscall_graphics_request_t request = {0};
    request.op = SYS_GRAPHICS_BLIT;
    request.blit = &blit;
    return mos_graphics_call(&request);
}

static inline long mos_graphics_present(int32_t x, int32_t y,
                                        int32_t width, int32_t height) {
    syscall_graphics_request_t request = {0};
    request.op = SYS_GRAPHICS_PRESENT;
    request.args[0] = x;
    request.args[1] = y;
    request.args[2] = width;
    request.args[3] = height;
    return mos_graphics_call(&request);
}

#endif // MINIMALOS_X86_64_USER_COMMON_H
